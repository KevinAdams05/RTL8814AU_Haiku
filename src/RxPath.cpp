/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * RxPath.cpp — RX data path implementation for RTL8814AU.
 *
 * Inbound frame flow:
 *   1. Start() submits kRxTransferCount (4) bulk IN buffers to the USB stack
 *   2. When data arrives, _RxCallback() fires with the aggregated data
 *   3. _ProcessTransfer() walks the aggregate, splitting on RX descriptors
 *   4. For each frame: parse descriptor, extract PHY status and RSSI,
 *      deliver payload via the registered callback
 *   5. The completed buffer is immediately re-submitted to keep the pipeline full
 *
 * De-aggregation:
 *   The RTL8814AU hardware packs multiple received frames into a single USB
 *   bulk IN transfer. Each frame has:
 *     - 24-byte RX descriptor (mandatory)
 *     - PHY status block (0–64 bytes, present if kRxDescPHYStatus bit set)
 *     - Frame payload (variable, length from descriptor)
 *     - Padding to 128-byte alignment (hardware requirement)
 *
 *   The driver walks the aggregate by reading the packet length and driver
 *   info size from each descriptor, advancing by the padded total.
 *
 * Reference: rtl8814a_rxdesc.c, rtl8814a_recv.c in ulli-kroll/rtl8814au.
 */

#include "RxPath.h"

#include <new>
#include <string.h>

#include <ByteOrder.h>
#include <KernelExport.h>
#include <OS.h>

#include "RegisterIO.h"


// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------


RTL8814AURxPath::RTL8814AURxPath(RTL8814AURegisterIO* registerIO,
	usb_module_info* usbModule, usb_device usbDevice, usb_pipe bulkIn)
	:
	fRegisterIO(registerIO),
	fUSBModule(usbModule),
	fUSBDevice(usbDevice),
	fBulkIn(bulkIn),
	fFrameCallback(NULL),
	fFrameCallbackCookie(NULL),
	fRunning(false),
	fSubmitsLogged(0),
	fTransfersCompleted(0),
	fFramesReceived(0),
	fFramesDropped(0),
	fCrcErrors(0),
	fInitStatus(B_NO_INIT)
{
	mutex_init(&fLock, "rtl8814au:rx");

	// Allocate receive buffers
	for (uint32 i = 0; i < kRxTransferCount; i++) {
		fBuffers[i] = new(std::nothrow) uint8[kUsbRxBufferSize];
		if (fBuffers[i] == NULL) {
			fInitStatus = B_NO_MEMORY;
			return;
		}
	}

	fInitStatus = B_OK;
	dprintf(RTL8814AU_DRIVER_NAME ": RX path initialized "
		"(%" B_PRIu32 " buffers × %" B_PRIu32 " bytes)\n",
		kRxTransferCount, kUsbRxBufferSize);
}


RTL8814AURxPath::~RTL8814AURxPath()
{
	Stop();

	for (uint32 i = 0; i < kRxTransferCount; i++) {
		delete[] fBuffers[i];
		fBuffers[i] = NULL;
	}

	mutex_destroy(&fLock);
}


// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------


/*! Register the callback that will receive parsed frames. Must be set
    before calling Start().
*/
void
RTL8814AURxPath::SetFrameCallback(RxFrameCallback callback, void* cookie)
{
	fFrameCallback = callback;
	fFrameCallbackCookie = cookie;
}


/*! Start the continuous receive loop. Submits all RX buffers to the USB
    bulk IN endpoint. Each completed transfer is processed and re-submitted
    in the callback.

    \return B_OK if all initial submissions succeeded.
*/
status_t
RTL8814AURxPath::Start()
{
	if (fRunning)
		return B_OK;

	if (fFrameCallback == NULL) {
		dprintf(RTL8814AU_DRIVER_NAME ": RX start failed — no callback\n");
		return B_NO_INIT;
	}

	dprintf(RTL8814AU_DRIVER_NAME ": starting RX receive loop\n");
	fRunning = true;

	// Submit all receive buffers to the USB stack
	for (uint32 i = 0; i < kRxTransferCount; i++) {
		status_t status = _SubmitTransfer(i);
		if (status != B_OK) {
			dprintf(RTL8814AU_DRIVER_NAME ": RX submit failed for "
				"buffer %" B_PRIu32 ": %s\n", i, strerror(status));
			Stop();
			return status;
		}
	}

	return B_OK;
}


