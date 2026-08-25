/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * RegisterIO.cpp — USB vendor control transfer register access for RTL8814AU.
 *
 * The RTL8814AU is a USB device with no memory-mapped I/O. All register
 * access goes through USB control transfers:
 *
 *   Read:  bmRequestType = 0xC0 (vendor, device-to-host)
 *   Write: bmRequestType = 0x40 (vendor, host-to-device)
 *   bRequest = 0x05
 *   wValue   = register address
 *   wIndex   = 0
 *   wLength  = 1, 2, or 4 (access width)
 *
 * Registers are little-endian. Multi-byte reads/writes must be atomic from
 * the hardware's perspective — the USB control transfer guarantees this.
 *
 * This module provides the Read8/16/32 and Write8/16/32 interface used by
 * all other driver modules (firmware loader, PHY config, TX/RX paths, etc.).
 */

#include "RegisterIO.h"

#include <string.h>

#include <ByteOrder.h>
#include <KernelExport.h>
#include <OS.h>
#include <util/AutoLock.h>


// USB vendor request code for register I/O. This is the bRequest value
// used in all control transfers to/from the RTL8814AU's register space.
static const uint8 kVendorRequestCode = 0x05;

// Maximum number of retry attempts for a failed USB control transfer
// before giving up. Transient USB errors (NAK, timeout) can happen
// during heavy traffic or immediately after resume from suspend.
static const uint32 kMaxRetryCount = 3;

// Attempts per bounded control write.
//
// One, on measured evidence, despite the Realtek reference allowing ten.
// Retrying was tried twice and does not work on this chip: immediately after
// the timeout it succeeded 0 times out of 2, and with a 100 ms pause in
// between it succeeded **0 times out of 8** across 12 timeouts. Requests to a
// control endpoint complete in order, so one issued behind a transfer that is
// still stuck simply queues behind it, and no delay or attempt count changes
// that.
//
// Retrying therefore bought nothing and cost up to 1.2 seconds per failed
// command while holding a lock that serialises every register access in the
// driver. Failing immediately is strictly better. The loop is left in place
// rather than unpicked, because the moment someone finds a way to clear a
// stuck control endpoint -- which Haiku currently offers no means of doing --
// raising this constant is the whole change.
//
// A timeout is usually survivable in any case: 12 of them produced only 5
// failed post-association setups, and 13 of 16 joins succeeded regardless.
static const uint32 kControlWriteAttempts = 1;

// Pause between attempts at a bounded control write. Kept only because the
// retry loop is kept; see kControlWriteAttempts for why there is now just one
// attempt and this is therefore unused in practice.
static const bigtime_t kControlRetryDelay = 100000;	// 100 ms


RTL8814AURegisterIO::RTL8814AURegisterIO(usb_device device,
	usb_module_info* usbModule)
	:
	fDevice(device),
	fUSBModule(usbModule),
	fControlDone(-1),
	fControlStatus(B_ERROR),
	fControlBufferIndex(0),
	fDevicePresent(true)
{
	recursive_lock_init(&fLock, "rtl8814au:register_io");
	memset(fControlBuffer, 0, sizeof(fControlBuffer));

	// Created once and reused. A semaphore per transfer would have to be
	// deleted while a cancelled request might still signal it.
	fControlDone = create_sem(0, "rtl8814au:control_done");
	if (fControlDone < 0) {
		dprintf(RTL8814AU_DRIVER_NAME ": could not create the control "
			"semaphore (%s); bounded writes will fall back to blocking "
			"ones\n", strerror(fControlDone));
	}
}


RTL8814AURegisterIO::~RTL8814AURegisterIO()
{
	if (fControlDone >= 0)
		delete_sem(fControlDone);
	recursive_lock_destroy(&fLock);
}


// ---------------------------------------------------------------------------
// Public read/write operations
// ---------------------------------------------------------------------------


/*! Read an 8-bit register.
    \param address  Register address (0x0000–0x3FFF)
    \return Register value, or 0xFF if the device is disconnected or the
            transfer failed.
*/
uint8
RTL8814AURegisterIO::Read8(uint16 address)
{
	uint8 value = 0xFF;
	status_t status = _VendorRead(address, &value, sizeof(value));
	if (status != B_OK)
		return 0xFF;
	return value;
}


