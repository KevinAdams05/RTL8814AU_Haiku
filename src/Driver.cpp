/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Driver.cpp — Kernel module entry points for the RTL8814AU USB WiFi driver.
 *
 * This is the top-level driver file that the Haiku kernel loads. It:
 *   1. Acquires the USB bus manager module
 *   2. Registers USB device IDs we support
 *   3. Installs notification hooks for plug/unplug events
 *   4. Manages the device list and published device names
 *
 * The actual device lifecycle (hardware init, firmware load, data transfer)
 * is handled by the RTL8814AUDevice class in Device.cpp.
 */

#include "Driver.h"
#include "WiFiIoctl.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>
#include <lock.h>
#include <util/AutoLock.h>

#include "Device.h"


// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

// Haiku driver API version — must be exported for the kernel to accept us
int32 api_version = B_CUR_DRIVER_API_VERSION;

// USB bus manager module pointer — acquired during init_driver()
usb_module_info* gUSBModule = NULL;

// Net-notification module — best-effort load in init_driver().  When
// non-NULL we publish B_NETWORK_WLAN_SCANNED / _JOINED / _LEFT events
// through it; when NULL the events are simply dropped.
net_notifications_module_info* gNotificationModule = NULL;

// Device list: up to kMaxDeviceCount simultaneously attached adapters.
// Protected by gDeviceListLock.
mutex gDeviceListLock;
RTL8814AUDevice* gDeviceList[kMaxDeviceCount];
uint32 gDeviceCount = 0;

// Published device paths (rebuilt in publish_devices). We need a persistent
// array of C strings that survives until the next publish_devices() call.
static char* sDeviceNames[kMaxDeviceCount + 1];


// ---------------------------------------------------------------------------
// USB notification hooks
// ---------------------------------------------------------------------------


/*! Called by the USB bus manager when a device matching our
    usb_support_descriptor table is plugged in. We create a new
    RTL8814AUDevice instance and add it to the device list.

    \param device   USB device handle from the bus manager
    \param cookie   Output: opaque pointer stored by the bus manager and
                    passed back to DeviceRemoved()
    \return B_OK if we claimed the device, B_ERROR otherwise
*/
status_t
DeviceAdded(usb_device device, void** cookie)
{
	*cookie = NULL;

	// Check the device descriptor to confirm it matches a known RTL8814AU
	const usb_device_descriptor* descriptor
		= gUSBModule->get_device_descriptor(device);
	if (descriptor == NULL)
		return B_ERROR;

	// Find which entry in our table matches
	const char* deviceName = NULL;
	for (uint32 i = 0; i < kSupportedDeviceCount; i++) {
		if (descriptor->vendor_id == kSupportedDevices[i].vendorID
			&& descriptor->product_id == kSupportedDevices[i].productID) {
			deviceName = kSupportedDevices[i].name;
			break;
		}
	}

	if (deviceName == NULL) {
		// Should not happen — the bus manager only calls us for matching
		// descriptors — but guard against it.
		return B_ERROR;
	}

	dprintf(RTL8814AU_DRIVER_NAME ": device added — %s "
		"(vendor 0x%04x, product 0x%04x)\n",
		deviceName, descriptor->vendor_id, descriptor->product_id);

	// Find a free slot in the device list
	MutexLocker locker(gDeviceListLock);

	uint32 slotIndex = UINT32_MAX;
	for (uint32 i = 0; i < kMaxDeviceCount; i++) {
		if (gDeviceList[i] == NULL) {
			slotIndex = i;
			break;
		}
	}

	if (slotIndex == UINT32_MAX) {
		dprintf(RTL8814AU_DRIVER_NAME ": no free device slots\n");
		return B_NO_MEMORY;
	}

	// Create the device instance. The constructor sets up USB endpoints
	// and prepares the device for later initialization (which happens
	// on the first open() call).
	RTL8814AUDevice* rtlDevice
		= new(std::nothrow) RTL8814AUDevice(device, slotIndex, deviceName);
	if (rtlDevice == NULL)
		return B_NO_MEMORY;

	status_t status = rtlDevice->InitCheck();
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": device init failed: %s\n",
			strerror(status));
		delete rtlDevice;
		return status;
	}

	gDeviceList[slotIndex] = rtlDevice;
	gDeviceCount++;
	*cookie = rtlDevice;

	dprintf(RTL8814AU_DRIVER_NAME ": device %s claimed at slot %" B_PRIu32
		"\n", deviceName, slotIndex);

	return B_OK;
}