/*! Stop the receive loop. Cancels all pending bulk IN transfers.
    After this returns, no more callbacks will fire.
*/
void
RTL8814AURxPath::Stop()
{
	if (!fRunning)
		return;

	dprintf(RTL8814AU_DRIVER_NAME ": stopping RX receive loop\n");
	fRunning = false;

	if (fBulkIn != 0)
		fUSBModule->cancel_queued_transfers(fBulkIn);
}


// ---------------------------------------------------------------------------
// Private implementation
// ---------------------------------------------------------------------------


/*! Parse the 24-byte RX descriptor into a structured RxFrameInfo.

    Descriptor layout:
      DWORD 0: packet length, CRC/ICV errors, driver info size, PHY status
      DWORD 1: MACID, TID, security type
      DWORD 2: sequence number, fragment number
      DWORD 3: RX rate, bandwidth
      DWORD 4-5: reserved / additional status
*/
void
RTL8814AURxPath::_ParseDescriptor(const uint8* descriptor, RxFrameInfo* info)
{
	const uint32* desc32 = reinterpret_cast<const uint32*>(descriptor);
	uint32 dword0 = B_LENDIAN_TO_HOST_INT32(desc32[0]);
	uint32 dword1 = B_LENDIAN_TO_HOST_INT32(desc32[1]);
	uint32 dword2 = B_LENDIAN_TO_HOST_INT32(desc32[2]);
	uint32 dword3 = B_LENDIAN_TO_HOST_INT32(desc32[3]);

	// DWORD 0
	info->packetLength = (dword0 & kRxDescPktLen_Mask)
		>> kRxDescPktLen_Shift;
	info->crcError = (dword0 & kRxDescCRC32_Err) != 0;
	info->icvError = (dword0 & kRxDescICV_Err) != 0;
	info->hasPhyStatus = (dword0 & kRxDescPHYStatus) != 0;
	// SWDEC = bit 27 of dword 0.  Chip sets this when it gave up on
	// hardware decrypt and is passing the still-encrypted payload up
	// to the host expecting software decrypt.
	info->swDecNeeded = (dword0 & (1u << 27)) != 0;

	// Chip appends a 4-byte 802.11 FCS after the frame body when
	// RCR_APPFCS is set (chip default).  Morrownr's RX path strips
	// it (`pattrib->pkt_len -= IEEE80211_FCS_LEN`); we do the same so
	// downstream consumers — including SW CCMP decrypt, which needs
	// the MIC at exactly `packetLength - 8` — see the right end of
	// the frame.  The FCS bytes are still present in the buffer past
	// `packetLength`, just unused.
	if (info->packetLength >= 4)
		info->packetLength -= 4;

	// DWORD 1
	info->macID = (uint8)((dword1 & kRxDescMACID_Mask)
		>> kRxDescMACID_Shift);
	info->tid = (uint8)((dword1 & kRxDescTID_Mask) >> kRxDescTID_Shift);
	info->securityType = (SecurityType)((dword1 & kRxDescSecType_Mask)
		>> kRxDescSecType_Shift);

	// DWORD 2
	info->sequenceNumber = (uint16)((dword2 & kRxDescSeq_Mask)
		>> kRxDescSeq_Shift);
	info->fragmentNumber = (uint8)((dword2 & kRxDescFrag_Mask)
		>> kRxDescFrag_Shift);

	// DWORD 3
	info->dataRate = (uint8)((dword3 & kRxDescRxRate_Mask)
		>> kRxDescRxRate_Shift);
	info->bandwidth = (uint8)((dword3 & kRxDescBW_Mask)
		>> kRxDescBW_Shift);

	// RSSI will be filled in from PHY status if available
	info->rssi = 0;
	for (uint32 i = 0; i < kRfPathCount; i++)
		info->rssiPerPath[i] = 0;
}


