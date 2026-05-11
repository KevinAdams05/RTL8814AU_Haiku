/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Firmware.cpp — RTL8814AU firmware loading implementation.
 *
 * The RTL8814AU contains a Lexra 3081 MIPS-derived MCU (not the 8051 found
 * in older Realtek chips). This requires a fundamentally different firmware
 * loading mechanism: beacon-queue TX + IDDMA (Internal Data DMA).
 *
 * Loading sequence:
 *   1. Read firmware binary from filesystem
 *   2. Validate the 64-byte header (signature 0x8814, DMEM/IRAM sizes)
 *   3. Enable firmware download mode (set MCUFWDL_EN)
 *   4. Halt the MCU via REG_SYS_FUNC_EN bit 12 (kSysFuncEnCpuEn)
 *   5. Reset the DDMA engine
 *   6. Configure the beacon queue for reserved-page submission
 *   7. For each DMEM chunk:
 *        a. Submit chunk as beacon-queue TX packet (QSEL = 0x10)
 *        b. Wait for BcnValid bit (REG_FIFOPAGE_CTRL_2 + 1, bit 7)
 *        c. IDDMA from beacon's location in TX packet buffer to OCP DMEM
 *   8. Repeat (7) for IRAM chunks, targeting OCP IRAM
 *   9. Resume the MCU via REG_SYS_FUNC_EN bit 12
 *  10. Disable download mode
 *  11. Poll CPU_DL_READY (bit 15 of REG_MCUFWDL) — 5 second timeout
 *
 * Why beacon-queue?
 *   The 8051-style control-transfer write to 0x1000 permanently locks the
 *   0x1200+ DDMA register window on the 3081-MCU chip: after any such write
 *   all reads of the DDMA registers return 0xffffffff and all writes
 *   time out.  The reference driver avoids this by submitting firmware as
 *   a TX packet on the beacon bulk OUT endpoint — the chip accepts it via
 *   the normal TX path and IDDMA then reads from the beacon's offset in
 *   the TX packet buffer.
 *
 * Reference: rtl8814a_hal_init.c — HalROMDownloadFWRSVDPage8814A(),
 *            SetDownLoadFwRsvdPagePkt_8814A(), WaitDownLoadRSVDPageOK_3081(),
 *            IDDMADownLoadFW_3081(), _3081Disable8814A(), _3081Enable8814A(),
 *            _FWFreeToGo8814A() in zebulon2/rtl8814au.
 */

#include "Firmware.h"

#include <fcntl.h>
#include <new>
#include <string.h>
#include <unistd.h>

#include <ByteOrder.h>
#include <KernelExport.h>
#include <OS.h>

#include "RegisterIO.h"
#include "TxPath.h"


RTL8814AUFirmware::RTL8814AUFirmware(RTL8814AURegisterIO* registerIO)
	:
	fRegisterIO(registerIO),
	fTxPath(NULL),
	fData(NULL),
	fDataSize(0),
	fLoaded(false),
	fVersion(0),
	fDmemOffset(0),
	fDmemSize(0),
	fIramOffset(0),
	fIramSize(0)
{
}


RTL8814AUFirmware::~RTL8814AUFirmware()
{
	delete[] fData;
}


// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------