/*! Called by the USB bus manager when a device we claimed is unplugged.
    We mark the device as removed and clean up if nobody has it open.

    \param cookie   The opaque pointer we returned from DeviceAdded()
    \return B_OK always
*/
status_t
DeviceRemoved(void* cookie)
{
	RTL8814AUDevice* device = static_cast<RTL8814AUDevice*>(cookie);
	if (device == NULL)
		return B_OK;

	dprintf(RTL8814AU_DRIVER_NAME ": device removed — %s\n",
		device->Name());

	MutexLocker locker(gDeviceListLock);

	// Mark the device as removed so ongoing operations bail out
	device->SetRemoved();

	// If no one has the device open, delete it immediately.
	// Otherwise, the last close() call will clean up.
	if (!device->IsOpen()) {
		uint32 slot = device->SlotIndex();
		gDeviceList[slot] = NULL;
		gDeviceCount--;
		locker.Unlock();
		delete device;
	}

	return B_OK;
}


// ---------------------------------------------------------------------------
// Standard Haiku kernel driver exports
// ---------------------------------------------------------------------------


/*! Called once when the driver is first loaded. We just confirm the hardware
    type is plausible (USB exists on this machine).
    \return B_OK always — we cannot meaningfully probe USB at this stage.
*/
status_t
init_hardware()
{
	return B_OK;
}


/*! Called to initialize the driver module. We acquire the USB bus manager,
    register our supported device list, and install plug/unplug notification
    hooks.
    \return B_OK on success, or an error code if USB is unavailable.
*/
status_t
init_driver()
{
	dprintf(RTL8814AU_DRIVER_NAME ": init_driver() SIOCS80211=0x%08x SIOCG80211=0x%08x ireqsize=%lu\n", (unsigned)SIOCS80211, (unsigned)SIOCG80211, (unsigned long)sizeof(struct ieee80211req));

	// Initialize the device list
	memset(gDeviceList, 0, sizeof(gDeviceList));
	memset(sDeviceNames, 0, sizeof(sDeviceNames));
	mutex_init(&gDeviceListLock, RTL8814AU_DRIVER_NAME ":device_list");

	// Acquire the USB bus manager module — this gives us the API to
	// register for device notifications and perform USB transfers.
	status_t status = get_module(B_USB_MODULE_NAME,
		(module_info**)&gUSBModule);
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME
			": failed to get USB module: %s\n", strerror(status));
		mutex_destroy(&gDeviceListLock);
		return status;
	}

	// Build the USB support descriptor table from our device ID list.
	// Each entry matches by vendor + product ID (class fields are 0 =
	// wildcard since the RTL8814AU uses vendor-specific class codes).
	static usb_support_descriptor supportDescriptors[kSupportedDeviceCount];
	for (uint32 i = 0; i < kSupportedDeviceCount; i++) {
		supportDescriptors[i].dev_class = 0;
		supportDescriptors[i].dev_subclass = 0;
		supportDescriptors[i].dev_protocol = 0;
		supportDescriptors[i].vendor = kSupportedDevices[i].vendorID;
		supportDescriptors[i].product = kSupportedDevices[i].productID;
	}

	// Register with the USB bus manager. This tells it which devices we
	// want to be notified about.
	gUSBModule->register_driver(RTL8814AU_DRIVER_NAME,
		supportDescriptors, kSupportedDeviceCount, NULL);

	// Install notification hooks so we learn about plug/unplug events.
	static usb_notify_hooks notifyHooks = {
		DeviceAdded,
		DeviceRemoved
	};
	gUSBModule->install_notify(RTL8814AU_DRIVER_NAME, &notifyHooks);

	// Best-effort: load the net-notification module so scan-done /
	// connect / disconnect events reach userland.  Failure isn't fatal;
	// the device still works without notifications, callers just have
	// to poll for state changes.
	if (get_module(NET_NOTIFICATIONS_MODULE_NAME,
			(module_info**)&gNotificationModule) != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": net_notifications module not "
			"available, scan/connect events won't be delivered\n");
		gNotificationModule = NULL;
	}

	dprintf(RTL8814AU_DRIVER_NAME ": driver initialized, "
		"%" B_PRIu32 " supported devices registered\n",
		kSupportedDeviceCount);

	return B_OK;
}


