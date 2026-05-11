/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * RxPath.h — RX data path for the RTL8814AU.
 *
 * Handles the entire inbound frame pipeline:
 *   1. Submit USB bulk IN transfers to receive aggregated frames
 *   2. De-aggregate the USB transfer into individual frames
 *   3. Parse the 24-byte RX descriptor for each frame
 *   4. Deliver frame payloads to the network stack or WiFi management
 *
 * The RTL8814AU hardware aggregates multiple received frames into a single
 * USB bulk IN transfer for efficiency. Each frame within the aggregate has
 * its own 24-byte RX descriptor followed by an optional PHY status block
 * and the frame payload.
 *
 * Frame layout within a USB bulk IN transfer:
 *   [RX desc 24B][PHY status 0-64B][Frame payload][padding to 128B]
 *   [RX desc 24B][PHY status 0-64B][Frame payload][padding to 128B]
 *   ...
 *
 * The driver maintains a continuous receive loop: when a bulk IN transfer
 * completes, it is immediately re-submitted after processing. This keeps
 * the USB pipeline full so no frames are dropped.
 *
 * Reference: rtl8814a_rxdesc.c, hal/rtl8814a/rtl8814a_recv.c in
 * ulli-kroll/rtl8814au.
 */
#ifndef RTL8814AU_RX_PATH_H
#define RTL8814AU_RX_PATH_H


#include <USB3.h>
#include <lock.h>

#include "RTL8814AU.h"


class RTL8814AURegisterIO;


// Parsed RX frame metadata — extracted from the 24-byte hardware descriptor
// and PHY status block. Passed alongside the frame payload to consumers.
struct RxFrameInfo {
	uint32		packetLength;		// Payload length (excluding descriptor)
	uint8		dataRate;			// DataRateIndex of the received frame
	uint8		bandwidth;			// 0=20MHz, 1=40MHz, 2=80MHz
	int8		rssi;				// Combined RSSI in dBm (averaged paths)
	int8		rssiPerPath[kRfPathCount];	// Per-path RSSI (A/B/C/D)
	uint8		macID;				// Station MACID (0-127)
	uint8		tid;				// Traffic ID (QoS)
	uint16		sequenceNumber;		// 802.11 sequence number
	uint8		fragmentNumber;		// 802.11 fragment number
	SecurityType securityType;		// Encryption type used on this frame
	bool		crcError;			// true if CRC32 check failed
	bool		icvError;			// true if ICV check failed
	bool		hasPhyStatus;		// true if PHY status block is present
	bool		swDecNeeded;		// SWDEC bit (RX desc dword 0 bit 27):
									// chip says "I couldn't HW-decrypt
									// this; please software-decrypt".
									// When set on a Protected frame,
									// it means our CAM/SECCFG didn't
									// match what the chip expected.
};

// Callback type for frame delivery. The RX path calls this for each
// successfully received frame. The callback must copy or consume the
// frame data before returning — the buffer will be reused.
typedef void (*RxFrameCallback)(void* cookie, const uint8* frameData,
	uint32 frameLength, const RxFrameInfo* info);

// Number of pre-allocated RX transfer buffers. Multiple buffers keep
// the USB pipeline full — while one is being processed, another is
// waiting in the USB stack for incoming data.
static const uint32 kRxTransferCount = 4;


class RTL8814AURxPath {
public:
								RTL8814AURxPath(
									RTL8814AURegisterIO* registerIO,
									usb_module_info* usbModule,
									usb_device usbDevice,
									usb_pipe bulkIn);
								~RTL8814AURxPath();

	status_t					InitCheck() const { return fInitStatus; }

	// Set the callback that receives parsed frames. Must be called
	// before Start(). The cookie is passed through to each callback.
	void						SetFrameCallback(RxFrameCallback callback,
									void* cookie);

	// Start the receive loop — submits all RX transfer buffers to the
	// USB bulk IN endpoint. Frames are delivered via the callback.
	status_t					Start();
	status_t					ClearHalt();

	// Stop the receive loop — cancels all pending bulk IN transfers.
	void						Stop();

	bool						IsRunning() const { return fRunning; }

	// RX statistics
	uint32						FramesReceived() const
									{ return fFramesReceived; }
	uint32						FramesDropped() const
									{ return fFramesDropped; }
	uint32						CrcErrors() const { return fCrcErrors; }

private:
	// Parse a single RX descriptor from raw bytes into an RxFrameInfo.
	static void					_ParseDescriptor(const uint8* descriptor,
									RxFrameInfo* info);

	// Parse the PHY status block to extract per-path RSSI values.
	static void					_ParsePhyStatus(const uint8* phyStatus,
									uint32 phyStatusSize,
									RxFrameInfo* info);

	// Process a completed USB bulk IN transfer — de-aggregate and
	// deliver each frame within it.
	void						_ProcessTransfer(const uint8* data,
									uint32 length);

	// Re-submit a transfer buffer to the USB stack for the next receive.
	status_t					_SubmitTransfer(uint32 index);

	// USB bulk IN completion callback
	static void					_RxCallback(void* cookie,
									status_t status,
									void* data,
									size_t actualLength);

	RTL8814AURegisterIO*		fRegisterIO;
	usb_module_info*			fUSBModule;
	usb_device					fUSBDevice;
	usb_pipe					fBulkIn;

	// Pre-allocated receive buffers
	uint8*						fBuffers[kRxTransferCount];

	// Frame delivery callback
	RxFrameCallback				fFrameCallback;
	void*						fFrameCallbackCookie;

	// State
	bool						fRunning;
	mutex						fLock;

	// Statistics
	uint32						fSubmitsLogged;
	uint32						fTransfersCompleted;
	uint32						fFramesReceived;
	uint32						fFramesDropped;
	uint32						fCrcErrors;

	status_t					fInitStatus;
};


#endif	// RTL8814AU_RX_PATH_H