/*! Load firmware from the given path, validate, and transfer to chip.
    \param firmwarePath  Absolute path to the firmware binary
    \return B_OK on success, or an error code describing what failed.
*/
status_t
RTL8814AUFirmware::Load(const char* firmwarePath)
{
	if (fLoaded)
		return B_OK;

	if (fTxPath == NULL) {
		dprintf(RTL8814AU_DRIVER_NAME ": firmware load requested without "
			"TxPath — call SetTxPath() first\n");
		return B_NO_INIT;
	}

	// Saved register values preserved for restoration on cleanup paths.
	uint8 savedCR1 = 0;
	uint8 savedTxqCtrl2 = 0;

	dprintf(RTL8814AU_DRIVER_NAME ": loading firmware from %s\n",
		firmwarePath);

	// Step 1: Read the firmware file into memory
	status_t status = _ReadFile(firmwarePath);
	if (status != B_OK)
		return status;

	// Step 2: Validate the 64-byte header and parse section sizes
	status = _ValidateHeader();
	if (status != B_OK) {
		delete[] fData;
		fData = NULL;
		return status;
	}

	dprintf(RTL8814AU_DRIVER_NAME ": firmware version %u, "
		"DMEM %" B_PRIu32 " bytes, IRAM %" B_PRIu32 " bytes\n",
		fVersion, fDmemSize, fIramSize);

	// Step 3: Enable download mode (sets MCUFWDL_EN)
	status = _EnableDownloadMode();
	if (status != B_OK)
		goto cleanup;

	// Step 4: Halt the Lexra 3081 MCU so we can write to its memory.
	// Uses REG_SYS_FUNC_EN bit 12, NOT the 8051 reset bit in REG_MCUFWDL.
	_HaltMCU();

	// Step 5: Reset the DDMA engine to clear any stale state
	_ResetDDMA();

	// Step 5b: Enable the TX DMA subsystem so the DDMA registers (0x1200+)
	// are accessible.  The reference driver sets REG_CR byte 1 bit 0
	// ("SW beacon DMA enable") in HalROMDownloadFWRSVDPage8814A().
	savedCR1 = fRegisterIO->Read8(kRegCR + 1);
	fRegisterIO->Write8(kRegCR + 1, savedCR1 | 0x01);

	// Step 5c: Save REG_FWHW_TXQ_CTRL + 2 so we can restore bit 6
	// after firmware download.
	savedTxqCtrl2 = fRegisterIO->Read8(kRegFwhwTxqCtrl + 2);

	dprintf(RTL8814AU_DRIVER_NAME ": TX DMA enabled for IDDMA "
		"(CR+1: 0x%02x -> 0x%02x, DDMA_CH0_CTRL=0x%08" B_PRIx32 ")\n",
		savedCR1, savedCR1 | 0x01,
		fRegisterIO->Read32(kRegDDMACh0Ctrl));

	// Step 6: Configure the beacon queue for reserved-page submission.
	status = _PrepareBeaconQueue();
	if (status != B_OK)
		goto cleanup_resume;

	// Step 6b: Clear any ENDPOINT_HALT condition on the beacon bulk OUT
	// pipe.  If the pipe is stalled, no bulk OUT data will ever drain;
	// firmware TX would hang waiting for the chip to pull from EP2.
	fTxPath->ClearBeaconPipeHalt();

	// Step 7: Transfer DMEM section — submit chunks as beacon-queue TX,
	// then IDDMA from the beacon's TX packet buffer offset to OCP DMEM.
	dprintf(RTL8814AU_DRIVER_NAME ": transferring DMEM "
		"(%" B_PRIu32 " bytes)\n", fDmemSize);

	status = _TransferSection(fData + fDmemOffset, fDmemSize,
		kOcpBaseDMem, true);
	if (status != B_OK)
		goto cleanup_resume;

	// Step 8: Transfer IRAM section — same pattern, target OCP IRAM.
	dprintf(RTL8814AU_DRIVER_NAME ": transferring IRAM "
		"(%" B_PRIu32 " bytes)\n", fIramSize);

	status = _TransferSection(fData + fIramOffset, fIramSize,
		kOcpBaseIMem, true);
	if (status != B_OK)
		goto cleanup_resume;

	// Step 8b: If both DMEM and IRAM checksums passed, set bit 6 of
	// byte 1 at REG_8051FW_CTRL (0x0081) to signal the MCU that firmware
	// is ready.  This matches the end of HalROMDownloadFWRSVDPage8814A().
	{
		uint8 fwCtrl0 = fRegisterIO->Read8(kRegMcuFwDl);
		if ((fwCtrl0 & kDmemChksumOk) && (fwCtrl0 & kImemChksumOk)) {
			uint8 fwCtrl1 = fRegisterIO->Read8(kRegMcuFwDl + 1);
			fRegisterIO->Write8(kRegMcuFwDl + 1, fwCtrl1 | 0x40);
			dprintf(RTL8814AU_DRIVER_NAME ": both checksums OK, "
				"firmware ready flag set\n");
		} else {
			dprintf(RTL8814AU_DRIVER_NAME ": WARNING: checksum flags "
				"not both set (byte0=0x%02x)\n", fwCtrl0);
		}
	}

	// Step 8c: Restore REG_FWHW_TXQ_CTRL + 2 and CR byte 1 to their
	// pre-download values.
	fRegisterIO->Write8(kRegFwhwTxqCtrl + 2, savedTxqCtrl2);
	fRegisterIO->Write8(kRegCR + 1, savedCR1);

	// Step 9: Resume the MCU — firmware will begin executing
	_ResumeMCU();

	// Step 10: Disable download mode
	status = _DisableDownloadMode();
	if (status != B_OK)
		goto cleanup;

	// Step 11: Poll until MCU signals CPU_DL_READY (bit 15)
	status = _PollForReady();
	if (status != B_OK)
		goto cleanup;

	fLoaded = true;
	dprintf(RTL8814AU_DRIVER_NAME ": firmware loaded successfully\n");

	// Free the file data — it's now in chip memory
	delete[] fData;
	fData = NULL;
	fDataSize = 0;

	return B_OK;

cleanup_resume:
	// If we failed during transfer, restore saved registers and resume
	// the MCU before cleaning up.
	fRegisterIO->Write8(kRegFwhwTxqCtrl + 2, savedTxqCtrl2);
	fRegisterIO->Write8(kRegCR + 1, savedCR1);
	_ResumeMCU();

cleanup:
	dprintf(RTL8814AU_DRIVER_NAME ": firmware load failed: %s\n",
		strerror(status));
	_DisableDownloadMode();
	delete[] fData;
	fData = NULL;
	fDataSize = 0;
	return status;
}


