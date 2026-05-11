/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Firmware.h — RTL8814AU firmware loading.
 *
 * The RTL8814AU contains a Lexra 3081 MIPS-derived CPU that runs firmware
 * for MLME management, rate adaptation, and power control. The firmware
 * binary has a 64-byte header followed by two sections: DMEM (data memory)
 * and IRAM (instruction RAM).
 *
 * Loading sequence:
 *   1. Enable download mode (set MCUFWDL_EN)
 *   2. Halt the MCU via REG_SYS_FUNC_EN bit 12 (kSysFuncEnCpuEn)
 *   3. Reset the DDMA engine
 *   4. Write DMEM section to TX buffer, IDDMA to OCP DMEM (0x00200000)
 *   5. Write IRAM section to TX buffer, IDDMA to OCP IRAM (0x00000000)
 *   6. Resume the MCU
 *   7. Disable download mode
 *   8. Poll CPU_DL_READY (bit 15 of REG_8051FW_CTRL_8814A) — 5 sec timeout
 *
 * Reference: rtl8814a_hal_init.c — FirmwareDownload8814A(),
 *            IDDMADownLoadFW_3081(), _FWFreeToGo8814A()
 *            in morrownr/8814au.
 */
#ifndef RTL8814AU_FIRMWARE_H
#define RTL8814AU_FIRMWARE_H


#include <SupportDefs.h>

#include "RTL8814AU.h"


class RTL8814AURegisterIO;
class RTL8814AUTxPath;


// Firmware binary header — 64 bytes at the start of the .bin file.
// Fields are little-endian. Matches the Lexra 3081 firmware header layout
// in the reference driver (GET_FIRMWARE_HDR_*_3081 macros).
//
// The header encodes sizes for two memory sections:
//   - DMEM (data memory): initialized variables and tables (up to 32 KB)
//   - IRAM (instruction RAM): executable code (up to 64 KB)
//
// In the binary, DMEM immediately follows the header, then IRAM follows
// DMEM. Each section has 4 extra bytes appended for checksum purposes.
struct RTL8814AUFirmwareHeader {
	// Bytes 0–7: identification
	uint16	signature;		// 0x00: Expected: 0x8814
	uint8	category;		// 0x02: Firmware category
	uint8	function;		// 0x03: Function version
	uint16	version;		// 0x04: Firmware version
	uint8	subversion;		// 0x06: Sub-version
	uint8	subIndex;		// 0x07: Sub-index

	// Bytes 8–15: SVN and build info
	uint32	svnIndex;		// 0x08: SVN revision
	uint32	reserved1;		// 0x0C: Reserved

	// Bytes 16–23: build timestamp
	uint8	month;			// 0x10: Build month
	uint8	day;			// 0x11: Build day
	uint8	hour;			// 0x12: Build hour
	uint8	minute;			// 0x13: Build minute
	uint16	year;			// 0x14: Build year
	uint8	foundry;		// 0x16: Foundry ID
	uint8	reserved2;		// 0x17: Reserved

	// Bytes 24–35: reserved
	uint32	reserved3;		// 0x18
	uint32	reserved4;		// 0x1C
	uint32	reserved5;		// 0x20

	// Bytes 36–39: DMEM section size (total including 4-byte checksum dummy)
	uint32	totalDmemSize;	// 0x24: DMEM payload size

	// Bytes 40–47: reserved
	uint32	reserved6;		// 0x28
	uint32	reserved7;		// 0x2C

	// Bytes 48–51: IRAM section size
	uint32	iramSize;		// 0x30: IRAM payload size

	// Bytes 52–63: ERAM size and padding
	uint32	eramSize;		// 0x34: ERAM size (unused on this chip)
	uint32	reserved8;		// 0x38
	uint32	reserved9;		// 0x3C
};	// 64 bytes total


class RTL8814AUFirmware {
public:
								RTL8814AUFirmware(
									RTL8814AURegisterIO* registerIO);
								~RTL8814AUFirmware();

	// Inject the TX path used to submit firmware chunks on the beacon
	// bulk OUT endpoint.  Must be called before Load() — the firmware
	// loader cannot operate without it.
	void						SetTxPath(RTL8814AUTxPath* txPath)
									{ fTxPath = txPath; }

	// Load firmware from the given filesystem path. Reads the file,
	// validates the header, and transfers DMEM + IRAM sections to the
	// chip via IDDMA. Returns B_OK on success.
	status_t					Load(const char* firmwarePath);

	// Query firmware status after loading
	bool						IsLoaded() const { return fLoaded; }
	uint16						Version() const { return fVersion; }

private:
	// Internal steps of the load sequence
	status_t					_ReadFile(const char* path);
	status_t					_ValidateHeader();
	status_t					_EnableDownloadMode();
	status_t					_DisableDownloadMode();
	void						_HaltMCU();
	void						_ResumeMCU();
	void						_ResetDDMA();

	// Configure the beacon queue for firmware reserved-page submission.
	// Matches the preamble of HalROMDownloadFWRSVDPage8814A() in the
	// reference driver.
	status_t					_PrepareBeaconQueue();

	// Transfer a firmware section to the chip.  Each chunk is submitted
	// as a beacon-queue TX packet, acknowledged via the BcnValid bit,
	// then IDDMA'd from the beacon's location in the TX packet buffer
	// to the target OCP region (DMEM or IRAM).
	status_t					_TransferSection(const uint8* data,
									uint32 size, uint32 ocpDestAddr,
									bool resetChecksum);

	// Wait for the chip to acknowledge a beacon-queue firmware chunk by
	// setting bit 7 of REG_FIFOPAGE_CTRL_2+1 (BcnValidReg).
	status_t					_WaitForRsvdPageOK();

	// IDDMA: trigger DMA from TX buffer to MCU memory, verify checksum
	status_t					_IDDMATransfer(uint32 srcAddr,
									uint32 destAddr, uint32 length,
									bool resetChecksum);

	// Poll for MCU ready after firmware load
	status_t					_PollForReady();

	RTL8814AURegisterIO*		fRegisterIO;
	RTL8814AUTxPath*			fTxPath;

	// Firmware file contents (allocated during _ReadFile, freed after
	// successful load since the data is now in chip memory)
	uint8*						fData;
	uint32						fDataSize;

	// Parsed header info
	bool						fLoaded;
	uint16						fVersion;
	uint32						fDmemOffset;	// Offset of DMEM section in file
	uint32						fDmemSize;		// DMEM section size (bytes)
	uint32						fIramOffset;	// Offset of IRAM section in file
	uint32						fIramSize;		// IRAM section size (bytes)
};


#endif	// RTL8814AU_FIRMWARE_H