/*! Read a 16-bit register.
    \param address  Register address (must be 2-byte aligned)
    \return Register value in host byte order, or 0xFFFF on failure.
*/
uint16
RTL8814AURegisterIO::Read16(uint16 address)
{
	uint16 value = 0xFFFF;
	status_t status = _VendorRead(address, &value, sizeof(value));
	if (status != B_OK)
		return 0xFFFF;
	// RTL8814AU registers are little-endian; convert to host byte order
	return B_LENDIAN_TO_HOST_INT16(value);
}


/*! Read a 32-bit register.
    \param address  Register address (must be 4-byte aligned)
    \return Register value in host byte order, or 0xFFFFFFFF on failure.
*/
uint32
RTL8814AURegisterIO::Read32(uint16 address)
{
	uint32 value = 0xFFFFFFFF;
	status_t status = _VendorRead(address, &value, sizeof(value));
	if (status != B_OK)
		return 0xFFFFFFFF;
	return B_LENDIAN_TO_HOST_INT32(value);
}


/*! Write an 8-bit register.
    \param address  Register address
    \param value    Value to write
    \return B_OK on success, or an error code.
*/
status_t
RTL8814AURegisterIO::Write8(uint16 address, uint8 value)
{
	return _VendorWrite(address, &value, sizeof(value));
}


/*! Write a 16-bit register.
    \param address  Register address (must be 2-byte aligned)
    \param value    Value to write (in host byte order; converted internally)
    \return B_OK on success, or an error code.
*/
status_t
RTL8814AURegisterIO::Write16(uint16 address, uint16 value)
{
	uint16 leValue = B_HOST_TO_LENDIAN_INT16(value);
	return _VendorWrite(address, &leValue, sizeof(leValue));
}


/*! Write a 32-bit register.
    \param address  Register address (must be 4-byte aligned)
    \param value    Value to write (in host byte order; converted internally)
    \return B_OK on success, or an error code.
*/
status_t
RTL8814AURegisterIO::Write32(uint16 address, uint32 value)
{
	uint32 leValue = B_HOST_TO_LENDIAN_INT32(value);
	return _VendorWrite(address, &leValue, sizeof(leValue));
}


/*! Write N consecutive bytes to a register/memory address in a single USB
    vendor control transfer. No byte-order conversion is performed — data
    is written as raw bytes.

    Used primarily for firmware download, where the reference driver sends
    data in 254-byte blocks (MAX_REG_BOLCK_SIZE for USB) via rtw_writeN().

    \param address  Starting register/memory address
    \param buffer   Data to write (raw bytes, no byte-order conversion)
    \param length   Number of bytes (1–254 recommended for USB control xfers)
    \return B_OK on success, or an error code.
*/
status_t
RTL8814AURegisterIO::WriteN(uint16 address, const void* buffer, uint16 length)
{
	return _VendorWrite(address, buffer, length);
}


// ---------------------------------------------------------------------------
// Masked write operations — read-modify-write with bit-level granularity
// ---------------------------------------------------------------------------


/*! Read an 8-bit register, clear the bits in mask, set new bits from value.
    Equivalent to: reg = (reg & ~mask) | (value & mask)
*/
status_t
RTL8814AURegisterIO::MaskedWrite8(uint16 address, uint8 mask, uint8 value)
{
	RecursiveLocker locker(fLock);
	uint8 current = Read8(address);
	uint8 updated = (current & ~mask) | (value & mask);
	return Write8(address, updated);
}


/*! Read a 16-bit register, clear the bits in mask, set new bits from value. */
status_t
RTL8814AURegisterIO::MaskedWrite16(uint16 address, uint16 mask, uint16 value)
{
	RecursiveLocker locker(fLock);
	uint16 current = Read16(address);
	uint16 updated = (current & ~mask) | (value & mask);
	return Write16(address, updated);
}


/*! Read a 32-bit register, clear the bits in mask, set new bits from value. */
status_t
RTL8814AURegisterIO::MaskedWrite32(uint16 address, uint32 mask, uint32 value)
{
	RecursiveLocker locker(fLock);
	uint32 current = Read32(address);
	uint32 updated = (current & ~mask) | (value & mask);
	return Write32(address, updated);
}