// ---------------------------------------------------------------------------
// Internal implementation
// ---------------------------------------------------------------------------


/*! Read the firmware binary file into fData/fDataSize. */
status_t
RTL8814AUFirmware::_ReadFile(const char* path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		dprintf(RTL8814AU_DRIVER_NAME ": cannot open firmware file: %s\n",
			path);
		return B_ENTRY_NOT_FOUND;
	}

	// Get file size
	off_t fileSize = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);

	if (fileSize <= 0 || (uint32)fileSize > kMaxFirmwareSize) {
		dprintf(RTL8814AU_DRIVER_NAME ": firmware file size invalid: "
			"%" B_PRIdOFF " bytes (max %" B_PRIu32 ")\n",
			fileSize, kMaxFirmwareSize);
		close(fd);
		return B_BAD_DATA;
	}

	fData = new(std::nothrow) uint8[fileSize];
	if (fData == NULL) {
		close(fd);
		return B_NO_MEMORY;
	}

	ssize_t bytesRead = read(fd, fData, fileSize);
	close(fd);

	if (bytesRead != fileSize) {
		dprintf(RTL8814AU_DRIVER_NAME ": short read: expected "
			"%" B_PRIdOFF ", got %zd\n", fileSize, bytesRead);
		delete[] fData;
		fData = NULL;
		return B_IO_ERROR;
	}

	fDataSize = (uint32)fileSize;
	return B_OK;
}


/*! Validate the 64-byte firmware header and parse DMEM/IRAM section info.

    The firmware binary layout:
      [64-byte header] [DMEM section] [IRAM section]
    DMEM starts immediately after the header. IRAM starts at the end of
    the file minus iramSize. Each section includes 4 extra bytes for
    checksum purposes appended by the build tools.
*/
status_t
RTL8814AUFirmware::_ValidateHeader()
{
	if (fDataSize < kFwHeaderSize) {
		dprintf(RTL8814AU_DRIVER_NAME ": firmware too small for header "
			"(%" B_PRIu32 " < %" B_PRIu32 ")\n", fDataSize, kFwHeaderSize);
		return B_BAD_DATA;
	}

	const RTL8814AUFirmwareHeader* header
		= reinterpret_cast<const RTL8814AUFirmwareHeader*>(fData);

	// Check signature — expect 0x8814 for RTL8814AU firmware
	uint16 signature = B_LENDIAN_TO_HOST_INT16(header->signature);
	if (signature != kRTL8814AU_ChipID) {
		dprintf(RTL8814AU_DRIVER_NAME ": bad firmware signature: 0x%04x "
			"(expected 0x%04x)\n", signature, kRTL8814AU_ChipID);
		return B_BAD_DATA;
	}

	fVersion = B_LENDIAN_TO_HOST_INT16(header->version);

	// Extract section sizes from the header.
	// The reference driver reads these via GET_FIRMWARE_HDR_TOTAL_DMEM_SZ_3081
	// (offset 36) and GET_FIRMWARE_HDR_IRAM_SZ_3081 (offset 48).
	uint32 dmemSize = B_LENDIAN_TO_HOST_INT32(header->totalDmemSize);
	uint32 iramSize = B_LENDIAN_TO_HOST_INT32(header->iramSize);

	dprintf(RTL8814AU_DRIVER_NAME ": header: sig=0x%04x ver=%u "
		"dmem=%" B_PRIu32 " iram=%" B_PRIu32 "\n",
		signature, fVersion, dmemSize, iramSize);

	// Sanity check: sections must fit within the file
	if (dmemSize == 0 && iramSize == 0) {
		dprintf(RTL8814AU_DRIVER_NAME ": firmware has no payload sections\n");
		return B_BAD_DATA;
	}

	if (kFwHeaderSize + dmemSize + iramSize > fDataSize) {
		dprintf(RTL8814AU_DRIVER_NAME ": firmware sections exceed file size "
			"(header %" B_PRIu32 " + dmem %" B_PRIu32 " + iram %" B_PRIu32
			" > file %" B_PRIu32 ")\n",
			kFwHeaderSize, dmemSize, iramSize, fDataSize);
		return B_BAD_DATA;
	}

	// DMEM immediately follows the 64-byte header and is followed by
	// an 8-byte XOR trailer.  IRAM is preceded by its own 8-byte XOR
	// trailer and ends at the file boundary.  Each trailer's u16 XOR
	// equals the section's u16 XOR, so transferring data+trailer to
	// the MCU yields a running checksum of 0 — required for the DDMA
	// engine to clear CHKSUM_FAIL.
	//
	// File layout:
	//   [0..64)        64-byte file header
	//   [64..h+dmem)   DMEM data
	//   [h+dmem..+8)   DMEM 8-byte trailer
	//   [next..+8)     IRAM 8-byte trailer
	//   [...end)       IRAM data
	//
	// Transferred section sizes are dmemSize+8 and iramSize+8.  The
	// DMEM trailer lands at DMEM[dmemSize..+8] (unused tail of MCU
	// data memory).  The IRAM trailer lands at IRAM[0..8]; its bytes
	// 0x00801800 0x00000000 decode as MIPS `sll $v1,$a0,0; nop`, so
	// the MCU's PC=0 entry harmlessly falls through to real code at
	// IRAM[8].
	const uint32 kSectionTrailerSize = 8;
	fDmemOffset = kFwHeaderSize;
	fDmemSize = dmemSize + kSectionTrailerSize;
	fIramSize = iramSize + kSectionTrailerSize;
	fIramOffset = fDataSize - fIramSize;

	return B_OK;
}


