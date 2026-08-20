/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * TxPath.cpp — TX data path implementation for RTL8814AU.
 *
 * Outbound frame flow:
 *   1. Network stack or WiFi management calls Transmit() with raw frame data
 *   2. Transmit() acquires a free transfer buffer from the pre-allocated pool
 *   3. _BuildDescriptor() writes the 40-byte TX descriptor at the buffer start
 *   4. Frame data is copied immediately after the descriptor
 *   5. The combined buffer is submitted via queue_bulk() on the appropriate
 *      bulk OUT endpoint (selected by WMM priority)
 *   6. _TxCallback() fires when the USB transfer completes, releasing the
 *      buffer back to the pool and updating statistics
 *
 * Buffer management:
 *   Each bulk OUT pipe has kTxTransfersPerQueue pre-allocated transfer buffers
 *   (currently 4 each, 12 total). This bounds memory usage while allowing
 *   enough USB pipelining for good throughput. If all buffers for a pipe are
 *   in-flight, Transmit() blocks briefly on the completion semaphore.
 *
 * Reference: rtl8814a_xmit.c in ulli-kroll/rtl8814au.
 */

#include "TxPath.h"

#include <new>
#include <string.h>

#include <ByteOrder.h>
#include <KernelExport.h>
#include <OS.h>
#include <util/AutoLock.h>

#include "RegisterIO.h"


// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------


RTL8814AUTxPath::RTL8814AUTxPath(RTL8814AURegisterIO* registerIO,
	usb_module_info* usbModule, usb_device usbDevice,
	usb_pipe bulkOut[kBulkOutEndpointCount])
	:
	fRegisterIO(registerIO),
	fUSBModule(usbModule),
	fUSBDevice(usbDevice),
	fSequenceNumber(0),
	fFramesSent(0),
	fFramesFailed(0),
	fInitStatus(B_NO_INIT)
{
	memcpy(fBulkOut, bulkOut, sizeof(fBulkOut));
	mutex_init(&fLock, "rtl8814au:tx");

	for (uint32 p = 0; p < kBulkOutEndpointCount; p++)
		fPipeSlotFree[p] = -1;

	// Allocate transfer buffers and semaphores for each slot
	for (uint32 i = 0; i < kTxTotalTransfers; i++) {
		fTransfers[i].buffer = new(std::nothrow) uint8[kUsbTxBufferSize];
		if (fTransfers[i].buffer == NULL) {
			fInitStatus = B_NO_MEMORY;
			return;
		}
		fTransfers[i].bufferSize = kUsbTxBufferSize;
		fTransfers[i].submitLength = 0;
		fTransfers[i].inUse = false;
		fTransfers[i].owner = this;
		fTransfers[i].pipeIndex = i / kTxTransfersPerQueue;

		fTransfers[i].completionSem = create_sem(0, "rtl8814au:tx_done");
		if (fTransfers[i].completionSem < 0) {
			fInitStatus = fTransfers[i].completionSem;
			return;
		}
	}

	// Per-pipe "any slot freed" semaphore.  Released on each TX
	// completion; Transmit() waits on it when the pipe has no free
	// slot.  Counting semantics don't matter — waiters re-scan
	// _FindFreeTransfer after every wake.
	for (uint32 p = 0; p < kBulkOutEndpointCount; p++) {
		fPipeSlotFree[p] = create_sem(0, "rtl8814au:tx_pipe_free");
		if (fPipeSlotFree[p] < 0) {
			fInitStatus = fPipeSlotFree[p];
			return;
		}
	}

	fInitStatus = B_OK;
	dprintf(RTL8814AU_DRIVER_NAME ": TX path initialized "
		"(%" B_PRIu32 " transfer buffers)\n", kTxTotalTransfers);
}


RTL8814AUTxPath::~RTL8814AUTxPath()
{
	CancelAll();

	for (uint32 i = 0; i < kTxTotalTransfers; i++) {
		delete[] fTransfers[i].buffer;
		fTransfers[i].buffer = NULL;
		if (fTransfers[i].completionSem >= 0)
			delete_sem(fTransfers[i].completionSem);
	}

	for (uint32 p = 0; p < kBulkOutEndpointCount; p++) {
		if (fPipeSlotFree[p] >= 0)
			delete_sem(fPipeSlotFree[p]);
	}

	mutex_destroy(&fLock);
}


// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------


/*! Transmit a single frame. Builds the TX descriptor, copies the frame
    into a transfer buffer, and submits it on the appropriate bulk OUT pipe.

    If all transfer buffers for the target pipe are in-flight, this will
    briefly block until one completes.

    \return B_OK on successful submission (NOT delivery confirmation).
*/
status_t
RTL8814AUTxPath::Transmit(const uint8* frameData, uint32 frameLength,
	TxQueueSelect queueSelect, uint8 dataRate, uint8 macID,
	SecurityType secType, bool isBroadcast)
{
	if (fInitStatus != B_OK)
		return fInitStatus;

	// Validate frame size — descriptor + frame must fit in the USB buffer
	uint32 totalLength = kTxDescSize + frameLength;
	if (totalLength > kUsbTxBufferSize) {
		dprintf(RTL8814AU_DRIVER_NAME ": TX frame too large: %" B_PRIu32
			" bytes (max %" B_PRIu32 ")\n",
			totalLength, kUsbTxBufferSize);
		return B_BAD_VALUE;
	}

	uint32 pipeIndex = _QueueToPipeIndex(queueSelect);

	// Find a free transfer buffer for this pipe.  Loop until either we
	// claim a slot or a hard deadline expires.  Waiting on the per-pipe
	// fPipeSlotFree sem (signaled on every TX completion for the pipe)
	// avoids the lost-wakeup bug where waiting on one specific slot's
	// sem misses completions on the other slots.
	const bigtime_t kTxWaitDeadline = system_time() + 2000000;	// 2s
	bool warnedFull = false;
	int32 transferIndex;
	MutexLocker locker(fLock);
	while (true) {
		transferIndex = _FindFreeTransfer(pipeIndex);
		if (transferIndex >= 0)
			break;

		const bigtime_t now = system_time();
		if (now >= kTxWaitDeadline) {
			dprintf(RTL8814AU_DRIVER_NAME ": TX wait timed out on pipe %u\n",
				(unsigned)pipeIndex);
			fFramesFailed++;
			return B_TIMED_OUT;
		}

		if (!warnedFull) {
			dprintf(RTL8814AU_DRIVER_NAME ": TX queue %u full, waiting\n",
				(unsigned)pipeIndex);
			warnedFull = true;
		}

		locker.Unlock();
		// Drain any stale tokens so the next acquire actually blocks for
		// a fresh completion, then wait briefly.  Spurious wakes are fine
		// — the loop re-scans _FindFreeTransfer.
		acquire_sem_etc(fPipeSlotFree[pipeIndex], 1,
			B_RELATIVE_TIMEOUT | B_CAN_INTERRUPT,
			kTxWaitDeadline - now);
		locker.Lock();
	}

	TxTransfer* transfer = &fTransfers[transferIndex];
	transfer->inUse = true;

	// Build the TX descriptor at the start of the buffer
	_BuildDescriptor(transfer->buffer, frameLength, queueSelect,
		dataRate, macID, secType, isBroadcast);

	// Copy the frame data after the descriptor
	memcpy(transfer->buffer + kTxDescSize, frameData, frameLength);

	locker.Unlock();

	// Submit the USB bulk OUT transfer
	status_t status = fUSBModule->queue_bulk(fBulkOut[pipeIndex],
		transfer->buffer, totalLength, _TxCallback, transfer);
	if (status != B_OK) {
		// Everything the USB stack could be objecting to.  queue_bulk
		// returning B_BAD_VALUE on a pipe that has already carried
		// traffic is the interesting case: it says the submission itself
		// is malformed rather than the link being busy.  Count in-use
		// slots too, since a slot that is handed out while still queued
		// would produce exactly this.
		uint32 inUseCount = 0;
		uint32 base = pipeIndex * kTxTransfersPerQueue;
		for (uint32 i = 0; i < kTxTransfersPerQueue; i++) {
			if (fTransfers[base + i].inUse)
				inUseCount++;
		}

		dprintf(RTL8814AU_DRIVER_NAME ": queue_bulk failed for TX: %s "
			"(pipe=%u handle=%p slot=%" B_PRId32 " len=%" B_PRIu32
			" frame=%" B_PRIu32 " inUse=%" B_PRIu32 "/%" B_PRIu32
			" buf=%p)\n", strerror(status), (unsigned)pipeIndex,
			(void*)(addr_t)fBulkOut[pipeIndex], transferIndex,
			totalLength, frameLength, inUseCount,
			(uint32)kTxTransfersPerQueue, transfer->buffer);
		MutexLocker relock(fLock);
		transfer->inUse = false;
		fFramesFailed++;
		return status;
	}

	return B_OK;
}