// ---------------------------------------------------------------------------
// Polling operations — wait for a register to reach an expected value
// ---------------------------------------------------------------------------


/*! Poll an 8-bit register until (reg & mask) == expected.
    \param address          Register to poll
    \param mask             Bits to check
    \param expected         Value those bits should have
    \param maxAttempts      Number of poll iterations before timeout
    \param delayPerAttempt  Microseconds to sleep between attempts
    \return B_OK if the condition was met, B_TIMED_OUT otherwise.
*/
status_t
RTL8814AURegisterIO::PollFor8(uint16 address, uint8 mask, uint8 expected,
	uint32 maxAttempts, bigtime_t delayPerAttempt)
{
	for (uint32 i = 0; i < maxAttempts; i++) {
		uint8 value = Read8(address);
		if ((value & mask) == expected)
			return B_OK;

		if (!fDevicePresent)
			return B_DEV_NOT_READY;

		snooze(delayPerAttempt);
	}

	dprintf(RTL8814AU_DRIVER_NAME ": PollFor8(0x%04x) timed out - "
		"expected 0x%02x, mask 0x%02x\n", address, expected, mask);
	return B_TIMED_OUT;
}


/*! Poll a 32-bit register until (reg & mask) == expected. */
status_t
RTL8814AURegisterIO::PollFor32(uint16 address, uint32 mask, uint32 expected,
	uint32 maxAttempts, bigtime_t delayPerAttempt)
{
	for (uint32 i = 0; i < maxAttempts; i++) {
		uint32 value = Read32(address);
		if ((value & mask) == expected)
			return B_OK;

		if (!fDevicePresent)
			return B_DEV_NOT_READY;

		snooze(delayPerAttempt);
	}

	dprintf(RTL8814AU_DRIVER_NAME ": PollFor32(0x%04x) timed out - "
		"expected 0x%08" B_PRIx32 ", mask 0x%08" B_PRIx32 "\n",
		address, expected, mask);
	return B_TIMED_OUT;
}


// ---------------------------------------------------------------------------
// Bulk register table write
// ---------------------------------------------------------------------------


/*! Write a sequence of register values from a table. Used during
    PHY/RF initialization to program long configuration sequences.
    Stops on the first error.

    \param table  Array of {address, value} pairs
    \param count  Number of entries in the table
    \return B_OK if all writes succeeded, or the first error encountered.
*/
status_t
RTL8814AURegisterIO::WriteTable(const RegisterValue* table, uint32 count)
{
	for (uint32 i = 0; i < count; i++) {
		status_t status = Write32(table[i].address, table[i].value);
		if (status != B_OK) {
			dprintf(RTL8814AU_DRIVER_NAME ": WriteTable failed at entry "
				"%" B_PRIu32 " (addr 0x%04x): %s\n",
				i, table[i].address, strerror(status));
			return status;
		}
	}
	return B_OK;
}


// ---------------------------------------------------------------------------
// Private USB control transfer helpers
// ---------------------------------------------------------------------------


/*! Perform a USB vendor-specific control transfer to read from a register.
    Retries up to kMaxRetryCount times on transient errors.

    \param address  Register address (placed in wValue)
    \param buffer   Output buffer
    \param length   Number of bytes to read (1, 2, or 4)
    \return B_OK on success, or USB error code.
*/
status_t
RTL8814AURegisterIO::_VendorRead(uint16 address, void* buffer, uint16 length)
{
	if (!fDevicePresent)
		return B_DEV_NOT_READY;

	RecursiveLocker locker(fLock);

	for (uint32 attempt = 0; attempt < kMaxRetryCount; attempt++) {
		size_t actualLength = length;
		status_t status = fUSBModule->send_request(
			fDevice,
			USB_REQTYPE_VENDOR | USB_REQTYPE_DEVICE_IN,	// 0xC0
			kVendorRequestCode,								// bRequest = 0x05
			address,										// wValue = reg addr
			0,												// wIndex = 0
			length,											// wLength
			buffer,
			&actualLength);

		if (status == B_OK && actualLength == length)
			return B_OK;

		// Transient error — retry after a brief delay
		if (attempt < kMaxRetryCount - 1)
			snooze(1000);	// 1 ms
	}

	dprintf(RTL8814AU_DRIVER_NAME ": _VendorRead(0x%04x, %u) failed after "
		"%" B_PRIu32 " attempts\n", address, length, kMaxRetryCount);
	return B_IO_ERROR;
}