/*! Enable firmware download mode: set the download enable bit and configure
    the control register for firmware transfer.

    The reference driver's _FWDownloadEnable_8814A(enable=TRUE) does a 16-bit
    read/write to REG_8051FW_CTRL_8814A (0x0080), clearing most bits and
    setting only bit 0 (MCUFWDL_EN) and bit 13 (FWDL_DISABLE_SIM).
    This also clears CPU_DL_READY (bit 15) so it can be polled after loading.
*/
status_t
RTL8814AUFirmware::_EnableDownloadMode()
{
	// Read the 16-bit control register (bytes 0-1 at 0x0080-0x0081).
	// The reference driver clears all bits except 12-13, then clears 12
	// and sets 13, plus sets bit 0 for download enable.
	uint16 ctrl = fRegisterIO->Read16(kRegMcuFwDl);

	dprintf(RTL8814AU_DRIVER_NAME ": enable download mode "
		"(REG_MCUFWDL before=0x%04x)\n", ctrl);

	ctrl &= 0x3000;			// Keep only bits 12-13
	ctrl &= ~(uint16)0x1000;	// Clear bit 12 (FWDL_CHKSUM_EN)
	ctrl |= (uint16)0x2000;	// Set bit 13 (FWDL_DISABLE_SIM)
	ctrl |= (uint16)0x0001;	// Set bit 0 (MCUFWDL_EN)

	status_t status = fRegisterIO->Write16(kRegMcuFwDl, ctrl);
	if (status != B_OK)
		return status;

	// Verify the write took effect
	uint16 verify = fRegisterIO->Read16(kRegMcuFwDl);
	dprintf(RTL8814AU_DRIVER_NAME ": enable download mode "
		"(REG_MCUFWDL after=0x%04x)\n", verify);

	return B_OK;
}


/*! Disable firmware download mode after transfer is complete.

    Matches _FWDownloadEnable_8814A(enable=false) in the reference driver.
*/
status_t
RTL8814AUFirmware::_DisableDownloadMode()
{
	// Clear firmware download enable (bit 0 of byte 0)
	uint8 ctrl = fRegisterIO->Read8(kRegMcuFwDl);
	ctrl &= ~(uint8)kMcuFwDlEn;
	return fRegisterIO->Write8(kRegMcuFwDl, ctrl);
}