/*! Send a single firmware download chunk synchronously on the beacon
    bulk OUT endpoint.

    Builds a minimal beacon-queue TX descriptor (QSEL = kQslBeacon = 0x10,
    FS | LS | OWN, Offset = 40, BMC set) and submits it via queue_bulk()
    on pipe 0, then blocks on the completion semaphore until the USB
    transfer finishes.

    Pipe 0 is the first enumerated bulk OUT endpoint — in the reference
    driver's 3EP non-WMM mapping (_ThreeOutPipeMapping), BCN, MGT, HIGH,
    and TXCMD queues all route to RtOutPipe[0] (QUEUE_HIGH priority,
    typically EP2).  Data queues use pipes 1–2.

    The chip writes the packet into its TX packet buffer at a location
    determined by the beacon-queue page boundary, then sets BcnValid
    (bit 7 of REG_FIFOPAGE_CTRL_2+1) to acknowledge.  The firmware
    loader waits on that bit before triggering IDDMA.

    Reference: SetDownLoadFwRsvdPagePkt_8814A() and _ThreeOutPipeMapping()
    in zebulon2/rtl8814au (hal/hal_com.c).
*/
status_t
RTL8814AUTxPath::SendFirmwareChunk(const uint8* data, uint32 length)
{
	if (fInitStatus != B_OK)
		return fInitStatus;

	if (length == 0 || length > kFwPageSize)
		return B_BAD_VALUE;

	uint32 totalLength = kTxDescSize + length;
	if (totalLength > kUsbTxBufferSize)
		return B_BAD_VALUE;

	// Use pipe 0 (first enumerated bulk OUT) — matches the reference
	// 3EP mapping where BCN and MGT go to RtOutPipe[0].  Slot 0 of
	// that pipe's transfer pool is used; firmware load is single-
	// threaded so no contention is expected.
	const uint32 pipeIndex = 0;
	const uint32 slotIndex = pipeIndex * kTxTransfersPerQueue;

	MutexLocker locker(fLock);

	TxTransfer* transfer = &fTransfers[slotIndex];
	if (transfer->inUse) {
		dprintf(RTL8814AU_DRIVER_NAME ": firmware TX slot busy\n");
		return B_BUSY;
	}
	transfer->inUse = true;

	// Build minimal beacon-queue TX descriptor.
	uint8* desc = transfer->buffer;
	memset(desc, 0, kTxDescSize);

	// DWORD 0: packet length, descriptor offset = 40, BMC, FS, LS, OWN
	uint32 dword0 = (length & kTxDescPktLen_Mask)
		| ((kTxDescSize << kTxDescOffset_Shift) & kTxDescOffset_Mask)
		| kTxDescBMC | kTxDescFS | kTxDescLS | kTxDescOWN;

	// DWORD 1: MACID=0, QSEL = 0x10 (QSLT_BEACON)
	uint32 dword1 = ((uint32)kQslBeacon << kTxDescQueueSel_Shift)
		& kTxDescQueueSel_Mask;

	uint32* desc32 = reinterpret_cast<uint32*>(desc);
	desc32[0] = B_HOST_TO_LENDIAN_INT32(dword0);
	desc32[1] = B_HOST_TO_LENDIAN_INT32(dword1);
	// DWORDs 2-9 remain zero — no aggregation, sequence, rate, or power

	// Compute the TX descriptor checksum.  The chip silently drops every
	// USB TX whose 16-bit descriptor checksum is wrong, and the bulk OUT
	// endpoint then never gets drained — which manifests as the bulk URB
	// never completing (Operation canceled on cancel_queued_transfers()).
	// Algorithm: XOR all 16 little-endian u16 words of the first 32 bytes
	// of the descriptor; the checksum field itself sits at byte offset 28
	// and is left zero until the final write.  Matches
	// rtl8814a_cal_txdesc_chksum() in the reference driver
	// (hal/rtl8814a/rtl8814a_xmit.c).
	uint16 checksum = 0;
	for (uint32 i = 0; i < 16; i++) {
		uint16 word = (uint16)desc[2 * i]
			| ((uint16)desc[2 * i + 1] << 8);
		checksum ^= word;
	}
	desc[28] = (uint8)(checksum & 0xFF);
	desc[29] = (uint8)((checksum >> 8) & 0xFF);

	memcpy(desc + kTxDescSize, data, length);

	// Clear the completion semaphore of any stale signals from prior TX.
	sem_info info;
	if (get_sem_info(transfer->completionSem, &info) == B_OK
		&& info.count > 0) {
		acquire_sem_etc(transfer->completionSem, info.count,
			B_RELATIVE_TIMEOUT, 0);
	}

	transfer->submitLength = totalLength;

	locker.Unlock();

	status_t status = fUSBModule->queue_bulk(fBulkOut[pipeIndex],
		transfer->buffer, totalLength, _TxCallback, transfer);
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": firmware queue_bulk failed: %s\n",
			strerror(status));
		MutexLocker relock(fLock);
		transfer->inUse = false;
		return status;
	}

	// Wait for the USB transfer to complete (1 second is generous for a
	// 4 KB bulk transfer at USB 2.0).  The callback clears inUse and
	// releases the semaphore.
	status = acquire_sem_etc(transfer->completionSem, 1,
		B_RELATIVE_TIMEOUT, 1000000);
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": firmware TX completion wait "
			"timed out (%" B_PRIu32 " bytes)\n", length);
		fUSBModule->cancel_queued_transfers(fBulkOut[pipeIndex]);
		return B_TIMED_OUT;
	}

	return B_OK;
}


