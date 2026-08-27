/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * TxPath.h — TX data path for the RTL8814AU.
 *
 * Handles the entire outbound frame pipeline:
 *   1. Build a 40-byte TX descriptor from frame metadata
 *   2. Select the correct bulk OUT endpoint based on WMM priority
 *   3. Submit the frame + descriptor as a USB bulk OUT transfer
 *   4. Track in-flight transfers and signal completion
 *
 * The TX descriptor encodes everything the hardware needs:
 *   - Packet length, header offset
 *   - MACID (station index), queue selection
 *   - Data rate, bandwidth, security type
 *   - Aggregation, retry limits, TX power offset
 *
 * TX buffer lifecycle:
 *   The caller provides the raw 802.11 frame. This module prepends the
 *   40-byte descriptor, copies the combined buffer into a USB transfer
 *   buffer, and submits it via queue_bulk(). The USB completion callback
 *   releases the transfer buffer back to the free pool.
 *
 * Reference: rtl8814a_xmit.c, hal/rtl8814a/rtl8814a_xmit.c in
 * ulli-kroll/rtl8814au.
 */
#ifndef RTL8814AU_TX_PATH_H
#define RTL8814AU_TX_PATH_H


#include <USB3.h>
#include <lock.h>

#include "RTL8814AU.h"


class RTL8814AURegisterIO;
class RTL8814AUTxPath;


// Per-transfer tracking structure. One of these is allocated for each
// in-flight USB bulk OUT transfer.
struct TxTransfer {
	uint8*			buffer;			// Pre-allocated USB transfer buffer
	uint32			bufferSize;		// Size of the allocated buffer
	bool			inUse;			// Whether this transfer is currently in-flight
	sem_id			completionSem;	// Signaled when the transfer completes
	RTL8814AUTxPath* owner;			// Back-pointer for the static USB callback
	uint32			pipeIndex;		// Pipe this slot belongs to (0..N-1)
	uint32			submitLength;	// Bytes handed to queue_bulk, so the
									// completion can report a short write
};

// Maximum number of simultaneous in-flight TX transfers per queue.
// Keeps memory usage bounded while allowing enough pipelining to
// saturate USB bandwidth.  16 per pipe (× 3 pipes × ~1.6 KB =
// ~80 KB total) gives ample headroom for a ping flood or DHCP
// burst without ever hitting the queue-full path.
static const uint32 kTxTransfersPerQueue = 16;
static const uint32 kTxTotalTransfers
	= kTxTransfersPerQueue * kBulkOutEndpointCount;


class RTL8814AUTxPath {
public:
								RTL8814AUTxPath(
									RTL8814AURegisterIO* registerIO,
									usb_module_info* usbModule,
									usb_device usbDevice,
									usb_pipe bulkOut[kBulkOutEndpointCount]);
								~RTL8814AUTxPath();

	// Post-construction check — returns B_OK if transfer buffers
	// were allocated and semaphores created successfully.
	status_t					InitCheck() const { return fInitStatus; }

	// Read the chip's per-queue "empty" flags.  A queue that never goes
	// empty after a submit means the MAC is holding the frame in its packet
	// buffer rather than putting it on the air — which is exactly what a
	// successful queue_bulk cannot tell us.
	uint16						ReadTxQueueEmpty();

	// Transmit a single frame. Builds the TX descriptor, selects the
	// appropriate bulk OUT endpoint, and submits the USB transfer.
	//
	// \param frameData   Pointer to the raw 802.11 frame (no descriptor)
	// \param frameLength Length of the frame in bytes
	// \param queueSelect Which hardware queue to use (TxQueueSelect enum)
	// \param dataRate    Data rate index for this frame
	// \param macID       MACID (station index, 0 for broadcast/management)
	// \param secType     Security type (kSecurityNone, kSecurityAESCCMP, etc.)
	// \param isBroadcast true if this is a broadcast/multicast frame
	// \return B_OK on success, or an error code.
	status_t					Transmit(const uint8* frameData,
									uint32 frameLength,
									TxQueueSelect queueSelect,
									uint8 dataRate,
									uint8 macID,
									SecurityType secType,
									bool isBroadcast);

