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


RTL8814AURegisterIO::RTL8814AURegisterIO(usb_device device,
	usb_module_info* usbModule)
	:
	fDevice(device),
	fUSBModule(usbModule),
	fDevicePresent(true)
{
	mutex_init(&fLock, "rtl8814au:register_io");
}


RTL8814AURegisterIO::~RTL8814AURegisterIO()
{
	mutex_destroy(&fLock);
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
	MutexLocker locker(fLock);
	uint8 current = Read8(address);
	uint8 updated = (current & ~mask) | (value & mask);
	return Write8(address, updated);
}


/*! Read a 16-bit register, clear the bits in mask, set new bits from value. */
status_t
RTL8814AURegisterIO::MaskedWrite16(uint16 address, uint16 mask, uint16 value)
{
	MutexLocker locker(fLock);
	uint16 current = Read16(address);
	uint16 updated = (current & ~mask) | (value & mask);
	return Write16(address, updated);
}


/*! Read a 32-bit register, clear the bits in mask, set new bits from value. */
status_t
RTL8814AURegisterIO::MaskedWrite32(uint16 address, uint32 mask, uint32 value)
{
	MutexLocker locker(fLock);
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

	dprintf(RTL8814AU_DRIVER_NAME ": PollFor8(0x%04x) timed out — "
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

	dprintf(RTL8814AU_DRIVER_NAME ": PollFor32(0x%04x) timed out — "
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