/*! Clear ENDPOINT_HALT on the beacon-queue bulk OUT pipe.

    If the chip's EP2 is in a STALL state (possibly a residue from a
    previous session or triggered by a malformed control transfer
    during probe), no bulk OUT transfer will ever be drained.  The
    standard USB recovery is a CLEAR_FEATURE(ENDPOINT_HALT) request,
    which resets the endpoint's halt condition and data toggle.
*/
status_t
RTL8814AUTxPath::ClearBeaconPipeHalt()
{
	status_t status = fUSBModule->clear_feature(fBulkOut[0],
		USB_FEATURE_ENDPOINT_HALT);
	dprintf(RTL8814AU_DRIVER_NAME ": clear_feature(ENDPOINT_HALT) on "
		"bulk OUT pipe 0 returned %s\n", strerror(status));
	return status;
}


/*! Cancel all pending TX transfers. Called during device shutdown or
    removal. After this returns, no callbacks will fire.
*/
void
RTL8814AUTxPath::CancelAll()
{
	MutexLocker locker(fLock);

	// Cancel all in-flight USB transfers on each bulk OUT pipe
	for (uint32 pipe = 0; pipe < kBulkOutEndpointCount; pipe++) {
		if (fBulkOut[pipe] != 0)
			fUSBModule->cancel_queued_transfers(fBulkOut[pipe]);
	}

	// Mark all transfer slots as free
	for (uint32 i = 0; i < kTxTotalTransfers; i++)
		fTransfers[i].inUse = false;
}


// ---------------------------------------------------------------------------
// Private implementation
// ---------------------------------------------------------------------------