	// Send a firmware download chunk on the beacon bulk OUT endpoint.
	//
	// Wraps the data in a minimal 40-byte TX descriptor with QSEL =
	// kQslBeacon (0x10), then submits it synchronously on pipe 2
	// (kTxQueueBCN).  Used by the firmware loader; the chunk lands in
	// the chip's TX packet buffer at a location determined by the
	// beacon-queue page boundary, from which IDDMA transfers it to
	// MCU memory.
	//
	// \param data    Firmware bytes (up to kFwPageSize = 4 KB)
	// \param length  Number of bytes in \a data
	// \return B_OK when the USB transfer completes successfully.
	status_t					SendFirmwareChunk(const uint8* data,
										uint32 length);

	// Clear ENDPOINT_HALT on the beacon-queue bulk OUT pipe (pipe 0).
	// Hypothesis: the chip's EP2 may be in a STALL state after a prior
	// session or control-transfer sequence, which would explain why the
	// first firmware bulk OUT transfer hangs with no chip-side activity.
	// Called right before the firmware download begins.
	status_t					ClearBeaconPipeHalt();

	// Cancel all pending TX transfers. Called during shutdown or
	// device removal to clean up in-flight USB operations.
	void						CancelAll();

	// Restart the capped per-pipe traces, so each association gets its own
	// allowance of log lines rather than spending it all at boot.
	void						ResetTraces();

	// TX statistics
	uint32						FramesSent() const { return fFramesSent; }
	uint32						FramesFailed() const
									{ return fFramesFailed; }

private:
	// Build the 40-byte TX descriptor into the provided buffer.
	void						_BuildDescriptor(uint8* descriptor,
									const uint8* frameData,
									uint32 frameLength,
									TxQueueSelect queueSelect,
									uint8 dataRate,
									uint8 macID,
									SecurityType secType,
									bool isBroadcast,
									uint32 packetOffset);

	// Map a TxQueueSelect value to a bulk OUT pipe index (0-2).
	static uint32				_QueueToPipeIndex(TxQueueSelect queue);

	// Find a free transfer slot for the given pipe. Returns the index
	// into fTransfers[], or -1 if all slots for that pipe are in use.
	int32						_FindFreeTransfer(uint32 pipeIndex);

	// Attempt to unstick a bulk OUT pipe whose transfers have stopped
	// completing.  Must be called with fLock NOT held: it issues USB
	// transfers, and the cancellations it triggers run the completion
	// callback, which takes fLock itself.
	void						_RecoverStalledPipe(uint32 pipeIndex);

	// USB bulk OUT completion callback — called by the USB bus manager
	// when a transfer finishes (success or failure).
	static void					_TxCallback(void* cookie,
									status_t status,
									void* data,
									size_t actualLength);

	RTL8814AURegisterIO*		fRegisterIO;
	usb_module_info*			fUSBModule;
	usb_device					fUSBDevice;
	usb_pipe					fBulkOut[kBulkOutEndpointCount];

	// Transfer tracking — pre-allocated buffers and state
	TxTransfer					fTransfers[kTxTotalTransfers];

	// Synchronization
	mutex						fLock;

	// Per-pipe "a slot was freed" semaphore.  Signaled by _TxCallback
	// every time a transfer completes; Transmit() acquires it when the
	// pipe has no free slot.  Signaling all pipes via one sem per pipe
	// avoids the lost-wakeup bug where waiting on one specific slot's
	// completion sem misses completions on the other slots.
	sem_id						fPipeSlotFree[kBulkOutEndpointCount];

	// TX sequence number.  Used only for QoS frames, which carry their
	// own sequence in descriptor dword 9; non-QoS frames set HWSEQ_EN
	// and let the hardware assign one.
	uint16						fSequenceNumber;

	// Statistics
	uint32						fDescriptorsLogged;
	uint32						fFramesSent;
	uint32						fFramesFailed;

	// Last time each pipe was given a recovery attempt, so a persistently
	// dead pipe cannot turn into a loop of resets.
	bigtime_t					fLastPipeRecovery[kBulkOutEndpointCount];

	status_t					fInitStatus;
};


#endif	// RTL8814AU_TX_PATH_H