/*! Halt the Lexra 3081 MCU by clearing bit 12 (FEN_CPUEN) of
    REG_SYS_FUNC_EN. Uses an 8-bit write to byte 1 of the 16-bit register
    to avoid disturbing other enable bits.

    Matches _3081Disable8814A() in the reference driver.
*/
void
RTL8814AUFirmware::_HaltMCU()
{
	// Bit 10 of REG_SYS_FUNC_EN = BIT2 of byte at REG_SYS_FUNC_EN + 1.
	// Matches _3081Disable8814A(): u1bTmp & (~BIT2)
	uint8 byte1 = fRegisterIO->Read8(kRegSysFuncEn + 1);
	byte1 &= ~(uint8)0x04;		// Clear BIT2 (= bit 10 of 16-bit reg)
	fRegisterIO->Write8(kRegSysFuncEn + 1, byte1);

	dprintf(RTL8814AU_DRIVER_NAME ": MCU halted (SYS_FUNC_EN bit 10 cleared)\n");
}


/*! Resume the Lexra 3081 MCU by setting bit 12 (FEN_CPUEN) of
    REG_SYS_FUNC_EN. The MCU begins executing the firmware loaded into
    DMEM and IRAM.

    Matches _3081Enable8814A() in the reference driver.
*/
void
RTL8814AUFirmware::_ResumeMCU()
{
	// Bit 10 of REG_SYS_FUNC_EN = BIT2 of byte at REG_SYS_FUNC_EN + 1.
	// Matches _3081Enable8814A(): u1bTmp | BIT2
	uint8 byte1 = fRegisterIO->Read8(kRegSysFuncEn + 1);
	byte1 |= (uint8)0x04;		// Set BIT2 (= bit 10 of 16-bit reg)
	fRegisterIO->Write8(kRegSysFuncEn + 1, byte1);

	dprintf(RTL8814AU_DRIVER_NAME ": MCU resumed (SYS_FUNC_EN bit 10 set)\n");
}


/*! Reset the DDMA engine by toggling bit 16 of REG_CPU_DMEM_CON_8814A.
    This clears any stale DMA state from a previous firmware load attempt.

    IMPORTANT: The reference driver's sequence is CLEAR bit 16 first, then
    SET bit 16.  Bit 16 must be LEFT SET for the DDMA registers (0x1200+) to
    be accessible.  Previous versions had this inverted, leaving DDMA in
    reset and causing reads of 0xeaeaeaea from the DDMA control register.
*/
void
RTL8814AUFirmware::_ResetDDMA()
{
	uint32 val = fRegisterIO->Read32(kRegCpuDmemCon);

	dprintf(RTL8814AU_DRIVER_NAME ": DDMA reset "
		"(REG_CPU_DMEM_CON before=0x%08" B_PRIx32 ")\n", val);

	// Clear bit 16 first (assert reset), then set it (release reset).
	// Matches the reference driver: clear → set, leaving DDMA active.
	fRegisterIO->Write32(kRegCpuDmemCon, val & ~(1 << 16));
	snooze(100);
	fRegisterIO->Write32(kRegCpuDmemCon, val | (1 << 16));

	// Verify DDMA registers are now accessible (should NOT be 0xeaeaeaea)
	uint32 ddmaCtrl = fRegisterIO->Read32(kRegDDMACh0Ctrl);
	dprintf(RTL8814AU_DRIVER_NAME ": DDMA engine reset "
		"(DDMA_CH0_CTRL=0x%08" B_PRIx32 ")\n", ddmaCtrl);
}


/*! Configure the beacon queue so firmware chunks submitted on the beacon
    bulk OUT endpoint land in the reserved-page area of the TX packet
    buffer instead of being transmitted as real beacons.

    Matches the preamble of HalROMDownloadFWRSVDPage8814A():
      - Set REG_CR + 1 bit 0 (SW beacon DMA)            [done in Load()]
      - Clear BCN_CTRL bit 3, set bit 4                  (disable beacon TX)
      - Clear REG_FWHW_TXQ_CTRL + 2 bit 6                ("not a real beacon")
      - Program beacon page boundary into REG_FIFOPAGE_CTRL_2
      - Clear BcnValid (bit 7 of REG_FIFOPAGE_CTRL_2+1)  (via write-1)
*/
status_t
RTL8814AUFirmware::_PrepareBeaconQueue()
{
	// Disable real beacon transmission: BCN_CTRL bit 3 (DIS_TSF_UDT),
	// bit 4 controls beacon processing.  Match SetBcnCtrlReg semantics:
	// clear BIT3 and set BIT4.
	uint8 bcnCtrl = fRegisterIO->Read8(kRegBcnCtrl);
	bcnCtrl &= ~(uint8)(1 << 3);
	bcnCtrl |= (uint8)(1 << 4);
	fRegisterIO->Write8(kRegBcnCtrl, bcnCtrl);

	// Mark "not a real beacon" so the chip doesn't try to transmit the
	// firmware packet: clear bit 6 of REG_FWHW_TXQ_CTRL + 2.
	uint8 txqCtrl2 = fRegisterIO->Read8(kRegFwhwTxqCtrl + 2);
	txqCtrl2 &= ~(uint8)(1 << 6);
	fRegisterIO->Write8(kRegFwhwTxqCtrl + 2, txqCtrl2);

	// Program the beacon-queue page boundary.  The firmware chunk will
	// land at OCPBASE_TXBUF_3081 + (bndy * 128) + 40 (after the TX desc).
	fRegisterIO->Write16(kRegFIFOPage, kFwTxPktBufBoundary);

	// Clear any stale BcnValid ack (bit 7 of REG_FIFOPAGE_CTRL_2 + 1)
	// by writing 1 to it — standard write-1-to-clear convention.
	uint8 bcnValid = fRegisterIO->Read8(kRegFIFOPage + 1);
	fRegisterIO->Write8(kRegFIFOPage + 1, bcnValid | kBcnValidBit);

	dprintf(RTL8814AU_DRIVER_NAME ": beacon queue prepared "
		"(bndy=0x%04x, BCN_CTRL=0x%02x, FWHW_TXQ+2=0x%02x)\n",
		kFwTxPktBufBoundary, bcnCtrl, txqCtrl2);

	return B_OK;
}