/*! Parse the PHY status block to extract RSSI values.

    The PHY status block immediately follows the RX descriptor and contains
    per-path signal strength and quality measurements. The block size is
    encoded in the RX descriptor's DrvInfoSize field (in 8-byte units).

    For the RTL8814AU with 4 RF paths, the block contains 4 independent
    RSSI measurements (one per antenna path). The combined RSSI is the
    average of the active paths.
*/
void
RTL8814AURxPath::_ParsePhyStatus(const uint8* phyStatus,
	uint32 phyStatusSize, RxFrameInfo* info)
{
	if (phyStatusSize < 4)
		return;

	// The PHY status format depends on the modulation type:
	//   Byte 0: PHY status type (0 = CCK, 1 = OFDM, 2 = HT/VHT)
	// For OFDM/HT/VHT, per-path RSSI is at bytes 4-7 (paths A-D).
	// Values are raw unsigned; conversion: RSSI_dBm = raw - 110

	uint8 phyType = phyStatus[0];

	if (phyType == 0) {
		// CCK — single RSSI value at byte 1
		info->rssi = (int8)(phyStatus[1]) - 110;
		for (uint32 i = 0; i < kRfPathCount; i++)
			info->rssiPerPath[i] = info->rssi;
	} else {
		// OFDM / HT / VHT — per-path RSSI at bytes 4-7
		if (phyStatusSize >= 8) {
			int32 total = 0;
			uint32 activePaths = 0;
			for (uint32 i = 0; i < kRfPathCount; i++) {
				uint8 raw = phyStatus[4 + i];
				if (raw > 0) {
					info->rssiPerPath[i] = (int8)(raw) - 110;
					total += info->rssiPerPath[i];
					activePaths++;
				} else {
					info->rssiPerPath[i] = -110;
				}
			}
			if (activePaths > 0)
				info->rssi = (int8)(total / (int32)activePaths);
			else
				info->rssi = -110;
		}
	}
}


/*! Process a completed USB bulk IN transfer. Walks the aggregated data,
    splitting on RX descriptors, and delivers each frame via the callback.

    \param data    Pointer to the raw USB transfer data
    \param length  Total bytes in the transfer
*/
void
RTL8814AURxPath::_ProcessTransfer(const uint8* data, uint32 length)
{
	uint32 offset = 0;

	while (offset + kRxDescSize <= length) {
		// Parse the RX descriptor at the current position
		RxFrameInfo info;
		_ParseDescriptor(data + offset, &info);

		// Extract the driver info size (PHY status block) from DWORD 0.
		// The DrvInfoSize field is in 8-byte units.
		const uint32* desc32 = reinterpret_cast<const uint32*>(
			data + offset);
		uint32 dword0 = B_LENDIAN_TO_HOST_INT32(desc32[0]);
		uint32 drvInfoSize = ((dword0 & kRxDescDrvInfoSize_Mask)
			>> kRxDescDrvInfoSize_Shift) * 8;

		// Calculate where the payload starts and its total frame size
		// with alignment padding
		// On the 8814AU, when descriptor's physt bit is set, the chip
		// prepends a fixed 32-byte PHY status block before the 802.11
		// frame, regardless of what the descriptor's drvinfo_sz field
		// reports (which may be 0 even with PHY status present).
		uint32 phyStatusBytes = info.hasPhyStatus ? 32 : drvInfoSize;
		uint32 headerSize = kRxDescSize + phyStatusBytes;
		uint32 payloadOffset = offset + headerSize;
		uint32 payloadLength = info.packetLength;

		// The shift field indicates additional bytes to skip before payload
		uint32 shift = (dword0 & kRxDescShift_Mask) >> kRxDescShift_Shift;
		payloadOffset += shift;

		// Bounds check — make sure the frame fits within the transfer
		if (payloadOffset + payloadLength > length) {
			dprintf(RTL8814AU_DRIVER_NAME ": RX frame extends beyond "
				"transfer (offset %" B_PRIu32 ", payload %" B_PRIu32
				", transfer %" B_PRIu32 ")\n",
				payloadOffset, payloadLength, length);
			fFramesDropped++;
			break;
		}

		// Parse PHY status if present
		if (info.hasPhyStatus && drvInfoSize > 0) {
			_ParsePhyStatus(data + offset + kRxDescSize,
				phyStatusBytes, &info);
		}

		// Check for errors
		if (info.crcError) {
			fCrcErrors++;
			// Skip CRC-errored frames — don't deliver to stack
		} else if (info.icvError) {
			fFramesDropped++;
			// ICV error = decryption failure — drop
		} else if (payloadLength > 0 && fFrameCallback != NULL) {
			// Deliver the valid frame to the registered callback
			static uint32 sDescDumps = 0;
			if (sDescDumps < 4) {
				dprintf(RTL8814AU_DRIVER_NAME ": rxdesc[%u] dword0=0x%08x phyStatus=%u drvInfo=%u shift=%u pktLen=%u payOff=%u\n",
					(unsigned)sDescDumps, (unsigned)dword0,
					(unsigned)info.hasPhyStatus, (unsigned)drvInfoSize,
					(unsigned)shift, (unsigned)payloadLength,
					(unsigned)(payloadOffset - offset));
				sDescDumps++;
			}
			fFrameCallback(fFrameCallbackCookie,
				data + payloadOffset, payloadLength, &info);
			fFramesReceived++;
		}

		// Advance to the next frame in the aggregate.
		// Frames are aligned to 128-byte boundaries (kTxPageSize).
		uint32 totalFrameSize = headerSize + shift + payloadLength;
		uint32 aligned = (totalFrameSize + kTxPageSize - 1)
			& ~(kTxPageSize - 1);
		offset += aligned;
	}
}