void
RTL8814AURegisterIO::_ControlCallback(void* cookie, status_t status,
	void* data, size_t actualLength)
{
	RTL8814AURegisterIO* self = static_cast<RTL8814AURegisterIO*>(cookie);
	if (self == NULL)
		return;

	self->fControlStatus = status;
	release_sem_etc(self->fControlDone, 1, B_DO_NOT_RESCHEDULE);
}


/*! Vendor write that gives up instead of blocking forever.

    Modelled on Haiku's usb_raw, the only in-tree driver that waits on an
    asynchronous control transfer: serialise with a lock, pass a long-lived
    object as the cookie rather than a stack local, and on giving up cancel and
    then wait for the callback. The one addition is the deadline. usb_raw waits
    indefinitely, and no Haiku driver bounds a control transfer by time, so
    that part has no in-tree precedent and is kept deliberately plain. The
    Realtek reference bounds every control transfer at 500 ms and retries,
    which is where the numbers come from.

    Why the lock matters beyond mutual exclusion: cancel_queued_requests() is
    device-wide. Holding fLock guarantees ours is the only control transfer
    outstanding, so cancelling cannot abort one another thread is still waiting
    on. An earlier attempt omitted the lock and was reverted after a KDL.

    The buffer and semaphore are members, and this does not return until the
    transfer has completed or been cancelled and reaped, so neither is reused
    while the USB stack still holds a reference.
*/
status_t
RTL8814AURegisterIO::_VendorWriteBounded(uint16 address, const void* buffer,
	uint16 length, bigtime_t timeout)
{
	if (!fDevicePresent)
		return B_DEV_NOT_READY;
	if (length > sizeof(fControlBuffer[0]))
		return B_BAD_VALUE;

	// Without a semaphore there is no way to wait with a deadline, and a
	// blocking write is still better than not writing at all.
	if (fControlDone < 0)
		return _VendorWrite(address, buffer, length);

	RecursiveLocker locker(fLock);

	// Retried, because bounding the wait only converts a hang into a failure
	// -- it does not deliver the command. Measured: the first attempt at
	// MEDIA_STATUS_RPT times out often enough to fail the association, which
	// is the same outcome as the hang it replaced, just survivable. The
	// Realtek reference retries every control transfer up to ten times for
	// exactly this reason.
	//
	// Fewer attempts than the reference uses, deliberately. Each one can cost
	// the full deadline, and this lock now serialises all register access, so
	// ten would mean holding it for five seconds.
	for (uint32 attempt = 1; attempt <= kControlWriteAttempts; attempt++) {

		// Drain any count left by a transfer that completed after we stopped
		// waiting for it, so this wait cannot be satisfied by stale news.
		while (acquire_sem_etc(fControlDone, 1, B_RELATIVE_TIMEOUT, 0) == B_OK)
			;

		uint8* attemptBuffer = fControlBuffer[fControlBufferIndex];
		fControlBufferIndex = (fControlBufferIndex + 1) % kControlBuffers;
		memcpy(attemptBuffer, buffer, length);
		fControlStatus = B_ERROR;

		status_t status = fUSBModule->queue_request(fDevice,
			USB_REQTYPE_VENDOR | USB_REQTYPE_DEVICE_OUT,
			kVendorRequestCode,
			address,
			0,
			length,
			attemptBuffer,
			_ControlCallback,
			this);
		if (status != B_OK)
			return status;

		status = acquire_sem_etc(fControlDone, 1, B_RELATIVE_TIMEOUT, timeout);
		if (status == B_TIMED_OUT) {
			dprintf(RTL8814AU_DRIVER_NAME ": control write to 0x%04x did not "
				"complete within %" B_PRIdBIGTIME " us; cancelling\n",
				address, timeout);

			// The transfer is abandoned rather than cancelled.
			//
			// cancel_queued_requests() was tried here first and measured not to
			// work: on both timeouts observed, the cancelled transfer's
			// callback never arrived within a two-second grace, so the code
			// fell through to marking the device unusable and the retry below
			// never ran at all. Cancellation cannot reclaim a stuck control
			// transfer on this chip.
			//
			// Abandoning is safe as long as nothing reuses what the
			// outstanding request points at, which is what the ring is for.
			// If its callback ever does arrive it releases the semaphore, and
			// the drain at the top of the next attempt discards that count.
			//
			// Note the cancel call was also device-wide, so it would have
			// aborted transfers belonging to other threads; not doing it at all
			// removes that hazard too.
			if (attempt < kControlWriteAttempts) {
				dprintf(RTL8814AU_DRIVER_NAME ": retrying control write to "
					"0x%04x (attempt %u of %u)\n", address,
					(unsigned)(attempt + 1), (unsigned)kControlWriteAttempts);

				// Wait before trying again. The first version retried
				// immediately and never once succeeded -- 0 of 2 --
				// which looked like proof that a stuck endpoint blocks
				// everything behind it. The measurements say otherwise:
				// three timeouts produced only one failed setup, so in
				// the other cases a later control write on the same
				// endpoint went through fine. The device answers again
				// shortly afterwards; it just does not answer instantly,
				// and an immediate retry asks at the one moment it is
				// certain to fail.
				snooze(kControlRetryDelay);
				continue;
			}
			return B_TIMED_OUT;
		}

		if (status != B_OK)
			return status;
		if (fControlStatus == B_OK) {
			if (attempt > 1) {
				dprintf(RTL8814AU_DRIVER_NAME ": control write to 0x%04x "
					"succeeded on attempt %u\n", address, (unsigned)attempt);
			}
			return B_OK;
		}

		// Completed with an error rather than timing out. Retry too: a NAK on
		// this chip is transient often enough that the synchronous path has
		// always retried.
		if (attempt == kControlWriteAttempts)
			return fControlStatus;

	}

	return B_TIMED_OUT;
}


