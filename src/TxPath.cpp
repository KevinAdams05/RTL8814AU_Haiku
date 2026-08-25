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
	fDescriptorsLogged(0),
	fFramesSent(0),
	fFramesFailed(0),
	fInitStatus(B_NO_INIT)
{
	memcpy(fBulkOut, bulkOut, sizeof(fBulkOut));
	mutex_init(&fLock, "rtl8814au:tx");

	for (uint32 p = 0; p < kBulkOutEndpointCount; p++) {
		fPipeSlotFree[p] = -1;
		fLastPipeRecovery[p] = 0;
	}

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
	// A bulk OUT transfer whose length is an exact multiple of the endpoint's
	// max packet size ends on a full packet, so the device cannot tell the
	// transfer is over and waits for a continuation that never comes.  The
	// usual cures are a zero-length terminating packet or padding; the
	// reference driver pads, and does it through the descriptor.
	//
	// Its buffers carry TXDESC_OFFSET = TXDESC_SIZE + PACKET_OFFSET_SZ = 48
	// bytes of headroom.  Normally it writes the 40-byte descriptor at
	// offset 8 so the frame follows immediately and submits 40 + length.  But
	// when 40 + length would land on a bulk-size boundary it leaves the
	// descriptor at offset 0, keeping an 8-byte gap before the frame,
	// declares the gap in the descriptor's PKT_OFFSET field, and submits
	// 48 + length instead — which cannot be a multiple of 512 or 1024 if
	// 40 + length was.
	//
	// We submit 40 + length unconditionally, so every frame whose total hits
	// a 512-byte multiple stalls.  Nothing in the handshake happens to land
	// there (M2 totals 193 bytes), but ordinary traffic does: an ICMP echo
	// with a 412-byte payload totals exactly 512.  That is the shape of "TCP
	// is unusable and pings above a certain size vanish".
	//
	// Testing against 512 covers SuperSpeed too, since every multiple of
	// 1024 is a multiple of 512.  Padding a frame that did not strictly need
	// it is harmless — it is the same path the reference takes.
	const uint32 kBulkBoundary = 512;
	uint32 packetOffset = 0;
	if (((kTxDescSize + frameLength) % kBulkBoundary) == 0)
		packetOffset = 1;	// units of 8 bytes

	uint32 totalLength = kTxDescSize + packetOffset * 8 + frameLength;
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

			// Every slot on this pipe is outstanding and nothing is
			// completing.  Left alone this is permanent -- the interface
			// stays associated and passes nothing until the machine is
			// rebooted -- so try to unstick the pipe before giving up on
			// the frame.  Drop the lock first: recovery issues USB
			// transfers, and the cancellations it triggers run the
			// completion callback, which takes this same lock.
			locker.Unlock();
			_RecoverStalledPipe(pipeIndex);
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
	_BuildDescriptor(transfer->buffer, frameData, frameLength, queueSelect,
		dataRate, macID, secType, isBroadcast, packetOffset);

	// Copy the frame data after the descriptor, leaving the declared gap.
	// Zero the gap so the chip is not handed stale bytes from a prior frame.
	if (packetOffset > 0)
		memset(transfer->buffer + kTxDescSize, 0, packetOffset * 8);
	memcpy(transfer->buffer + kTxDescSize + packetOffset * 8, frameData,
		frameLength);

	// The 802.11 sequence number is NOT written here.
	//
	// It used to be, straight into Sequence Control in the frame header, on the
	// reasoning that the descriptor's HWSEQ_EN asked for a service
	// REG_HWSEQ_CTRL never enabled. The header write does not work: this chip's
	// MAC overwrites those two bytes on transmit. Measured over the air -- a
	// frame submitted with sequence 3 in its header arrived as sequence 0, and
	// so did all 369 frames captured from this station, while a vendor-driven
	// adapter on the same access point numbered its frames 1, 2, 3.
	//
	// That is not a cosmetic fault. Duplicate detection keys on (transmitter,
	// sequence, fragment), so every frame this driver sent looked to the access
	// point like a retransmission of the previous one, and was entitled to be
	// dropped. It shows up as authentication being ignored outright, and as a
	// retransmitted M2 being discarded when the first M2 has already been seen.
	//
	// Sequencing is now the descriptor's business, as it is in the vendor
	// driver: HWSEQ_EN for non-QoS frames, the SEQ field for QoS frames. See
	// _BuildDescriptor().

	// Record what we are about to submit, so the completion callback can
	// tell a short transfer from a complete one.  Only the firmware-download
	// path used to set this, which left it at zero here -- so every ordinary
	// transmit compared its byte count against zero, decided it was short,
	// and logged itself as SHORT/FAILED.  Every TX in the log looked like a
	// failure and none of them were.
	transfer->submitLength = totalLength;

	locker.Unlock();

	// One-shot descriptor dump per pipe, for byte-level comparison against a
	// usbmon capture of the vendor driver. The descriptor is the whole
	// interface to the chip's transmit engine, so a field-by-field diff
	// against a known-good frame settles questions that no amount of reading
	// our own code can.
	{
		static uint32 sDumped[kBulkOutEndpointCount] = {};
		if (pipeIndex < kBulkOutEndpointCount && sDumped[pipeIndex] < 1) {
			sDumped[pipeIndex]++;
			const uint32* words
				= reinterpret_cast<const uint32*>(transfer->buffer);
			dprintf(RTL8814AU_DRIVER_NAME ": TXDESC pipe=%" B_PRIu32
				" q=%d len=%" B_PRIu32 "\n"
				"  dw 0=%08x 1=%08x 2=%08x 3=%08x 4=%08x\n"
				"  dw 5=%08x 6=%08x 7=%08x 8=%08x 9=%08x\n",
				pipeIndex, (int)queueSelect, totalLength,
				(unsigned)words[0], (unsigned)words[1], (unsigned)words[2],
				(unsigned)words[3], (unsigned)words[4], (unsigned)words[5],
				(unsigned)words[6], (unsigned)words[7], (unsigned)words[8],
				(unsigned)words[9]);
			const uint8* frame = transfer->buffer + kTxDescSize
				+ packetOffset * 8;
			dprintf(RTL8814AU_DRIVER_NAME ": TXDESC hdr "
				"%02x %02x %02x %02x  %02x%02x%02x%02x%02x%02x "
				"%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x "
				"%02x %02x\n",
				frame[0], frame[1], frame[2], frame[3],
				frame[4], frame[5], frame[6], frame[7], frame[8], frame[9],
				frame[10], frame[11], frame[12], frame[13], frame[14],
				frame[15], frame[16], frame[17], frame[18], frame[19],
				frame[20], frame[21], frame[22], frame[23]);
		}
	}

	// Per-pipe submission trace. Paired with the per-pipe completion trace
	// in _TxCallback, this separates "the frame was never submitted on this
	// pipe" from "it was submitted and never came back" -- the two have
	// completely different causes and the same symptom.
	{
		static uint32 sSubmitted[kBulkOutEndpointCount] = {};
		if (pipeIndex < kBulkOutEndpointCount
			&& sSubmitted[pipeIndex] < 8) {
			sSubmitted[pipeIndex]++;
			dprintf(RTL8814AU_DRIVER_NAME ": TX submit pipe=%" B_PRIu32
				" queue=%d len=%" B_PRIu32 " rate=0x%02x\n",
				pipeIndex, (int)queueSelect, totalLength, dataRate);
		}
	}

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

	// DWORD 0: packet length, descriptor offset = 40, BMC, FS, LS, and bit 31.
	// Bit 31 was called OWN here; it is DISQSELSEQ, the same bit under its
	// 8814A name.  Firmware download works with it set, so leave it set.
	uint32 dword0 = (length & kTxDescPktLen_Mask)
		| ((kTxDescSize << kTxDescOffset_Shift) & kTxDescOffset_Mask)
		| kTxDescBMC | kTxDescFS | kTxDescLS | kTxDescDisQSelSeq;

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


/*! Try to unstick a bulk OUT pipe whose transfers have stopped completing.

    The failure this addresses: after association, every transfer slot on the
    best-effort data pipe ends up outstanding and no completion ever arrives.
    It is intermittent -- most joins are clean -- and it is not band-specific.
    Because nothing here used to recover, the result was an interface that
    stayed associated and carried nothing until the machine was rebooted.

    This is a condition the vendor driver expects. Its
    sreset_xmit_status_check() for this chip watches for exactly two things:
    a non-zero REG_TXDMA_STATUS, and "all transmit buffers in use with no
    completion for four seconds" -- which it calls a tx hang. Its answer in
    both cases is a silent reset of the MAC.

    We do the cheaper USB-level recovery rather than a MAC reset, in two
    steps, because either one alone is insufficient:

      1. Cancel the queued transfers. This is what reclaims the slots: each
         cancellation completes its transfer with B_CANCELED, which runs
         _TxCallback, which clears inUse and releases the per-pipe semaphore.
      2. Clear ENDPOINT_HALT on the pipe. If the endpoint has halted, no
         bulk OUT transfer on it will ever be drained again, and freeing the
         slots would just let the next frame wedge in the same way. Note the
         driver only ever cleared halt on pipe 0, at init -- never on the
         data pipes, and never after init.

    REG_TXDMA_STATUS is read and logged first, because it is the vendor's own
    discriminator and its value says whether the chip's transmit DMA has
    faulted or the stall is confined to USB.

    Rate-limited per pipe: a pipe that is genuinely dead must not turn this
    into a reset loop.
*/
void
RTL8814AUTxPath::_RecoverStalledPipe(uint32 pipeIndex)
{
	if (pipeIndex >= kBulkOutEndpointCount)
		return;

	const bigtime_t now = system_time();
	if (fLastPipeRecovery[pipeIndex] != 0
		&& now - fLastPipeRecovery[pipeIndex] < kPipeRecoveryInterval) {
		return;
	}
	fLastPipeRecovery[pipeIndex] = now;

	uint32 txDmaStatus = fRegisterIO->Read32(kRegTxDmaStatus);

	fUSBModule->cancel_queued_transfers(fBulkOut[pipeIndex]);
	status_t haltStatus = fUSBModule->clear_feature(fBulkOut[pipeIndex],
		USB_FEATURE_ENDPOINT_HALT);

	dprintf(RTL8814AU_DRIVER_NAME ": pipe %u stalled: TXDMA_STATUS=0x%08x, "
		"cancelled queued transfers, clear_feature(ENDPOINT_HALT)=%s\n",
		(unsigned)pipeIndex, (unsigned)txDmaStatus, strerror(haltStatus));
}


/*! Cancel all pending TX transfers. Called during device shutdown or
    removal. After this returns, no callbacks will fire.
*/
void
RTL8814AUTxPath::CancelAll()
{
	// Cancel WITHOUT holding fLock. This used to take the lock first and
	// deadlocked against itself, hanging `ifconfig <device> down` forever and
	// leaving the interface unusable until the machine was rebooted.
	//
	// The reason is in the host controller: XHCI's CancelQueuedTransfers runs
	// each cancelled transfer's callback **inline on the calling thread** --
	//
	//     endpointLocker.Unlock();
	//     for (...) { if (!force) transfers[i]->Finished(B_CANCELED, 0); }
	//
	// -- and our _TxCallback takes fLock to clear the slot. Haiku's mutexes
	// are not recursive, so the thread blocked waiting for a lock it already
	// held. The close path logged "stopping RX receive loop" and stopped
	// there, which is why the hang looked like a receive problem.
	//
	// _RecoverStalledPipe already dropped the lock before cancelling for this
	// exact reason; this function was simply missed.
	for (uint32 pipe = 0; pipe < kBulkOutEndpointCount; pipe++) {
		if (fBulkOut[pipe] != 0)
			fUSBModule->cancel_queued_transfers(fBulkOut[pipe]);
	}

	// Then reclaim the slots. The callbacks above will have cleared most of
	// them already; this covers any transfer whose callback did not run, which
	// happens when a cancellation is forced.
	MutexLocker locker(fLock);
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
RTL8814AUTxPath::_BuildDescriptor(uint8* descriptor, const uint8* frameData,
	uint32 frameLength, TxQueueSelect queueSelect, uint8 dataRate,
	uint8 macID, SecurityType secType, bool isBroadcast, uint32 packetOffset)
{
	// Zero the entire descriptor first — unused fields must be 0
	memset(descriptor, 0, kTxDescSize);

	// Read the frame's own type and subtype rather than being told, so the
	// descriptor cannot disagree with the bytes it describes.  A QoS data
	// frame carries its own sequence number and a 2-byte QoS Control field;
	// a non-QoS frame leaves sequencing to the hardware.  The reference
	// driver switches on exactly this distinction.
	const uint8 frameControl = frameData[0];
	const bool isData = ((frameControl >> 2) & 0x03) == 2;
	const bool isQosData = isData && ((frameControl >> 4) & 0x08) != 0;

	// DWORD 0: packet length, descriptor offset (40 bytes = 0x28),
	// broadcast/multicast flag, first segment, last segment, OWN
	// DISQSELSEQ is set for every non-QoS frame, as the reference does.
	// FIRST_SEG is deliberately not set: it is a ring-descriptor concept, and
	// the reference driver's USB transmit path has it commented out, setting
	// only LAST_SEG.  The bit we used to also set as OWN is bit 31, which on
	// this chip is DISQSELSEQ — the same bit, under an older name — so there
	// was never a separate OWN to clear.
	uint32 dword0 = (frameLength & kTxDescPktLen_Mask)
		| ((kTxDescSize << kTxDescOffset_Shift) & kTxDescOffset_Mask)
		| kTxDescLS;
	if (!isQosData)
		dword0 |= kTxDescDisQSelSeq;
	if (isBroadcast)
		dword0 |= kTxDescBMC;

	// Translate the queue identity to the descriptor's QSEL value.  These
	// used to be inferred from the pipe index, which could not distinguish
	// queues that shared a pipe and got two of them wrong: best effort was
	// sent as 0x02 (QSLT_BK, the background queue) and video as 0x05 while
	// voice frames were labelled video.  Now the enum carries the identity,
	// so the mapping is a direct lookup.
	uint32 qslt;
	switch (queueSelect) {
		case kTxQueueVO:	qslt = kQslVO;		break;
		case kTxQueueVI:	qslt = kQslVI;		break;
		case kTxQueueBE:	qslt = kQslBE;		break;
		case kTxQueueBK:	qslt = kQslBK;		break;
		case kTxQueueBCN:	qslt = kQslBeacon;	break;
		case kTxQueueMGT:	qslt = kQslMgnt;	break;
		case kTxQueueHIGH:	qslt = kQslHigh;	break;
		case kTxQueueCMD:	qslt = kQslCmd;		break;
		default:			qslt = kQslBE;		break;
	}

	// MACID selection follows the reference driver's allocation rules, which
	// are not obvious from the descriptor alone.
	//
	// In station mode the reference reserves MACID 1 as
	// RTW_DEFAULT_MGMT_MACID and hands it to the broadcast/multicast station,
	// with the comment "STA mode have no BMC data TX, shared with this
	// macid".  The access point's own station entry is allocated from a loop
	// that starts at 0, so it gets MACID 0.  Unicast data addressed to the
	// access point therefore goes out on MACID 0, while management and
	// group-addressed frames go out on MACID 1.
	//
	// This matters because it was got wrong in both directions.  An earlier
	// comment here claimed MACID 0 has no rate-adaptation table and that the
	// scheduler discards its frames, so everything was forced onto MACID 1.
	// The measurement that contradicted it is the strongest single data point
	// in this investigation: DHCP completed on an open network with data
	// frames on MACID 0 and stopped completing when they were moved to MACID
	// 1.  The reference explains why.
	//
	// If MACID 0 needs a rate-adaptation table, the fix is to send it one —
	// see _DoPostAssocSetup — not to move traffic to the MACID that already
	// has one and means something else.
	uint8 effectiveMacID = macID;
	if (effectiveMacID == 0
		&& (queueSelect == kTxQueueMGT || queueSelect == kTxQueueCMD
			|| isBroadcast)) {
		effectiveMacID = 1;
	}

	// RATE_ID selects which of the MAC's rate sets this frame belongs to.
	//
	// This was 8, commented as "the chip's default rate set, OFDM 6-54
	// Mbps". Value 8 is RATEID_IDX_B in the reference driver's enum: the
	// 802.11b CCK-only set, very nearly the opposite of what the comment
	// claimed. It happened to be consistent for the CCK frames this driver
	// sends at the lowest basic rate, and inconsistent for every OFDM data
	// frame, which asked the MAC for an OFDM rate out of a CCK-only set.
	//
	// 12 is RATEID_IDX_MIX2, the mixed set, and is what the vendor driver
	// puts on its data frames on this chip -- confirmed by decoding the
	// descriptors in a usbmon capture rather than by reading its source.
	uint32 rateID = 12;	// RATEID_IDX_MIX2 — mixed CCK/OFDM/HT rate set

	// DWORD 1: MACID, queue select, rate ID, security type, and the
	// padding gap between descriptor and frame in units of 8 bytes.
	uint32 dword1 = (effectiveMacID & kTxDescMACID_Mask)
		| ((qslt << kTxDescQueueSel_Shift) & kTxDescQueueSel_Mask)
		| ((rateID << kTxDescRateID_Shift) & kTxDescRateID_Mask)
		| (((uint32)secType << kTxDescSecType_Shift)
			& kTxDescSecType_Mask)
		| ((packetOffset << kTxDescPktOffset_Shift)
			& kTxDescPktOffset_Mask);

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
	// SPE_RPT asks the firmware for a per-frame transmit report.  It is not
	// set: the vendor driver asks for one on almost nothing, no C2H report
	// ever arrived in the whole time we asked on every frame, and a report
	// nothing consumes is at best wasted firmware work.
	uint32 dword2 = 0;

	// Data frames are not aggregated here, and the reference marks exactly
	// that with BK.  Management frames go out on their own queue and do not
	// take part in aggregation at all, which is one candidate explanation
	// for why they reach the air while data frames never have: a data frame
	// the chip believes is a candidate for A-MPDU can sit in the buffer
	// waiting for company.
	if (queueSelect != kTxQueueMGT && queueSelect != kTxQueueCMD)
		dword2 |= kTxDescBK;

	// DWORD 3: USE_RATE only.
	//
	// This dword used to also carry a 12-bit software sequence number at
	// bits 16-27, which is simply the wrong place for it: the descriptor's
	// SEQ field is at offset 36, bits 12-23.  Bits 16-27 of this dword are
	// USE_MAX_LEN, MAX_AGG_NUM, **NDPA** and AMPDU_MAX_TIME, so an
	// incrementing counter was scribbling over four aggregation and
	// beamforming controls on every single frame.  NDPA is the damaging one:
	// a non-zero value there tells the chip the frame is an HT/VHT null-data
	// packet announcement for channel sounding rather than something to
	// transmit normally, and three out of every four sequence numbers set
	// one of its two bits.
	//
	// Nothing needs to be written in its place: Transmit() writes the
	// sequence number straight into the frame header before submitting.
	//
	// USE_RATE (bit 8) tells the chip to use the rate in dword 4 rather than
	// waiting for a rate-adaptation hint.
	uint32 dword3 = (1 << 8);	// USE_RATE

	// No RTS/CTS, on any frame.
	//
	// Data frames used to set RTS_ENABLE here, RTS_RATE in dword 4 and
	// RTS_SHORT in dword 5, on the stated grounds that "the vendor driver
	// protects data frames with RTS/CTS ... which is what the usbmon capture
	// of a working handshake on this chip shows". Going back to that same
	// capture and decoding the descriptors: the vendor sets RTS_ENABLE on
	// none of its data frames, at any size from 64 to 1528 bytes. The claim
	// was simply wrong.
	//
	// It was not harmless. With RTS_ENABLE the MAC must win an RTS/CTS
	// exchange before it will transmit the frame at all, so a missing CTS
	// means the frame is dropped inside the chip -- the USB write completes,
	// the transmit counter increments, and nothing reaches the air. That is
	// an unusually hard failure to attribute, and whether the exchange
	// succeeds depends on antenna wiring and transmit power, which is why it
	// passed on one adapter and stalled the four-way handshake on another.

	// DWORD 4: transmit rate, and the retry limit for management frames.
	// DATA_SHORT is NOT here — see dword 5.
	uint32 dword4 = (dataRate & kTxDescDataRate_Mask);
	if (queueSelect == kTxQueueMGT || queueSelect == kTxQueueCMD) {
		// The reference gives management frames an explicit retry limit and
		// leaves fallback alone.  We used to set DISABLE_FB and NAV_USE_HDR
		// instead; those belong to its beamforming NDPA branch, not here.
		dword4 |= kTxDescRetryLimitEn
			| ((12u << kTxDescRetryLimit_Shift) & kTxDescRetryLimit_Mask);
	}

	// DWORD 5: short preamble, for CCK rates only.  The chip ignores this
	// when the rate is legacy OFDM or better.
	uint32 dword5 = 0;
	if (dataRate <= kRateCCK11)
		dword5 |= kTxDescDataShort;

	// DWORD 6: SW_DEFINE.  Bit 0 is the firmware's "the driver picked the
	// rate" flag and pairs with USE_RATE, which we always set.
	uint32 dword6 = kTxDescSwDefineFixedRate;

	// DWORDs 8 and 9: sequencing, split the way the reference driver splits it.
	//
	// A non-QoS frame gets HWSEQ_EN and lets the MAC number it. A QoS frame
	// carries an explicit number in the SEQ field of dword 9. EN_HWEXSEQ stays
	// clear in both cases, and HW_SSN_SEL in dword 3 stays 0, selecting the
	// first hardware sequence counter.
	//
	// Neither path touches the frame header, because on this chip the MAC
	// overwrites Sequence Control on transmit -- see Transmit().
	//
	// HWSEQ_EN is a request the MAC only honours if REG_HWSEQ_CTRL enables the
	// queue, so _DoJoin() writes that register before authenticating.
	uint32 dword8 = 0;
	uint32 dword9 = 0;
	if (!isQosData) {
		dword8 |= kTxDescHwSeqEn;
		dword8 &= ~kTxDescEnHwExSeq;
	} else {
		uint16 sequence = fSequenceNumber++;
		if (fSequenceNumber > 0x0FFF)
			fSequenceNumber = 0;
		dword9 |= ((uint32)sequence << kTxDescSeqNum_Shift)
			& kTxDescSeqNum_Mask;
	}

	// Write the dwords (DWORD 7 reserved for descriptor checksum,
	// computed below over the first 32 bytes).
	uint32* desc32 = reinterpret_cast<uint32*>(descriptor);
	desc32[0] = B_HOST_TO_LENDIAN_INT32(dword0);
	desc32[1] = B_HOST_TO_LENDIAN_INT32(dword1);
	desc32[2] = B_HOST_TO_LENDIAN_INT32(dword2);
	desc32[3] = B_HOST_TO_LENDIAN_INT32(dword3);
	desc32[4] = B_HOST_TO_LENDIAN_INT32(dword4);
	desc32[5] = B_HOST_TO_LENDIAN_INT32(dword5);
	desc32[6] = B_HOST_TO_LENDIAN_INT32(dword6);
	desc32[8] = B_HOST_TO_LENDIAN_INT32(dword8);
	desc32[9] = B_HOST_TO_LENDIAN_INT32(dword9);

	// 16-bit XOR checksum over first 32 bytes, stored at offset 28-29.
	uint16 checksum = 0;
	const uint16* words = reinterpret_cast<const uint16*>(descriptor);
	for (uint32 i = 0; i < 16; i++)
		checksum ^= B_LENDIAN_TO_HOST_INT16(words[i]);
	descriptor[28] = (uint8)(checksum & 0xFF);
	descriptor[29] = (uint8)((checksum >> 8) & 0xFF);

}


/*! Map a TX queue selection to the bulk OUT pipe index.

    This comment used to claim "the TxQueueSelect enum values are defined to
    equal the pipe index directly", and listed BE/BK on pipe 1 with management
    on pipe 2. That is the mapping this driver had when **no data frame ever
    reached the air**, and it is backwards: management belongs on pipe 0 and
    best-effort data on pipe 2. The enum carries queue identity, not a pipe
    index, and deliberately so -- inferring the pipe from the value is what
    collapsed distinct queues onto one pipe. See the table in the body.
*/
uint32
RTL8814AUTxPath::_QueueToPipeIndex(TxQueueSelect queue)
{
	// The three bulk OUT endpoints are 0x02, 0x03 and 0x04 in enumeration
	// order, and the chip expects specific queues on specific ones.  From
	// _ThreeOutPipeMapping in the reference driver's hal/hal_com.c:
	//
	//   VO, BCN, MGT, HIGH, CMD -> RtOutPipe[0]  (endpoint 0x02)
	//   VI                      -> RtOutPipe[1]  (endpoint 0x03)
	//   BE, BK                  -> RtOutPipe[2]  (endpoint 0x04)
	//
	// This is not a preference; it is what the hardware services.  A usbmon
	// capture of the vendor Linux driver completing a WPA2 handshake on this
	// same chip shows exactly that split: 43 management frames on endpoint
	// 0x02, the EAPOL data frames on endpoint 0x04, and endpoint 0x03 never
	// used at all.
	//
	// We had it backwards.  Management went to 0x04 and data to 0x03 — an
	// endpoint the working driver never touches — which is why management
	// frames reached the air and **no data frame ever did**, across three
	// over-the-air captures. Everything downstream of that (EAPOL M2 going
	// unanswered until the access point gave up with a four-way handshake
	// timeout, DHCP never completing, TCP unusable) followed from it.
	switch (queue) {
		case kTxQueueVO:
		case kTxQueueBCN:
		case kTxQueueMGT:
		case kTxQueueHIGH:
		case kTxQueueCMD:
			return 0;
		case kTxQueueVI:
			return 1;
		case kTxQueueBE:
		case kTxQueueBK:
			return 2;
	}
	return 2;	// unreachable; best-effort is the safe default
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
		// Budget the logging PER PIPE, not globally. A single global counter
		// is spent entirely on the firmware download -- which all goes to
		// pipe 0 -- so every later completion on the data pipes is invisible,
		// and "no log line for pipe 2" reads as "pipe 2 failed" when it
		// actually means "we never looked". That cost real debugging time.
		static uint32 sLogged[kBulkOutEndpointCount] = {};
		bool bad = status != B_OK
			|| actualLength != (size_t)transfer->submitLength;
		const uint32 pipe = transfer->pipeIndex < kBulkOutEndpointCount
			? transfer->pipeIndex : 0;
		if (bad || sLogged[pipe] < 8) {
			if (!bad)
				sLogged[pipe]++;
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