/*! Build the 40-byte TX descriptor that the RTL8814AU hardware expects
    prepended to every transmitted frame.

    The descriptor is written in little-endian format into the provided
    buffer, which must be at least kTxDescSize (40) bytes.

    Descriptor layout (10 DWORDs):
      DWORD 0: packet length, descriptor offset, BMC, first/last segment, OWN
      DWORD 1: MACID, queue select, security type, packet offset
      DWORD 2: aggregation flags
      DWORD 3: sequence number
      DWORD 4: data rate, bandwidth, RTS/CTS, short preamble
      DWORD 5: TX power offset
      DWORD 6–9: reserved / future use
*/
void
RTL8814AUTxPath::_BuildDescriptor(uint8* descriptor, uint32 frameLength,
	TxQueueSelect queueSelect, uint8 dataRate, uint8 macID,
	SecurityType secType, bool isBroadcast)
{
	// Zero the entire descriptor first — unused fields must be 0
	memset(descriptor, 0, kTxDescSize);

	// Assign and advance the sequence number
	uint16 seqNum = fSequenceNumber++;
	if (fSequenceNumber > 0x0FFF)
		fSequenceNumber = 0;

	// DWORD 0: packet length, descriptor offset (40 bytes = 0x28),
	// broadcast/multicast flag, first segment, last segment, OWN
	uint32 dword0 = (frameLength & kTxDescPktLen_Mask)
		| ((kTxDescSize << kTxDescOffset_Shift) & kTxDescOffset_Mask)
		| kTxDescFS | kTxDescLS | kTxDescOWN;
	if (isBroadcast)
		dword0 |= kTxDescBMC;

	// Translate our internal queue enum to the descriptor's QSLT_*
	// value namespace.  The TX descriptor's queue_sel field expects:
	//   QSLT_MGNT = 0x12, QSLT_HIGH = 0x11, QSLT_BCN = 0x10,
	//   QSLT_VO = 0x07, QSLT_VI = 0x05, QSLT_BE = 0x02, QSLT_BK = 0x01.
	// Our enum encodes the USB pipe index instead.  Without the right
	// QSLT value the chip's MAC scheduler never picks the frame off
	// its internal queue, so nothing reaches the air.
	// NB: TxQueueSelect enum values are USB pipe indices, not unique
	// queue IDs — kTxQueueMGT/CMD/BCN all = 2.  In _BuildDescriptor
	// we treat any pipe-3 queue (MGT/CMD/BCN) as MGNT (0x12); the
	// beacon path uses its own dword-1 build above with kQslBeacon.
	// VO/VI both map to pipe 0; BE/BK to pipe 1.  Pick the lower-prio
	// value of each pair so we don't lie about urgency to the chip.
	// The reference driver's values (hal_com.h): BE is 0, BK is 2, VI is 5,
	// VO is 7, MGNT is 0x12.
	//
	// This used to send best-effort traffic as 0x02, labelled QSLT_BE in a
	// comment. 0x02 is QSLT_BK — the background queue. Management frames
	// were tagged 0x12 and correct, which is exactly why auth and assoc
	// always reached the air while **no data frame ever did**: an
	// over-the-air capture of a full association showed 55 management frames
	// from this station and zero data frames. Everything that looked like a
	// separate mystery — EAPOL M2 never reaching the access point, DHCP
	// never completing, pings above a trivial size vanishing — was this one
	// wrong constant.
	uint32 qslt;
	if (queueSelect == kTxQueueMGT)			// pipe 3 (MGT/CMD/BCN)
		qslt = 0x12;							// QSLT_MGNT
	else if (queueSelect == kTxQueueBE)		// pipe 1 (BE/BK)
		qslt = 0x00;							// QSLT_BE
	else if (queueSelect == kTxQueueVO)		// pipe 0 (VO/VI)
		qslt = 0x05;							// QSLT_VI
	else
		qslt = 0x00;							// QSLT_BE

	// For management frames morrownr's pcap shows MACID=1, rate_id=8.
	// Data frames also use MACID=1 because we haven't sent the H2C
	// MacIDCfg that would set up MACID 0's rate-adaptation table —
	// without that, frames TX'd with MACID 0 are silently dropped by
	// the chip's MAC scheduler.  rate_id=8 indexes the chip's default
	// rate set which covers OFDM 6-54 Mbps.
	// Management frames go out on MACID 1; data frames go out on whatever
	// the caller passed, which is 0.
	//
	// The comment above claims data frames should use MACID 1 too, on the
	// grounds that MACID 0 has no rate-adaptation table and the scheduler
	// discards it.  That claim is wrong, and it was tested: forcing data
	// frames onto MACID 1 stopped DHCP working on an open network that had
	// completed DHCP moments earlier on MACID 0 — no address, nothing
	// received.  Whatever RA_INFO does for MACID 1, data frames need
	// MACID 0, so leave them there.  It did not help the EAPOL handshake
	// either, which is what prompted trying it.
	uint8 effectiveMacID = macID;
	uint32 rateID = 8;	// chip's default rate set, OFDM 6-54 Mbps
	if (queueSelect == kTxQueueMGT || queueSelect == kTxQueueCMD) {
		if (effectiveMacID == 0)
			effectiveMacID = 1;
	}

	// DWORD 1: MACID, queue select, rate ID, security type
	uint32 dword1 = (effectiveMacID & kTxDescMACID_Mask)
		| ((qslt << kTxDescQueueSel_Shift) & kTxDescQueueSel_Mask)
		| ((rateID << kTxDescRateID_Shift) & kTxDescRateID_Mask)
		| (((uint32)secType << kTxDescSecType_Shift)
			& kTxDescSecType_Mask);

	// DWORD 2: aggregation enable.  We previously set kTxDescAGGEn
	// for data frames here, but the chip never actually emits them —
	// almost certainly because we haven't configured A-MPDU/A-MSDU
	// state, so the chip waits for aggregation that never comes and
	// silently discards the queued frame.  Leave AGG clear until
	// aggregation is properly set up.
	//
	// morrownr's probe-req sets the high byte of dword2 to 0x3F (NAV /
	// RTS-control bits).  Setting those bits hangs the chip's MAC
	// scheduler — the queue stops draining and the USB control pipe
	// times out.  Leave them clear; mgmt frames don't need them.
	// Ask for a transmit report on this frame.  The report says whether the
	// peer acknowledged it, which is the only way from inside the driver to
	// tell "the chip put it on the air and nobody answered" apart from "the
	// chip never sent it".  Cheap while traffic is this light.
	uint32 dword2 = kTxDescSpeRpt;

	// DWORD 3: sequence number + USE_RATE / DISABLE_FB / NAV_USE_HDR.
	// USE_RATE (bit 8) tells the chip to use the data_rate in dword4
	// instead of waiting for rate-adaptation hints — without it the
	// chip never transmits.
	uint32 dword3 = ((uint32)seqNum << kTxDescSeq_Shift)
		& kTxDescSeq_Mask;
	dword3 |= (1 << 8);		// USE_RATE
	if (queueSelect == kTxQueueMGT || queueSelect == kTxQueueCMD) {
		dword3 |= (1 << 10);	// DISABLE_FB
		dword3 |= (1 << 15);	// NAV_USE_HDR
	}

	// DWORD 4: data rate, short preamble for CCK rates.
	// morrownr probe-req has 0x001A0000 (rate index 0x1A which seems
	// off — possibly bandwidth/spec related).  For now keep our value.
	uint32 dword4 = (dataRate & kTxDescDataRate_Mask);
	if (dataRate <= kRateCCK11)
		dword4 |= kTxDescDataShort;

	// DWORD 6: morrownr's pcap shows 0x00000001 here for mgmt frames
	// but writing that bit also wedges the chip.  Skip it — frames go
	// out fine without it.

	// DWORD 8 bit 15: HWSEQ_EN — let HW assign sequence numbers.
	uint32 dword8 = (1 << 15);

	// Write the dwords (DWORD 7 reserved for descriptor checksum,
	// computed below over the first 32 bytes).
	uint32* desc32 = reinterpret_cast<uint32*>(descriptor);
	desc32[0] = B_HOST_TO_LENDIAN_INT32(dword0);
	desc32[1] = B_HOST_TO_LENDIAN_INT32(dword1);
	desc32[2] = B_HOST_TO_LENDIAN_INT32(dword2);
	desc32[3] = B_HOST_TO_LENDIAN_INT32(dword3);
	desc32[4] = B_HOST_TO_LENDIAN_INT32(dword4);
	desc32[8] = B_HOST_TO_LENDIAN_INT32(dword8);

	// 16-bit XOR checksum over first 32 bytes, stored at offset 28-29.
	uint16 checksum = 0;
	const uint16* words = reinterpret_cast<const uint16*>(descriptor);
	for (uint32 i = 0; i < 16; i++)
		checksum ^= B_LENDIAN_TO_HOST_INT16(words[i]);
	descriptor[28] = (uint8)(checksum & 0xFF);
	descriptor[29] = (uint8)((checksum >> 8) & 0xFF);
}