/*! Poll REG_FIFOPAGE_CTRL_2 + 1 bit 7 (BcnValid) until the chip sets it,
    indicating it has accepted a firmware chunk into the TX packet buffer.

    Matches WaitDownLoadRSVDPageOK_3081() in the reference driver:
    200 attempts × 50 µs = 10 ms total.  The ack bit is then cleared by
    writing 1 to it, ready for the next chunk.
*/
status_t
RTL8814AUFirmware::_WaitForRsvdPageOK()
{
	for (uint32 attempt = 0; attempt < kBcnValidPollAttempts; attempt++) {
		uint8 reg = fRegisterIO->Read8(kRegFIFOPage + 1);
		if (reg & kBcnValidBit) {
			// Clear the ack so the next chunk can re-arm it.
			fRegisterIO->Write8(kRegFIFOPage + 1, reg | kBcnValidBit);
			return B_OK;
		}
		snooze(kBcnValidPollDelay);
	}

	dprintf(RTL8814AU_DRIVER_NAME ": BcnValid poll timed out "
		"(REG_FIFOPAGE_CTRL_2+1=0x%02x)\n",
		fRegisterIO->Read8(kRegFIFOPage + 1));
	return B_TIMED_OUT;
}


/*! Transfer a firmware section (DMEM or IRAM) to the chip via beacon-queue
    TX packets and IDDMA.

    For each chunk:
      1. SendFirmwareChunk() submits the chunk as a beacon-queue TX packet
         on bulk OUT pipe 2 (QSEL = 0x10).
      2. The chip parks the chunk in the TX packet buffer at offset
         (kFwTxPktBufBoundary * 128) and sets BcnValid when done.
      3. We wait for BcnValid via _WaitForRsvdPageOK().
      4. IDDMA copies the chunk from its TX-buffer location (skipping the
         40-byte TX descriptor) to the target OCP address.

    \param data           Pointer to the section data
    \param size           Section size in bytes
    \param ocpDestAddr    OCP base address (kOcpBaseDMem or kOcpBaseIMem)
    \param resetChecksum  True to reset the DDMA checksum accumulator

    Matches the per-chunk loop inside HalROMDownloadFWRSVDPage8814A().
*/
status_t
RTL8814AUFirmware::_TransferSection(const uint8* data, uint32 size,
	uint32 ocpDestAddr, bool resetChecksum)
{
	if (size == 0)
		return B_OK;

	// Source address for IDDMA: the beacon-queue slot in TX packet buffer,
	// offset past the 40-byte TX descriptor we prepended.
	const uint32 iddmaSrc = kOcpBaseTxBuf
		+ (uint32)kFwTxPktBufBoundary * kFwTxBufPageSize
		+ kFwTxDescOffset;

	uint32 offset = 0;

	while (offset < size) {
		uint32 chunkSize = size - offset;
		if (chunkSize > kFwPageSize)
			chunkSize = kFwPageSize;

		// Step 1: Submit the chunk as a beacon-queue TX packet.
		status_t status = fTxPath->SendFirmwareChunk(data + offset, chunkSize);
		if (status != B_OK) {
			dprintf(RTL8814AU_DRIVER_NAME ": firmware chunk TX failed at "
				"offset %" B_PRIu32 ": %s\n", offset, strerror(status));
			return status;
		}

		// Step 2: Wait for the chip to acknowledge the chunk.
		status = _WaitForRsvdPageOK();
		if (status != B_OK) {
			dprintf(RTL8814AU_DRIVER_NAME ": BcnValid wait failed at "
				"offset %" B_PRIu32 "\n", offset);
			return status;
		}

		// Step 3: IDDMA from the TX buffer's beacon slot to MCU memory.
		bool isFirstChunk = (offset == 0) && resetChecksum;
		status = _IDDMATransfer(iddmaSrc, ocpDestAddr + offset,
			chunkSize, isFirstChunk);
		if (status != B_OK) {
			dprintf(RTL8814AU_DRIVER_NAME ": IDDMA transfer failed at "
				"offset %" B_PRIu32 "\n", offset);
			return status;
		}

		offset += chunkSize;
	}

	// After the last IDDMA transfer, the reference driver signals
	// readiness to the MCU by writing flags to byte 0 of REG_8051FW_CTRL
	// (0x0080).  If the checksum passed, set DL_RDY + CHKSUM_OK for the
	// section.
	uint32 ddmaCtrl = fRegisterIO->Read32(kRegDDMACh0Ctrl);
	uint8 fwCtrl = fRegisterIO->Read8(kRegMcuFwDl);

	if (!(ddmaCtrl & kDDMAChksumFail)) {
		if (ocpDestAddr == kOcpBaseIMem) {
			fwCtrl |= kImemDlRdy | kImemChksumOk;
			fRegisterIO->Write8(kRegMcuFwDl, fwCtrl);
			dprintf(RTL8814AU_DRIVER_NAME ": IRAM checksum OK, "
				"IMEM_DL_RDY set (0x%02x)\n", fwCtrl);
		} else {
			fwCtrl |= kDmemDlRdy | kDmemChksumOk;
			fRegisterIO->Write8(kRegMcuFwDl, fwCtrl);
			dprintf(RTL8814AU_DRIVER_NAME ": DMEM checksum OK, "
				"DMEM_DL_RDY set (0x%02x)\n", fwCtrl);
		}
	} else {
		dprintf(RTL8814AU_DRIVER_NAME ": section checksum FAILED "
			"(DDMA_CH0_CTRL=0x%08" B_PRIx32 ")\n", ddmaCtrl);
		fRegisterIO->Write32(kRegDDMACh0Ctrl,
			ddmaCtrl | kDDMAChksumRst);
		return B_IO_ERROR;
	}

	return B_OK;
}