/*! 32-bit register write with a deadline. See _VendorWriteBounded. */
status_t
RTL8814AURegisterIO::WriteBounded32(uint16 address, uint32 value,
	bigtime_t timeout)
{
	uint32 littleEndian = B_HOST_TO_LENDIAN_INT32(value);
	return _VendorWriteBounded(address, &littleEndian, sizeof(littleEndian),
		timeout);
}


/*! Perform a USB vendor-specific control transfer to write to a register
    or memory address. Retries up to kMaxRetryCount times on transient errors.

    \param address  Register/memory address (placed in wValue)
    \param buffer   Data to write
    \param length   Number of bytes to write (1–254 for USB control xfers)
    \return B_OK on success, or USB error code.
*/
status_t
RTL8814AURegisterIO::_VendorWrite(uint16 address, const void* buffer,
	uint16 length)
{
	if (!fDevicePresent)
		return B_DEV_NOT_READY;

	RecursiveLocker locker(fLock);

	status_t lastStatus = B_ERROR;

	for (uint32 attempt = 0; attempt < kMaxRetryCount; attempt++) {
		size_t actualLength = length;
		lastStatus = fUSBModule->send_request(
			fDevice,
			USB_REQTYPE_VENDOR | USB_REQTYPE_DEVICE_OUT,	// 0x40
			kVendorRequestCode,								// bRequest = 0x05
			address,										// wValue = reg addr
			0,												// wIndex = 0
			length,											// wLength
			(void*)buffer,
			&actualLength);

		if (lastStatus == B_OK)
			return B_OK;

		if (attempt < kMaxRetryCount - 1)
			snooze(2000);
	}

	dprintf(RTL8814AU_DRIVER_NAME ": _VendorWrite(0x%04x, %u) failed after "
		"%" B_PRIu32 " attempts (last status=0x%08" B_PRIx32 ")\n",
		address, length, kMaxRetryCount, (uint32)lastStatus);
	return B_IO_ERROR;
}