/*! Map a TX queue selection to the bulk OUT pipe index.

    The TxQueueSelect enum values are defined to equal the pipe index directly:
      VO/VI/HIGH = 0 → pipe 0 (high priority)
      BE/BK      = 1 → pipe 1 (normal priority)
      MGT/CMD/BCN = 2 → pipe 2 (management)
*/
uint32
RTL8814AUTxPath::_QueueToPipeIndex(TxQueueSelect queue)
{
	uint32 index = (uint32)queue;
	if (index >= kBulkOutEndpointCount)
		return 1;	// Default to best-effort
	return index;
}


/*! Read the chip's per-queue TX "empty" flags.

    Bit set means that queue has drained.  If a queue stays non-empty after
    a frame was submitted and its USB completion reported success, the frame
    is sitting in the chip's packet buffer and the MAC is not transmitting
    it — which distinguishes "we never delivered it to the chip" from "the
    chip has it and will not send it".
*/
uint16
RTL8814AUTxPath::ReadTxQueueEmpty()
{
	if (fRegisterIO == NULL)
		return 0;

	return fRegisterIO->Read16(kRegTxPktEmpty);
}


/*! Search for a free transfer slot among the slots allocated to the
    given pipe index. Each pipe has kTxTransfersPerQueue dedicated slots.

    \param pipeIndex  Bulk OUT pipe index (0–2)
    \return Transfer slot index (0..kTxTotalTransfers-1), or -1 if none free.
*/
int32
RTL8814AUTxPath::_FindFreeTransfer(uint32 pipeIndex)
{
	uint32 base = pipeIndex * kTxTransfersPerQueue;
	for (uint32 i = 0; i < kTxTransfersPerQueue; i++) {
		if (!fTransfers[base + i].inUse)
			return (int32)(base + i);
	}
	return -1;
}