/*! Trigger an IDDMA transfer from the TX packet buffer to MCU memory.

    Programs DDMA channel 0 registers with source/destination addresses and
    transfer length, then polls until the hardware clears the ownership bit.
    The DDMA engine also computes a running checksum over the transferred data.

    The reference driver (IDDMADownLoadFW_3081) first polls for channel idle,
    then programs SA/DA/CTRL, then polls for completion.  For the first chunk
    of each section it omits CHKSUM_CONT; for subsequent chunks it sets
    CHKSUM_CONT to continue the running checksum.

    \param srcAddr        OCP source address (typically kOcpBaseTxBuf)
    \param destAddr       OCP destination address (DMEM or IRAM base + offset)
    \param length         Number of bytes to transfer
    \param resetChecksum  True for the first chunk of a section (no CHKSUM_CONT)

    Matches IDDMADownLoadFW_3081() in the reference driver.
*/
status_t
RTL8814AUFirmware::_IDDMATransfer(uint32 srcAddr, uint32 destAddr,
	uint32 length, bool resetChecksum)
{
	// Step 1: Wait for DDMA channel 0 to be idle (OWN bit clear).
	// The reference driver checks this before every transfer.
	uint32 attempts = 0;
	while (attempts < kDDMAPollAttempts) {
		uint32 status = fRegisterIO->Read32(kRegDDMACh0Ctrl);
		if (!(status & kDDMAChOwn))
			break;
		snooze(kDDMAPollDelay);
		attempts++;
	}
	if (attempts >= kDDMAPollAttempts) {
		dprintf(RTL8814AU_DRIVER_NAME ": IDDMA channel not idle "
			"(ctrl=0x%08" B_PRIx32 ")\n",
			fRegisterIO->Read32(kRegDDMACh0Ctrl));
		return B_TIMED_OUT;
	}

	// Step 2: For the first chunk of a section, explicitly clear the
	// DDMA running-checksum accumulator and any stale CHKSUM_FAIL
	// flag by writing CHKSUM_RST without OWN.  Just omitting
	// CHKSUM_CONT is NOT sufficient on this chip — the accumulator
	// carries non-zero state from prior chip operations (EFUSE reads,
	// MAC init, etc.) and the first chunk's checksum will fail unless
	// we hard-reset it here.
	if (resetChecksum) {
		fRegisterIO->Write32(kRegDDMACh0Ctrl,
			kDDMAChksumEn | kDDMAChksumRst);
	}

	// Step 3: Build control word.  The reference driver uses:
	//   DDMA_CHKSUM_EN | DDMA_CH_OWN | (length & mask)
	//   + DDMA_CH_CHKSUM_CNT for non-first chunks (fs == FALSE).
	uint32 ctrl = kDDMAChOwn | kDDMAChksumEn | (length & kDDMALenMask);
	if (!resetChecksum)
		ctrl |= kDDMAChksumCont;

	// Step 4: Program source, destination, then control (last triggers DMA)
	fRegisterIO->Write32(kRegDDMACh0SA, srcAddr);
	fRegisterIO->Write32(kRegDDMACh0DA, destAddr);
	fRegisterIO->Write32(kRegDDMACh0Ctrl, ctrl);

	// Step 5: Poll until hardware clears the OWN bit (transfer complete).
	// Note: do NOT inspect kDDMAChksumFail here.  CHKSUM_FAIL reflects
	// the running checksum the DDMA engine accumulates across all
	// chunks of a section, and is only meaningful after the LAST chunk
	// has been transferred.  Matches IDDMADownLoadFW_3081() in the
	// reference driver, which only checks DDMA_CHKSUM_FAIL when
	// ls == _TRUE.  On non-last chunks the bit may still be set from a
	// prior incomplete section or reflect transient accumulator state.
	// The caller validates CHKSUM_FAIL once the section is fully
	// downloaded (see _StartFirmwareSection).
	attempts = 0;
	while (attempts < kDDMAPollAttempts) {
		uint32 status = fRegisterIO->Read32(kRegDDMACh0Ctrl);
		if (!(status & kDDMAChOwn))
			return B_OK;
		snooze(kDDMAPollDelay);
		attempts++;
	}

	dprintf(RTL8814AU_DRIVER_NAME ": IDDMA transfer timed out "
		"(ctrl=0x%08" B_PRIx32 ")\n",
		fRegisterIO->Read32(kRegDDMACh0Ctrl));
	return B_TIMED_OUT;
}