/*! Clear any ENDPOINT_HALT condition on the bulk-IN endpoint.  If the
    pipe is stalled at start of day, the chip will accept submitted URBs
    but never deliver data on them, exactly mirroring the bulk-OUT halt
    we already clear during firmware load.
*/
status_t
RTL8814AURxPath::ClearHalt()
{
	status_t st = fUSBModule->clear_feature(fBulkIn,
		USB_FEATURE_ENDPOINT_HALT);
	dprintf(RTL8814AU_DRIVER_NAME ": clear_feature(ENDPOINT_HALT) on "
		"bulk IN returned %s\n", strerror(st));
	return st;
}


/*! Submit (or re-submit) a receive buffer to the USB bulk IN endpoint.

    \param index  Buffer index (0..kRxTransferCount-1)
    \return B_OK on success.
*/
status_t
RTL8814AURxPath::_SubmitTransfer(uint32 index)
{
	status_t st = fUSBModule->queue_bulk(fBulkIn, fBuffers[index],
		kUsbRxBufferSize, _RxCallback, this);
	if (fSubmitsLogged < 8) {
		dprintf(RTL8814AU_DRIVER_NAME ": _SubmitTransfer(%" B_PRIu32 ") -> %s\n",
			index, strerror(st));
		fSubmitsLogged++;
	}
	return st;
}


/*! USB bulk IN completion callback. Called by the USB bus manager when
    data arrives on the bulk IN endpoint.

    \param cookie       Pointer to the RTL8814AURxPath instance
    \param status       B_OK on success, B_CANCELED if stopped,
                        B_DEV_NOT_READY if device removed
    \param data         Pointer to the receive buffer
    \param actualLength Number of bytes received
*/
void
RTL8814AURxPath::_RxCallback(void* cookie, status_t status, void* data,
	size_t actualLength)
{
	RTL8814AURxPath* rxPath = static_cast<RTL8814AURxPath*>(cookie);
	if (rxPath == NULL)
		return;

	rxPath->fTransfersCompleted++;
	// At ~225 callbacks/sec on this hardware, even every-64th is
	// ~3.5 syslog lines/sec, which contributed to the BFS log-rotation
	// panic.  Log first 8 to confirm RX comes alive at boot, then
	// only every 4096 (~every 18 sec) as a heartbeat.
	if (rxPath->fTransfersCompleted <= 8
		|| (rxPath->fTransfersCompleted & 4095) == 0) {
		dprintf(RTL8814AU_DRIVER_NAME ": RX cb #%" B_PRIu32 " status=%s len=%" B_PRIuSIZE " frames=%" B_PRIu32 " crc=%" B_PRIu32 " drop=%" B_PRIu32 "\n",
			rxPath->fTransfersCompleted, strerror(status), actualLength,
			rxPath->fFramesReceived, rxPath->fCrcErrors, rxPath->fFramesDropped);
	}

	// If the transfer was canceled or the device removed, don't re-submit
	if (status == B_CANCELED || status == B_DEV_NOT_READY) {
		return;
	}

	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": RX callback error: %s\n",
			strerror(status));
		// Re-submit on transient errors to keep the pipeline alive
	}

	// Process the received data if we got any
	if (status == B_OK && actualLength > kRxDescSize) {
		rxPath->_ProcessTransfer(static_cast<const uint8*>(data),
			(uint32)actualLength);
	}

	// Re-submit this buffer if we're still running.
	// Find which buffer index this is by comparing pointers.
	if (rxPath->fRunning) {
		for (uint32 i = 0; i < kRxTransferCount; i++) {
			if (rxPath->fBuffers[i] == data) {
				status_t resubmit = rxPath->_SubmitTransfer(i);
				if (resubmit != B_OK) {
					dprintf(RTL8814AU_DRIVER_NAME ": RX re-submit "
						"failed: %s\n", strerror(resubmit));
				}
				break;
			}
		}
	}
}