/*! USB bulk OUT completion callback. Called by the USB bus manager when
    a TX transfer finishes (successfully or with an error).

    \param cookie       Pointer to the TxTransfer that completed
    \param status       B_OK on success, error code on failure
    \param data         Transfer buffer pointer (same as TxTransfer::buffer)
    \param actualLength Number of bytes actually transferred
*/
void
RTL8814AUTxPath::_TxCallback(void* cookie, status_t status, void* data,
	size_t actualLength)
{
	TxTransfer* transfer = static_cast<TxTransfer*>(cookie);
	if (transfer == NULL)
		return;

	// A successful queue_bulk only means the USB stack accepted the
	// submission.  This is where we find out whether the write actually
	// completed, and completely — a short write would leave the chip with a
	// truncated frame it can never put on the air, which is one explanation
	// for large frames vanishing while small ones get through.
	{
		static uint32 sLogged = 0;
		bool bad = status != B_OK
			|| actualLength != (size_t)transfer->submitLength;
		if (bad || sLogged < 12) {
			if (!bad)
				sLogged++;
			dprintf(RTL8814AU_DRIVER_NAME ": TX done pipe=%" B_PRIu32
				" status=%s actual=%" B_PRIuSIZE "/%" B_PRIu32 "%s\n",
				transfer->pipeIndex, strerror(status), actualLength,
				transfer->submitLength, bad ? "  <-- SHORT/FAILED" : "");
		}
	}

	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": TX callback error: %s\n",
			strerror(status));
	}

	RTL8814AUTxPath* self = transfer->owner;
	const uint32 pipeIndex = transfer->pipeIndex;

	// Release the transfer slot under fLock so _FindFreeTransfer in
	// Transmit() always sees a consistent inUse view.
	{
		MutexLocker locker(self->fLock);
		transfer->inUse = false;
		self->fFramesSent++;
	}

	// Wake the synchronous firmware-load path (per-transfer sem) AND
	// any Transmit() waiter blocked on the per-pipe slot-free sem.
	release_sem_etc(transfer->completionSem, 1, B_DO_NOT_RESCHEDULE);
	release_sem_etc(self->fPipeSlotFree[pipeIndex], 1,
		B_DO_NOT_RESCHEDULE);
}