/*! Called when the driver module is unloaded. Clean up all devices and
    release the USB module.
*/
void
uninit_driver()
{
	dprintf(RTL8814AU_DRIVER_NAME ": uninit_driver()\n");

	gUSBModule->uninstall_notify(RTL8814AU_DRIVER_NAME);

	MutexLocker locker(gDeviceListLock);
	for (uint32 i = 0; i < kMaxDeviceCount; i++) {
		if (gDeviceList[i] != NULL) {
			delete gDeviceList[i];
			gDeviceList[i] = NULL;
		}
	}
	gDeviceCount = 0;
	locker.Unlock();

	mutex_destroy(&gDeviceListLock);

	if (gNotificationModule != NULL) {
		put_module(NET_NOTIFICATIONS_MODULE_NAME);
		gNotificationModule = NULL;
	}

	put_module(B_USB_MODULE_NAME);

	// Free the cached device name strings
	for (uint32 i = 0; i < kMaxDeviceCount; i++) {
		free(sDeviceNames[i]);
		sDeviceNames[i] = NULL;
	}

	gUSBModule = NULL;
}


/*! Return the list of device paths we currently publish. Called by the
    kernel to populate /dev/. We return paths like
    "net/rtl8814au/0", "net/rtl8814au/1", etc.

    \return NULL-terminated array of device path strings.
*/
const char**
publish_devices()
{
	// Free previous names
	for (uint32 i = 0; i < kMaxDeviceCount; i++) {
		free(sDeviceNames[i]);
		sDeviceNames[i] = NULL;
	}

	MutexLocker locker(gDeviceListLock);
	uint32 nameIndex = 0;
	for (uint32 i = 0; i < kMaxDeviceCount; i++) {
		if (gDeviceList[i] == NULL)
			continue;

		// Build path: "net/rtl8814au/0"
		char path[64];
		snprintf(path, sizeof(path), "%s/%" B_PRIu32,
			RTL8814AU_DEVICE_PATH_BASE, i);
		sDeviceNames[nameIndex] = strdup(path);
		nameIndex++;
	}
	sDeviceNames[nameIndex] = NULL;

	return (const char**)sDeviceNames;
}


/*! Find the device_hooks structure for a given published device path.
    Called by the kernel when userland opens one of our devices.

    \param name  Device path (e.g., "net/rtl8814au/0")
    \return Pointer to our device_hooks, or NULL if not found.
*/
device_hooks*
find_device(const char* name)
{
	// All our devices share the same hooks — the device instance is
	// identified by the slot index in the path, resolved during open().
	static device_hooks hooks = {
		RTL8814AUDevice::Open,
		RTL8814AUDevice::Close,
		RTL8814AUDevice::Free,
		RTL8814AUDevice::Control,
		RTL8814AUDevice::Read,
		RTL8814AUDevice::Write,
	};

	// Verify the name matches our base path
	if (strncmp(name, RTL8814AU_DEVICE_PATH_BASE,
		strlen(RTL8814AU_DEVICE_PATH_BASE)) != 0)
		return NULL;

	return &hooks;
}