/*! Poll for CPU_DL_READY (bit 15) in REG_MCUFWDL / REG_8051FW_CTRL_8814A.
    The Lexra 3081 MCU sets this bit when firmware initialization is complete.

    Timeout: 100 iterations x 50 ms = 5 seconds.

    Matches _FWFreeToGo8814A() in the reference driver.
*/
status_t
RTL8814AUFirmware::_PollForReady()
{
	dprintf(RTL8814AU_DRIVER_NAME ": waiting for CPU_DL_READY "
		"(REG_MCUFWDL=0x%08" B_PRIx32 ")\n",
		fRegisterIO->Read32(kRegMcuFwDl));

	uint32 attempts = 0;
	while (attempts < kFirmwareReadyAttempts) {
		snooze(kFirmwareReadyDelay);
		uint32 ctrl = fRegisterIO->Read32(kRegMcuFwDl);
		if (ctrl & kMcuCpuDlReady) {
			dprintf(RTL8814AU_DRIVER_NAME ": firmware init complete "
				"(CPU_DL_READY set after %" B_PRIu32 " polls, "
				"REG_MCUFWDL=0x%08" B_PRIx32 ")\n",
				attempts + 1, ctrl);
			return B_OK;
		}
		attempts++;
	}

	uint32 finalCtrl = fRegisterIO->Read32(kRegMcuFwDl);
	dprintf(RTL8814AU_DRIVER_NAME ": firmware init ready timed out after "
		"%" B_PRIu32 " polls (REG_MCUFWDL=0x%08" B_PRIx32 ")\n",
		attempts, finalCtrl);

	// Log additional diagnostic registers
	uint32 dmemCon = fRegisterIO->Read32(kRegCpuDmemCon);
	uint16 sysFuncEn = fRegisterIO->Read16(kRegSysFuncEn);
	dprintf(RTL8814AU_DRIVER_NAME ": REG_CPU_DMEM_CON=0x%08" B_PRIx32
		" REG_SYS_FUNC_EN=0x%04x\n", dmemCon, sysFuncEn);

	return B_TIMED_OUT;
}
