/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the GNU General Public License version 2.
 *
 * RegisterIO.h — Hardware register access for the RTL8814AU.
 *
 * All RTL8814AU registers are accessed via USB vendor-specific control
 * transfers. This class wraps the raw USB send_request() calls into
 * typed read/write operations with error handling and retry logic.
 *
 * USB control transfer format for register access:
 *   bmRequestType: 0xC0 (vendor, device-to-host) for reads
 *                  0x40 (vendor, host-to-device) for writes
 *   bRequest:      0x05
 *   wValue:        register address
 *   wIndex:        0
 *   wLength:       1, 2, or 4 bytes
 */
#ifndef RTL8814AU_REGISTER_IO_H
#define RTL8814AU_REGISTER_IO_H


#include <USB3.h>
#include <lock.h>

#include "RTL8814AU.h"


class RTL8814AURegisterIO {
public:
								RTL8814AURegisterIO(
									usb_device device,
									usb_module_info* usbModule);
								~RTL8814AURegisterIO();

	// Single-register read operations. Return the register value on
	// success, or 0xFF/0xFFFF/0xFFFFFFFF on failure (matching the
	// convention for invalid PCIe reads, which is also useful for USB
	// since a disconnected device returns all-ones).
	uint8						Read8(uint16 address);
	uint16						Read16(uint16 address);
	uint32						Read32(uint16 address);

	// Single-register write operations. Return B_OK on success.
	status_t					Write8(uint16 address, uint8 value);
	status_t					Write16(uint16 address, uint16 value);
	status_t					Write32(uint16 address, uint32 value);

	// Write with a deadline, for paths where blocking forever is worse than
	// failing. The synchronous writes above go through the USB stack's
	// send_request, which has no timeout: on a device that accepts a control
	// transfer and never completes it, the calling thread blocks permanently.
	// That was the driver's largest failure -- a post-association H2C command
	// took its worker thread down with it and left 5 GHz failing two joins in
	// three.
	status_t					WriteBounded32(uint16 address, uint32 value,
									bigtime_t timeout);

	// Bulk data write: writes N consecutive bytes to the given address
	// in a single USB control transfer. No byte-order conversion is
	// performed — data is written as raw bytes. Used for firmware
	// download where the reference driver sends 254-byte blocks.
	status_t					WriteN(uint16 address,
									const void* buffer, uint16 length);

	// Masked write: read current value, clear bits in mask, set new
	// bits. Useful for modifying individual fields without disturbing
	// adjacent bits.
	status_t					MaskedWrite8(uint16 address, uint8 mask,
									uint8 value);
	status_t					MaskedWrite16(uint16 address, uint16 mask,
									uint16 value);
	status_t					MaskedWrite32(uint16 address, uint32 mask,
									uint32 value);

	// Polling: read a register repeatedly until (value & mask) ==
	// expected, or until maxAttempts is reached (with delayPerAttempt
	// microseconds between each). Returns B_OK if matched, B_TIMED_OUT
	// if not.
	status_t					PollFor8(uint16 address, uint8 mask,
									uint8 expected, uint32 maxAttempts,
									bigtime_t delayPerAttempt);
	status_t					PollFor32(uint16 address, uint32 mask,
									uint32 expected, uint32 maxAttempts,
									bigtime_t delayPerAttempt);

	// Bulk register write from a table. Used during PHY/RF
	// initialization to program large sequences of register values.
	struct RegisterValue {
		uint16	address;
		uint32	value;
	};

	status_t					WriteTable(const RegisterValue* table,
									uint32 count);

	// Check if the device is still connected (not removed).
	bool						IsDevicePresent() const
									{ return fDevicePresent; }
	void						SetDeviceRemoved()
									{ fDevicePresent = false; }

private:
	// Raw USB control transfer helpers
	status_t					_VendorRead(uint16 address, void* buffer,
									uint16 length);
	static void					_ControlCallback(void* cookie,
									status_t status, void* data,
									size_t actualLength);

	status_t					_VendorWriteBounded(uint16 address,
									const void* buffer, uint16 length,
									bigtime_t timeout);

	status_t					_VendorWrite(uint16 address,
									const void* buffer, uint16 length);

	usb_device					fDevice;
	usb_module_info*			fUSBModule;
	// Serialises every vendor control transfer, not just the read-modify-write
	// helpers it originally guarded.
	//
	// Two reasons it has to cover all of them. Atomicity: MaskedWrite* was
	// protected against other MaskedWrite* calls but not against a plain
	// Write*, so a concurrent write could still land in the middle of a
	// read-modify-write. And exclusivity: only one control transfer being
	// outstanding at a time is the precondition that makes
	// cancel_queued_requests() safe, since that call is device-wide and would
	// otherwise abort transfers belonging to other threads. Haiku's own
	// usb_raw relies on exactly this.
	//
	// Recursive because MaskedWrite* takes it and then calls the public
	// Read*/Write*, which now take it too; a plain mutex would deadlock.
	recursive_lock				fLock;

	// State for a bounded transfer. These are members rather than locals on
	// purpose: an outstanding request points at the buffer and signals the
	// semaphore, and both must outlive any single call. fLock guarantees only
	// one bounded transfer is in flight, and the call does not return until
	// that transfer has completed or been cancelled and reaped, so nothing is
	// ever reused underneath the USB stack.
	sem_id						fControlDone;
	status_t					fControlStatus;
	// A ring rather than one buffer. When a transfer times out it is
	// *abandoned*, not cancelled -- cancel_queued_requests() was measured not
	// to reclaim a stuck transfer on this chip, its callback simply never
	// arrives -- so the request stays outstanding and keeps pointing at
	// whichever buffer it was given. Rotating means the next few transfers
	// cannot overwrite it, which would otherwise let an abandoned write
	// deliver the wrong bytes to a register if it ever did complete.
	static const uint32			kControlBuffers = 4;
	uint8						fControlBuffer[kControlBuffers]
									[sizeof(uint32)];
	uint32						fControlBufferIndex;
	bool						fDevicePresent;
};


#endif	// RTL8814AU_REGISTER_IO_H
