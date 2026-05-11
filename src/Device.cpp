/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Device.cpp — RTL8814AUDevice implementation.
 *
 * Manages the full lifecycle of a single RTL8814AU USB WiFi adapter:
 *   - USB endpoint discovery (3 bulk OUT, 1 bulk IN, 1 interrupt IN)
 *   - Hardware power-on sequencing
 *   - Coordination of firmware loading, EFUSE reading, PHY init
 *   - Device open/close reference counting for hot-unplug safety
 *   - Ioctl dispatch for network stack integration
 *
 * Hardware initialization is deferred to the first open() call rather than
 * happening at USB attach time. This avoids blocking the USB bus manager
 * during firmware download (~1 second) and allows cleaner error reporting
 * to the caller that actually wants to use the device.
 */

#include "Device.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>
#include <OS.h>
#include <net/if_media.h>
#include <util/AutoLock.h>
#include <util/KMessage.h>

#include <ether_driver.h>
#include <NetworkNotifications.h>

#include "Driver.h"
#include "WiFiIoctl.h"
#include "WPA2Crypto.h"


// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------


/*! Create a new device instance for a USB device we claimed.
    Sets up the register I/O module and discovers USB endpoints.
    Does NOT initialize the hardware yet — that happens on first open().

    \param device       USB device handle from the bus manager
    \param slotIndex    Index in gDeviceList[] for this device
    \param deviceName   Human-readable name (e.g., "ASUS USB-AC68")
*/
RTL8814AUDevice::RTL8814AUDevice(usb_device device, uint32 slotIndex,
	const char* deviceName)
	:
	fUSBDevice(device),
	fBulkIn(0),
	fInterruptIn(0),
	fSlotIndex(slotIndex),
	fInitStatus(B_NO_INIT),
	fRemoved(false),
	fHardwareInitialized(false),
	fOpenCount(0),
	fRegisterIO(NULL),
	fFirmware(NULL),
	fEfuseReader(NULL),
	fPhyConfig(NULL),
	fTxPath(NULL),
	fRxPath(NULL),
	fWiFiManager(NULL)
{
	memset(fBulkOut, 0, sizeof(fBulkOut));
	memset(fMacAddress, 0, sizeof(fMacAddress));
	memset(fRxRing, 0, sizeof(fRxRing));
	memset(fJoinSsid, 0, sizeof(fJoinSsid));
	fJoinSsidLength = 0;
	memset(fJoinBssid, 0, sizeof(fJoinBssid));
	fRoaming = 0;
	fPrivacy = 0;
	fWpaMode = 0;
	fAuthMode = 0;
	fDropUnencrypted = 0;
	memset(fWpaIe, 0, sizeof(fWpaIe));
	fWpaIeLength = 0;
	fRxRingHead = 0;
	fRxRingTail = 0;
	memset(&fEapolInbox, 0, sizeof(fEapolInbox));
	fEapolPending = false;
	fEapolReady = -1;
	fEapolThread = -1;
	fEapolStop = false;
	fEapolState = kEapolIdle;
	memset(fAnonce, 0, sizeof(fAnonce));
	memset(fSnonce, 0, sizeof(fSnonce));
	memset(fPmk, 0, sizeof(fPmk));
	fPmkValid = false;
	memset(fPtk, 0, sizeof(fPtk));
	fPtkValid = false;
	fM1ReplayCounter = 0;
	fCcmpEnabled = false;
	memset(fGtk, 0, sizeof(fGtk));
	fGtkKeyId = 0;
	fGtkValid = false;
	fTxPnPairwise = 1;
	fTxPnGroup = 1;
	fLinkStateSem = -1;
	fPostAssocSem = -1;
	fPostAssocThread = -1;
	fPostAssocStop = false;
	fScanNotifierThread = -1;
	strlcpy(fDeviceName, deviceName, sizeof(fDeviceName));
	mutex_init(&fLock, "rtl8814au:device");

	fRxDataReady = create_sem(0, "rtl8814au:rx_ready");
	if (fRxDataReady < 0) {
		fInitStatus = fRxDataReady;
		return;
	}

	fEapolReady = create_sem(0, "rtl8814au:eapol_ready");
	if (fEapolReady < 0) {
		fInitStatus = fEapolReady;
		return;
	}

	// Create the register I/O module — everything else depends on this
	fRegisterIO = new(std::nothrow) RTL8814AURegisterIO(device, gUSBModule);
	if (fRegisterIO == NULL) {
		fInitStatus = B_NO_MEMORY;
		return;
	}

	// Create the firmware loader
	fFirmware = new(std::nothrow) RTL8814AUFirmware(fRegisterIO);
	if (fFirmware == NULL) {
		fInitStatus = B_NO_MEMORY;
		return;
	}

	// Create the EFUSE reader
	fEfuseReader = new(std::nothrow) RTL8814AUEfuseReader(fRegisterIO);
	if (fEfuseReader == NULL) {
		fInitStatus = B_NO_MEMORY;
		return;
	}

	// Discover and configure USB endpoints — must happen before creating
	// modules that need endpoint handles (TxPath, RxPath, WiFiManager)
	fInitStatus = _SetupEndpoints();
	if (fInitStatus != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": endpoint setup failed: %s\n",
			strerror(fInitStatus));
		return;
	}

	// Create the PHY configuration module
	fPhyConfig = new(std::nothrow) RTL8814AUPhyConfig(fRegisterIO,
		fEfuseReader);
	if (fPhyConfig == NULL) {
		fInitStatus = B_NO_MEMORY;
		return;
	}

	// Create the TX path — needs bulk OUT endpoint handles
	fTxPath = new(std::nothrow) RTL8814AUTxPath(fRegisterIO, gUSBModule,
		device, fBulkOut);
	if (fTxPath == NULL || fTxPath->InitCheck() != B_OK) {
		fInitStatus = (fTxPath != NULL) ? fTxPath->InitCheck() : B_NO_MEMORY;
		return;
	}

	// Firmware loader submits download chunks on the beacon bulk OUT
	// endpoint via the TX path, so it needs a pointer to it.
	fFirmware->SetTxPath(fTxPath);

	// Create the RX path — needs bulk IN endpoint handle
	fRxPath = new(std::nothrow) RTL8814AURxPath(fRegisterIO, gUSBModule,
		device, fBulkIn);
	if (fRxPath == NULL || fRxPath->InitCheck() != B_OK) {
		fInitStatus = (fRxPath != NULL) ? fRxPath->InitCheck() : B_NO_MEMORY;
		return;
	}

	// Register our RX frame handler
	fRxPath->SetFrameCallback(_RxFrameReceived, this);

	// Create the WiFi management module — needs interrupt IN for C2H events
	fWiFiManager = new(std::nothrow) RTL8814AUWiFiManager(fRegisterIO,
		gUSBModule, device, fInterruptIn);
	if (fWiFiManager == NULL || fWiFiManager->InitCheck() != B_OK) {
		fInitStatus = (fWiFiManager != NULL) ? fWiFiManager->InitCheck()
			: B_NO_MEMORY;
		return;
	}
}


RTL8814AUDevice::~RTL8814AUDevice()
{
	if (fHardwareInitialized)
		_Shutdown();

	delete fWiFiManager;
	delete fRxPath;
	delete fTxPath;
	delete fPhyConfig;
	delete fEfuseReader;
	delete fFirmware;
	delete fRegisterIO;

	if (fRxDataReady >= 0)
		delete_sem(fRxDataReady);
	if (fEapolReady >= 0)
		delete_sem(fEapolReady);
	if (fPostAssocSem >= 0)
		delete_sem(fPostAssocSem);

	mutex_destroy(&fLock);
}


/*! Mark the device as physically removed (USB unplug). All pending and
    future operations will return B_DEV_NOT_READY.
*/
void
RTL8814AUDevice::SetRemoved()
{
	fRemoved = true;
	if (fRegisterIO != NULL)
		fRegisterIO->SetDeviceRemoved();
}


// ---------------------------------------------------------------------------
// USB endpoint discovery
// ---------------------------------------------------------------------------


/*! Walk the USB configuration to find our 5 expected endpoints:
    3 bulk OUT, 1 bulk IN, 1 interrupt IN. The RTL8814AU always exposes
    these on the first interface of the first configuration.

    \return B_OK if all endpoints found, B_BAD_VALUE otherwise.
*/
status_t
RTL8814AUDevice::_SetupEndpoints()
{
	// Get the first (and usually only) configuration
	const usb_configuration_info* config
		= gUSBModule->get_nth_configuration(fUSBDevice, 0);
	if (config == NULL) {
		dprintf(RTL8814AU_DRIVER_NAME ": no USB configuration found\n");
		return B_BAD_VALUE;
	}

	// Set this configuration as active
	status_t status = gUSBModule->set_configuration(fUSBDevice, config);
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": set_configuration failed: %s\n",
			strerror(status));
		return status;
	}

	// The RTL8814AU uses a single interface
	if (config->interface_count < 1) {
		dprintf(RTL8814AU_DRIVER_NAME ": no interfaces in configuration\n");
		return B_BAD_VALUE;
	}

	const usb_interface_info* interface
		= config->interface[0].active;
	if (interface == NULL) {
		dprintf(RTL8814AU_DRIVER_NAME ": no active interface\n");
		return B_BAD_VALUE;
	}

	// Walk all endpoints and classify them by type and direction.
	// The RTL8814AU has a fixed endpoint layout:
	//   Bulk OUT ×3: TX data (priority-mapped to VO/VI, BE/BK, MGT/CMD)
	//   Bulk IN  ×1: RX data (aggregated frames from hardware)
	//   Interrupt IN ×1: C2H firmware events
	uint32 bulkOutIndex = 0;

	for (uint32 i = 0; i < interface->endpoint_count; i++) {
		const usb_endpoint_descriptor* endpoint
			= interface->endpoint[i].descr;
		usb_pipe pipe = interface->endpoint[i].handle;

		if (endpoint == NULL)
			continue;

		bool isIn = (endpoint->endpoint_address & USB_ENDPOINT_ADDR_DIR_IN)
			!= 0;
		uint8 attributes = endpoint->attributes & USB_ENDPOINT_ATTR_MASK;

		if (attributes == USB_ENDPOINT_ATTR_BULK) {
			if (isIn) {
				fBulkIn = pipe;
				dprintf(RTL8814AU_DRIVER_NAME ": found bulk IN endpoint "
					"0x%02x\n", endpoint->endpoint_address);
			} else {
				if (bulkOutIndex < kBulkOutEndpointCount) {
					fBulkOut[bulkOutIndex] = pipe;
					dprintf(RTL8814AU_DRIVER_NAME ": found bulk OUT "
						"endpoint 0x%02x (queue %"
						B_PRIu32 ")\n",
						endpoint->endpoint_address, bulkOutIndex);
					bulkOutIndex++;
				}
			}
		} else if (attributes == USB_ENDPOINT_ATTR_INTERRUPT && isIn) {
			fInterruptIn = pipe;
			dprintf(RTL8814AU_DRIVER_NAME ": found interrupt IN endpoint "
				"0x%02x\n", endpoint->endpoint_address);
		}
	}

	// Validate that we found all required endpoints
	if (fBulkIn == 0) {
		dprintf(RTL8814AU_DRIVER_NAME ": missing bulk IN endpoint\n");
		return B_BAD_VALUE;
	}

	if (bulkOutIndex == 0) {
		dprintf(RTL8814AU_DRIVER_NAME ": no bulk OUT endpoints found\n");
		return B_BAD_VALUE;
	}

	// Some RTL8814AU devices may expose fewer than 3 bulk OUT endpoints
	// depending on USB speed. Fill missing OUT pipes with the last one
	// found (all TX goes through a single pipe in that case).
	for (uint32 i = bulkOutIndex; i < kBulkOutEndpointCount; i++)
		fBulkOut[i] = fBulkOut[bulkOutIndex - 1];

	dprintf(RTL8814AU_DRIVER_NAME ": endpoint setup complete — "
		"%" B_PRIu32 " bulk OUT, 1 bulk IN, interrupt %s\n",
		bulkOutIndex, fInterruptIn != 0 ? "IN" : "missing");

	return B_OK;
}


// ---------------------------------------------------------------------------
// Hardware initialization — called on first open()
// ---------------------------------------------------------------------------


/*! Full hardware initialization sequence. Called with fLock held.
    This brings the chip from a powered-down state to fully operational:
      1. Power-on sequence (register writes to bring chip out of reset)
      2. Read EFUSE (MAC address, calibration data)
      3. Initialize MAC registers (queue page allocation, DMA enables)
      4. Load firmware (DMEM + IRAM to Lexra 3081 via beacon queue)
      5. Write MAC address to hardware
      6. PHY/RF initialization (4-path setup — TODO)

    MAC init precedes firmware download because the 3081-MCU firmware
    loader submits its chunks via the beacon bulk OUT endpoint — the
    chip needs TX packet buffer pages allocated (via REG_RQPN) and
    MAC TX enabled before it will drain those bulk OUT transfers.
    This matches the reference driver's init order: MAC queue config
    is set up in _InitQueueReservedPage_8814AU() before
    FirmwareDownload8814A() is called.

    \return B_OK on success, or the first error encountered.
*/
status_t
RTL8814AUDevice::_InitHardware()
{
	dprintf(RTL8814AU_DRIVER_NAME ": initializing hardware for %s\n",
		fDeviceName);

	// Step 1: Power-on sequence — bring the chip out of reset
	status_t status = _PowerOnSequence();
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": power-on sequence failed: %s\n",
			strerror(status));
		return status;
	}

	// Step 2: Read EFUSE — get MAC address and calibration data
	status = fEfuseReader->ReadEfuseMap();
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": EFUSE read failed: %s\n",
			strerror(status));
		return status;
	}

	status = _ReadMacAddress();
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": MAC address read failed: %s\n",
			strerror(status));
		return status;
	}

	dprintf(RTL8814AU_DRIVER_NAME ": MAC address: "
		"%02x:%02x:%02x:%02x:%02x:%02x\n",
		fMacAddress[0], fMacAddress[1], fMacAddress[2],
		fMacAddress[3], fMacAddress[4], fMacAddress[5]);

	// Step 3: Initialize MAC — allocate TX packet buffer pages to queues
	// and enable DMA engines.  MUST happen before firmware load because
	// the 3081-MCU loader submits firmware chunks via the beacon bulk
	// OUT endpoint, which requires the beacon queue to have pages.
	status = _InitMAC();
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": MAC init failed: %s\n",
			strerror(status));
		return status;
	}

	// Step 4: Load firmware — transfer DMEM and IRAM to the Lexra 3081
	status = fFirmware->Load(RTL8814AU_FIRMWARE_PATH);
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": firmware load failed: %s\n",
			strerror(status));
		return status;
	}

	// Step 5: Write MAC address to hardware
	status = _WriteMacAddress();
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": MAC address write failed: %s\n",
			strerror(status));
		return status;
	}

	// Step 5.4: Take the BB out of GLOBAL reset so subsequent BB writes
	// (init table, AGC, RFE pinmux, OFDMCCK_EN, etc.) actually land.
	// REG 0x1002 BIT0 (FEN_BBRSTB) and BIT1 (FEN_BB_GLB_RSTn) must BOTH
	// be set; the chip starts with these clear, so writes to 0x800-0x1FFF
	// are silently dropped until we set them.
	//
	// Discovered via usbmon capture from morrownr/8814au's cold-start
	// init: frame 9642 writes 0x03 to 0x1002, frame 9650 onward writes
	// to BB regs (0x800, 0x804, 0x808, ...) succeed.  Our previous
	// attempt set only BIT0 (assuming it was a clock gate); turns out
	// BIT1 is the actual BB-global-reset deassert and is what unblocks
	// register writes.
	{
		uint8 before = fRegisterIO->Read8(0x1002);
		fRegisterIO->Write8(0x1002, before | 0x03u);
		uint8 after = fRegisterIO->Read8(0x1002);
		dprintf(RTL8814AU_DRIVER_NAME ": BB out of global reset "
			"(0x1002 0x%02x->0x%02x)\n",
			(unsigned)before, (unsigned)after);
	}

	// Step 5.5: Power on all 4 RF analog paths BEFORE the BB init table
	// is applied.  The RTL8814AU has Path A/B/C/D RF blocks at addresses
	// 0x01F/0x020/0x021/0x076 respectively (different layout from the
	// 8812A which only has 2 paths).  Each register holds EN|RSTB|SDMRSTB
	// = 0x07 to bring its path out of reset.  See morrownr/8814au
	// rtl8814a_phycfg.c::PHY_BB8814A_Config_1T for the reference sequence.
	{
		fRegisterIO->Write8(0x001F, 0x07);	// Path A
		fRegisterIO->Write8(0x0020, 0x07);	// Path B
		fRegisterIO->Write8(0x0021, 0x07);	// Path C
		fRegisterIO->Write8(0x0076, 0x07);	// Path D
		uint8 rfA = fRegisterIO->Read8(0x001F);
		uint8 rfB = fRegisterIO->Read8(0x0020);
		uint8 rfC = fRegisterIO->Read8(0x0021);
		uint8 rfD = fRegisterIO->Read8(0x0076);
		dprintf(RTL8814AU_DRIVER_NAME ": RF analog powered on "
			"(A=0x%02x B=0x%02x C=0x%02x D=0x%02x)\n",
			(unsigned)rfA, (unsigned)rfB, (unsigned)rfC, (unsigned)rfD);
	}

	// Step 5.6: GPIO_IO_SEL prerequisite for the RFE.  Mirrors the
	// bInit==TRUE / rfe_type=0 branch of morrownr/8814au
	// PHY_SetRFEReg8814A.  Sets bits 22-23 of REG 0x40 (= bits 6-7 of
	// byte 0x42) so the GPIO mux routes the RFE pins correctly.
	//
	// The companion BB-side write (0x1994[3:0] = 0xF) is deferred to
	// _ConfigTrxPath because BB writes only stick when the BB clock
	// is gated (0x1002 BIT0 = 0).
	{
		uint8 gpioSel = fRegisterIO->Read8(0x0042);
		fRegisterIO->Write8(0x0042, gpioSel | 0xC0);
		dprintf(RTL8814AU_DRIVER_NAME ": GPIO_IO_SEL set "
			"(0x42: 0x%02x->0x%02x)\n",
			(unsigned)gpioSel, (unsigned)fRegisterIO->Read8(0x0042));
	}

	// Step 6: PHY/RF initialization — configure all 4 RF paths
	if (fPhyConfig != NULL) {
		status = fPhyConfig->Initialize();
		if (status != B_OK) {
			dprintf(RTL8814AU_DRIVER_NAME ": PHY init failed: %s\n",
				strerror(status));
			return status;
		}
	}

	// Step 6.4: Configure USB RX aggregation so the chip will DMA
	// received frames out over the bulk-IN endpoint.  Without this the
	// MAC will receive but never push anything to USB.
	status = _InitRxAggregation();
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": RX aggregation init failed: %s\n",
			strerror(status));
		return status;
	}

	// Step 6.5: Enable MAC TX/RX engines.  These are deliberately left off
	// until after firmware load, PHY init, and RF/IQ calibration complete —
	// turning them on earlier stalls the USB TX DMA (the chip never pulls
	// bulk-OUT frames off EP2).  Without these bits set, the chip never
	// pumps anything onto the bulk-IN endpoint either, so the RX callback
	// never fires.
	{
		uint16 crBefore = fRegisterIO->Read16(kRegCR);
		uint16 crAfter = crBefore | kCR_MAC_TX_En | kCR_MAC_RX_En;
		fRegisterIO->Write16(kRegCR, crAfter);
		dprintf(RTL8814AU_DRIVER_NAME ": MAC TX/RX enabled "
			"(REG_CR 0x%04x -> 0x%04x)\n",
			(unsigned)crBefore, (unsigned)fRegisterIO->Read16(kRegCR));
	}

	// Step 6.55: Apply the 8814AU-specific CCK TX/RX path configuration.
	// _rtw_config_trx_path_8814a() in the morrownr reference runs after
	// the BB init table so we follow the same ordering.  Disables 2R CCA
	// on CCK and points the CCK demodulator at the correct paths.
	_ConfigTrxPath();
	_DumpRxState("after-trx-config");

	// Step 6.6: Clear bulk-IN ENDPOINT_HALT.  We already clear it on the
	// beacon bulk-OUT path during firmware load; do the same on bulk-IN
	// just in case the chip left it stalled, which would silently
	// swallow every queued RX URB.
	if (fRxPath != NULL)
		fRxPath->ClearHalt();

	// Step 7: Start the RX receive loop (bulk IN transfers)
	if (fRxPath != NULL) {
		status = fRxPath->Start();
		if (status != B_OK) {
			dprintf(RTL8814AU_DRIVER_NAME ": RX start failed: %s\n",
				strerror(status));
			return status;
		}
	}

	// Step 8: Start the WiFi management module (interrupt IN for C2H events)
	if (fWiFiManager != NULL) {
		status = fWiFiManager->Start();
		if (status != B_OK) {
			dprintf(RTL8814AU_DRIVER_NAME ": WiFi manager start failed: %s\n",
				strerror(status));
			return status;
		}
	}

	fJoinState = kJoinIdle;
	fJoinSeqCounter = 0;
	fHardwareInitialized = true;

	// Spawn the post-assoc worker.  It blocks on fPostAssocSem until
	// _HandleAssocResponse releases it, then issues the firmware H2C
	// setup (RA_INFO + MEDIA_STATUS_RPT) from a context that can
	// safely block on USB control transfers.
	fPostAssocSem = create_sem(0, "rtl8814au:post_assoc");
	if (fPostAssocSem >= 0) {
		fPostAssocThread = spawn_kernel_thread(_PostAssocThreadEntry,
			"rtl8814au:post_assoc", B_NORMAL_PRIORITY, this);
		if (fPostAssocThread >= 0) {
			resume_thread(fPostAssocThread);
		} else {
			dprintf(RTL8814AU_DRIVER_NAME ": failed to spawn post-assoc "
				"worker: %s\n", strerror(fPostAssocThread));
			delete_sem(fPostAssocSem);
			fPostAssocSem = -1;
		}
	} else {
		dprintf(RTL8814AU_DRIVER_NAME ": failed to create post-assoc "
			"sem: %s\n", strerror(fPostAssocSem));
	}

	// Spawn the in-driver 4-way handshake worker.  Blocks on fEapolReady
	// (released from _RxFrameReceived when an EAPOL frame arrives), drains
	// fEapolInbox under fLock, then advances the state machine.
	fEapolStop = false;
	fEapolState = kEapolIdle;
	fEapolThread = spawn_kernel_thread(_Eapol4WayThreadEntry,
		"rtl8814au:eapol_4way", B_NORMAL_PRIORITY, this);
	if (fEapolThread >= 0)
		resume_thread(fEapolThread);
	else {
		dprintf(RTL8814AU_DRIVER_NAME ": failed to spawn eapol worker: "
			"%s\n", strerror(fEapolThread));
		fEapolThread = -1;
	}

	// TX path verification: send a single broadcast probe-request frame.
	// If TX works, we should see APs responding with unicast probe-responses
	// (frame subtype 5) addressed to our MAC over the next second or two.
	// This is a one-shot diagnostic — keep until JOIN is wired up.
	{
		uint8 probe[28];
		probe[0] = 0x40;	// Frame Control byte 0: type=mgmt(0), subtype=ProbeReq(4)
		probe[1] = 0x00;	// Frame Control byte 1: no flags
		probe[2] = 0x00; probe[3] = 0x00;	// Duration
		probe[4] = 0xFF; probe[5] = 0xFF; probe[6] = 0xFF;	// DA (broadcast)
		probe[7] = 0xFF; probe[8] = 0xFF; probe[9] = 0xFF;
		memcpy(probe + 10, fMacAddress, 6);					// SA (our MAC)
		probe[16] = 0xFF; probe[17] = 0xFF; probe[18] = 0xFF;	// BSSID (broadcast)
		probe[19] = 0xFF; probe[20] = 0xFF; probe[21] = 0xFF;
		probe[22] = 0x00; probe[23] = 0x00;	// Sequence
		probe[24] = 0x00; probe[25] = 0x00;	// SSID IE (id=0, len=0 = wildcard)
		probe[26] = 0x01; probe[27] = 0x00;	// Supported Rates IE (id=1, len=0)

		status_t txStatus = fTxPath->Transmit(probe, sizeof(probe),
			kTxQueueMGT, 0, 0, kSecurityNone, true);
		dprintf(RTL8814AU_DRIVER_NAME ": TX probe-req test: %s\n",
			strerror(txStatus));
	}
	dprintf(RTL8814AU_DRIVER_NAME ": hardware initialization complete\n");
	return B_OK;
}


/*! Power-on sequence for the RTL8814AU. This is a series of register
    writes that bring the chip from deep sleep to an active state where
    the CPU, MAC, and baseband are powered and clocked.

    Derived from Hal8814PwrSeq.c in the reference driver.
*/
status_t
RTL8814AUDevice::_PowerOnSequence()
{
	dprintf(RTL8814AU_DRIVER_NAME ": starting power-on sequence\n");

	// Undocumented Realtek workaround: set bit 1 of register 0x10C2
	// before the power-on FSM transition.  Comment in the reference
	// driver attributes this to "YX sugguested 2014.06.03".  Without
	// this the chip's bulk OUT endpoint FIFOs do not accept data —
	// every firmware TX hangs at the USB layer with the chip never
	// pulling the packet.  (Observed symptom prior to adding this.)
	uint8 reg10C2 = fRegisterIO->Read8(0x10C2);
	fRegisterIO->Write8(0x10C2, reg10C2 | 0x02);
	dprintf(RTL8814AU_DRIVER_NAME ": 0x10C2 workaround (before=0x%02x, "
		"wrote=0x%02x)\n", (unsigned)reg10C2, (unsigned)(reg10C2 | 0x02));

	// ---------------------------------------------------------------
	// Phase 1: Analog and clock setup (AFE, crystal, PLL)
	// ---------------------------------------------------------------

	// Enable analog front-end and PLL
	fRegisterIO->Write8(kRegApsRsvd, 0x00);

	// Enable SPS (Switching Power Supply)
	fRegisterIO->MaskedWrite8(kRegApsRsvd, 0x01, 0x01);
	snooze(2000);	// 2 ms — wait for power supply to stabilize

	// Enable AFE (analog front-end) bandgap
	fRegisterIO->MaskedWrite8(kRegAfeCtrl1, 0x80, 0x80);

	// Enable crystal oscillator
	fRegisterIO->MaskedWrite8(kRegAfeCtrl2, 0x02, 0x02);
	snooze(2000);

	// Disable isolation between analog and digital domains
	fRegisterIO->MaskedWrite8(kRegRsvCtrl, 0x02, 0x00);

	// Enable XTAL output for MAC clock
	fRegisterIO->MaskedWrite8(kRegAfeCtrl2, 0x04, 0x04);

	// Enable system function blocks
	uint16 sysFuncEn = fRegisterIO->Read16(kRegSysFuncEn);
	sysFuncEn |= kSysFuncEnUSBA | kSysFuncEnUSBD;
	fRegisterIO->Write16(kRegSysFuncEn, sysFuncEn);

	// Enable MAC and baseband clocks
	fRegisterIO->MaskedWrite8(kRegAfeCtrl2 + 1, 0x18, 0x18);

	// Wait for MAC to be ready
	snooze(2000);

	// Enable digital core power
	sysFuncEn = fRegisterIO->Read16(kRegSysFuncEn);
	sysFuncEn |= kSysFuncEnBBRSTB | kSysFuncEnBBGlbRst | kSysFuncEnDcore;
	fRegisterIO->Write16(kRegSysFuncEn, sysFuncEn);

	// ---------------------------------------------------------------
	// Phase 2: APS FSM power-on transition (Rtl8814A_NIC_ENABLE_FLOW)
	//
	// The RTL8814AU has an Auto Power State (APS) FSM controlled via
	// register 0x0004-0x0005 (APS_FSMCO). Triggering the card-emu-to-
	// active transition brings the MAC register domain (0x0100+) online.
	// Without this, MAC registers return 0xEA (powered-off domain).
	//
	// Matches HalPwrSeqCmdParsing(Rtl8814A_NIC_ENABLE_FLOW) from
	// the reference driver's _InitPowerOn_8814AU().
	// ---------------------------------------------------------------

	// Step 1: Clear APS_FSMCO byte 1 bit 2 (= overall bit 10)
	fRegisterIO->MaskedWrite8(kRegApsFsmco + 1, 0x04, 0x00);

	// Step 2: Poll SYS_CLKR (0x0006) until bit 1 is set (clock ready)
	status_t status = fRegisterIO->PollFor8(kRegSysClkr, 0x02, 0x02,
		200, 500);
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": SYS_CLKR clock ready timeout\n");
		return status;
	}

	// Step 3: Clear APS_FSMCO byte 1 bit 3 (= overall bit 11)
	fRegisterIO->MaskedWrite8(kRegApsFsmco + 1, 0x08, 0x00);

	// Step 4: Clear SYS_CFG (0x00F0) bit 7
	fRegisterIO->MaskedWrite8(kRegSysCfg, 0x80, 0x00);

	// Step 5: Configure FW_CTRL byte 1 (0x0081): set bit 5, clear bit 4
	fRegisterIO->MaskedWrite8(kRegMcuFwDl + 1, 0x30, 0x20);

	// Step 6: Set APS_FSMCO byte 1 bit 0 (= overall bit 8) to trigger
	// the card-emu-to-active power state transition
	fRegisterIO->MaskedWrite8(kRegApsFsmco + 1, 0x01, 0x01);

	// Step 7: Poll APS_FSMCO byte 1 until bit 0 clears (transition done)
	status = fRegisterIO->PollFor8(kRegApsFsmco + 1, 0x01, 0x00,
		200, 500);
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": APS FSM transition timeout\n");
		return status;
	}

	// ---------------------------------------------------------------
	// Phase 3: Enable MAC DMA engines
	//
	// Now that the MAC domain is online, enable the DMA engines,
	// protocol engine, scheduler, and security engine via REG_CR.
	// This matches _InitPowerOn_8814AU() in the reference driver.
	// ---------------------------------------------------------------

	fRegisterIO->Write16(kRegCR, 0x0000);
	uint16 crValue = fRegisterIO->Read16(kRegCR);
	crValue |= kCR_HCI_TxDMA_En | kCR_HCI_RxDMA_En
		| kCR_TxDMA_En | kCR_RxDMA_En
		| kCR_Protocol_En | kCR_Schedule_En
		| kCR_EnsecCAMTx | kCR_EnsecCAMRx;
	fRegisterIO->Write16(kRegCR, crValue);

	dprintf(RTL8814AU_DRIVER_NAME ": power-on sequence complete "
		"(REG_CR=0x%04x)\n", fRegisterIO->Read16(kRegCR));
	return B_OK;
}


/*! Initialize the MAC block after firmware is loaded. Configures:
    - Command register (enable TX/RX DMA, protocol, scheduler)
    - TX/RX buffer page boundaries
    - AMPDU aggregation parameters
    - Receive filter (initially accept management + data + broadcast)
*/
status_t
RTL8814AUDevice::_InitMAC()
{
	dprintf(RTL8814AU_DRIVER_NAME ": initializing MAC\n");

	// NOTE: CR is already set to 0x603F by _PowerOnSequence (HCI_TxDMA |
	// HCI_RxDMA | TxDMA | RxDMA | Protocol | Schedule | ENSEC | CALTMR).
	// DO NOT clobber it here and DO NOT enable MAC_TX_EN/MAC_RX_EN yet —
	// the MAC state machine needs firmware before those can be set.
	// Enabling them prematurely causes the USB TX DMA to stall (the chip
	// never pulls bulk OUT frames off EP2), which is the exact symptom
	// we were debugging.  MAC_TX_EN/MAC_RX_EN get enabled post-firmware.

	uint16 crBefore = fRegisterIO->Read16(kRegCR);
	dprintf(RTL8814AU_DRIVER_NAME ": CR entering _InitMAC = 0x%04x\n",
		(unsigned)crBefore);

	// Allocate TX packet buffer pages across the six internal queues.
	// This must happen before any bulk OUT transfer to the chip; without
	// it the hardware has no pages to buffer incoming frames and USB
	// transfers stall waiting for the chip to pull them off the wire.
	status_t status = _InitPageAllocation();
	if (status != B_OK)
		return status;

	// LLT auto-init was tried at REG_AUTO_LLT (0x0208) BIT0 but found to
	// be a no-op on 8814A — the chip auto-builds the link-list as part
	// of the RQPN_CTRL_2 commit.  Skip the trigger here.

	// Program the queue-to-endpoint priority map so the three USB bulk
	// OUT endpoints (pipe 0 = HIGH, pipe 1 = NORMAL, pipe 2 = LOW) route
	// to the right internal queues.  Without this, the chip doesn't know
	// where frames on EP2/EP3/EP4 belong.
	status = _InitQueuePriority();
	if (status != B_OK)
		return status;

	// Clear REG_TXPAUSE — the reference driver ensures transmission is
	// not paused before the first bulk OUT.  Without this, the chip will
	// accept but not dispatch frames.
	fRegisterIO->Write8(kRegTxPause, 0x00);

	// Set AMPDU aggregation parameters
	fRegisterIO->Write8(kRegAmpduMaxTime, kAmpduMaxTime);
	fRegisterIO->Write32(kRegAmpduMaxLength, kAmpduMaxLength);

	// Configure initial RX filter — accept unicast, broadcast,
	// multicast, data, and mgmt frames.  We deliberately leave
	// CBSSID_DATA / CBSSID_BCN cleared at init so beacons from every
	// BSS reach _ParseBeaconOrProbe during scan.  They get OR'd in
	// later by _InstallSessionKeys once we associate.  ICV/MIC/PHYST
	// retain bits stay set (chip defaults f4000000) so the HW crypto
	// engine can stage frames through the decrypt unit.
	uint32 rxFilter = kRCR_APM | kRCR_AM | kRCR_AB
		| kRCR_ADF | kRCR_AMF
		| kRCR_HTC_LOC_CTRL
		| kRCR_APP_PHYST_RXFF | kRCR_APP_ICV | kRCR_APP_MIC;
	fRegisterIO->Write32(kRegRCR, rxFilter);

	// Enable DMA engines
	status = _EnableDMA();
	if (status != B_OK)
		return status;

	dprintf(RTL8814AU_DRIVER_NAME ": MAC initialization complete\n");
	return B_OK;
}


/*! Configure USB RX aggregation registers so the chip pushes received
	frames out over bulk-IN.  Without this the MAC receives into internal
	buffers but never DMAs them to the host, so the bulk-IN URB callback
	never fires.

	We currently hardcode USB 2.0 thresholds because the ASUS USB-AC68
	we target presents itself as USB 2.0.  The Realtek reference reads
	REG_TYPE_ID+3 bit 0x80 to choose USB2 vs USB3 burst-size; if we ever
	support USB 3.0 hosts properly, branch here on a runtime check.
*/
status_t
RTL8814AUDevice::_InitRxAggregation()
{
	// Threshold: flush RX FIFO to USB when 1 page is full or 16*32us
	// has elapsed (USB 2.0 defaults from rtwn r12au_postattach).
	fRegisterIO->Write16(kRegRxDmaAggPgTh, kRxDmaAggUsb2Value);

	// Burst length / DMA mode for USB 2.0 path.
	uint8 dmaPro = fRegisterIO->Read8(kRegRxDmaPro);
	uint8 newDmaPro = (dmaPro & ~kRxDmaProBurstSzMask) | kRxDmaProUsb2Value;
	fRegisterIO->Write8(kRegRxDmaPro, newDmaPro);

	// Enable RX-DMA aggregation on the TRXDMA control register.  This
	// is the bit that actually unblocks bulk-IN delivery.
	uint16 trxCtrl = fRegisterIO->Read16(kRegTrxDmaCfg);
	fRegisterIO->Write16(kRegTrxDmaCfg, trxCtrl | kTrxDmaCtrlRxDmaAggEn);

	// Open the per-subtype filters.  Without these the MAC silently
	// drops all 802.11 frames regardless of RCR.AMF/ACF/ADF, so the
	// bulk-IN endpoint stays empty.  Use 0xffff during bring-up to
	// accept every subtype; tighten later when association works.
	uint16 fmap0Before = fRegisterIO->Read16(kRegRxFltMap0);
	uint16 fmap1Before = fRegisterIO->Read16(kRegRxFltMap1);
	uint16 fmap2Before = fRegisterIO->Read16(kRegRxFltMap2);
	// Tell the chip to prepend a 32-byte PHY status block to every RX
	fRegisterIO->Write8(0x060F, 0x04);
	fRegisterIO->Write16(kRegRxFltMap0, 0xFFFF);
	fRegisterIO->Write16(kRegRxFltMap1, 0xFFFF);
	fRegisterIO->Write16(kRegRxFltMap2, 0xFFFF);

	dprintf(RTL8814AU_DRIVER_NAME ": RX aggregation enabled "
		"(RXDMA_AGG_PG_TH=0x%04x, RXDMA_PRO 0x%02x->0x%02x, "
		"TRXDMA_CTRL 0x%04x->0x%04x)\n",
		(unsigned)fRegisterIO->Read16(kRegRxDmaAggPgTh),
		(unsigned)dmaPro, (unsigned)newDmaPro,
		(unsigned)trxCtrl, (unsigned)fRegisterIO->Read16(kRegTrxDmaCfg));
	dprintf(RTL8814AU_DRIVER_NAME ": RX subtype filters opened "
		"(FLTMAP0 0x%04x->0x%04x, FLTMAP1 0x%04x->0x%04x, "
		"FLTMAP2 0x%04x->0x%04x)\n",
		(unsigned)fmap0Before, (unsigned)fRegisterIO->Read16(kRegRxFltMap0),
		(unsigned)fmap1Before, (unsigned)fRegisterIO->Read16(kRegRxFltMap1),
		(unsigned)fmap2Before, (unsigned)fRegisterIO->Read16(kRegRxFltMap2));
	_DumpRxState("post-init");
	return B_OK;
}


/*! Dump the receive-related register state.  Useful for spotting
	whether the MAC is actually receiving frames internally (RXPKT_NUM
	increments), or whether RX-DMA is stuck (RXDMA_STATUS).

	Called after _InitRxAggregation and again at the start of every
	scan request — so we can compare quiescent state vs scan-active
	state of the chip's RX path without modifying user code.
*/
void
RTL8814AUDevice::_DumpRxState(const char* tag)
{
	uint16 cr = fRegisterIO->Read16(kRegCR);
	uint32 rcr = fRegisterIO->Read32(kRegRCR);
	uint16 fmap0 = fRegisterIO->Read16(kRegRxFltMap0);
	uint16 fmap1 = fRegisterIO->Read16(kRegRxFltMap1);
	uint16 fmap2 = fRegisterIO->Read16(kRegRxFltMap2);
	uint16 rxPktNum = fRegisterIO->Read16(kRegRxPktNum);
	uint32 rxDmaStatus = fRegisterIO->Read32(kRegRxDmaStatus);
	uint16 trxDma = fRegisterIO->Read16(kRegTrxDmaCfg);
	uint32 ofdmCck = fRegisterIO->Read32(kRegBBOfdmCckEn);
	uint32 txPath = fRegisterIO->Read32(kRegBBTxPath);
	uint32 cckRx = fRegisterIO->Read32(kRegBBCckRxPath);
	uint8 cckCheck = fRegisterIO->Read8(kRegBBCckCheck);
	uint32 rfeMux = fRegisterIO->Read32(kRegBBRfePinmux0);
	dprintf(RTL8814AU_DRIVER_NAME ": [%s] CR=0x%04x RCR=0x%08x "
		"FLTMAP=%04x/%04x/%04x RXPKT_NUM=0x%04x RXDMA_STATUS=0x%08x "
		"TRXDMA_CTRL=0x%04x\n",
		tag, (unsigned)cr, (unsigned)rcr,
		(unsigned)fmap0, (unsigned)fmap1, (unsigned)fmap2,
		(unsigned)rxPktNum, (unsigned)rxDmaStatus, (unsigned)trxDma);
	dprintf(RTL8814AU_DRIVER_NAME ": [%s] BB OFDMCCK_EN=0x%08x TX_PATH=0x%08x "
		"CCK_RX_PATH=0x%08x CCK_CHECK=0x%02x RFE_PINMUX0=0x%08x\n",
		tag, (unsigned)ofdmCck, (unsigned)txPath, (unsigned)cckRx,
		(unsigned)cckCheck, (unsigned)rfeMux);
}


/*! Bring the BB demodulator and TX/RX path masks online for 2.4 GHz
	operation.  Mirrors freebsd_wlan rtl8812a r12a_set_band_2ghz logic:
	enable CCK + OFDM demod, route TX path A, route CCK RX path A.

	The 8814AU has 4 RF paths but for first-light we mask path A only —
	just like the 8812A reference — to prove that the receiver actually
	hears the air.  Once RXPKT_NUM increments we can extend the masks
	to include all 4 paths.

	Note: this is the missing piece that makes REG_RXPKT_NUM go non-zero;
	without enabling the OFDMCCK demodulator the BB drops everything
	even though MAC TX/RX, RCR, FLTMAP and RXDMA aggregation are all
	correctly programmed.
*/
status_t
RTL8814AUDevice::_ConfigTrxPath()
{
	// 1) CCK TX/RX path mask: write the exact value morrownr/8814au
	//    writes during cold-start (frame 11100 of the cold-start
	//    trace) for this hardware variant — 0x46ff800c.
	//    bits 31:28 = 0x4  (CCK TX path B)
	//    bits 27:24 = 0x6  (CCK RX paths B+C)
	//    Our BB init table leaves bits 24-27 = 0x1 (only path A),
	//    which doesn't match the physical antennas on this 4-stream
	//    hardware (EFUSE antenna=12 → paths C+D, rfe_type=20).
	uint32 cckBefore = fRegisterIO->Read32(kRegBBCckRxPath);
	fRegisterIO->Write32(kRegBBCckRxPath, 0x46ff800cu);
	uint32 cckAfter = fRegisterIO->Read32(kRegBBCckRxPath);

	// 2) OFDM+CCK demodulator enable: 0x0808 bits 28-29 = 11.
	uint32 ofdmBefore = fRegisterIO->Read32(kRegBBOfdmCckEn);
	fRegisterIO->Write32(kRegBBOfdmCckEn, ofdmBefore | 0x30000000u);
	uint32 ofdmAfter = fRegisterIO->Read32(kRegBBOfdmCckEn);

	dprintf(RTL8814AU_DRIVER_NAME ": TRX path configured "
		"(CCK_RX_PATH 0x%08x->0x%08x, OFDMCCK_EN 0x%08x->0x%08x)\n",
		(unsigned)cckBefore, (unsigned)cckAfter,
		(unsigned)ofdmBefore, (unsigned)ofdmAfter);
	return B_OK;
}


/*! Allocate TX packet buffer pages across the six internal hardware
    queues and set the beacon-queue boundary.

    The 8814A uses a 5-register bank at 0x0230–0x0240 (one per queue)
    plus a commit register at 0x022C.  Writing kRQPNCommit (0x80000000)
    to REG_RQPN_CTRL_2 latches the per-queue values into hardware.

    Page counts match the reference driver's HPQ/LPQ/NPQ/EPQ_PGNUM
    values (20 each) and PUB = 2048 - BCNQ(8) - 4*20 = 1960.

    Reference: _InitQueueReservedPage_8814AUsb() in morrownr/8814au,
               hal/rtl8814a/usb/usb_halinit.c.
*/
status_t
RTL8814AUDevice::_InitPageAllocation()
{
	// Write per-queue page counts
	fRegisterIO->Write32(kRegFIFOPage_Info_1, kPageNumHPQ);
	fRegisterIO->Write32(kRegFIFOPage_Info_2, kPageNumLPQ);
	fRegisterIO->Write32(kRegFIFOPage_Info_3, kPageNumNPQ);
	fRegisterIO->Write32(kRegFIFOPage_Info_4, kPageNumEPQ);
	fRegisterIO->Write32(kRegFIFOPage_Info_5, kPageNumPUB);

	// Commit the per-queue allocation into hardware
	fRegisterIO->Write32(kRegRQPN_Ctrl_2, kRQPNCommit);

	// Set the beacon-queue page boundary.  All boundary registers get the
	// same value — the TX packet buffer region above this boundary is
	// reserved for beacon / reserved-page use.  The reference driver also
	// writes REG_FIFOPAGE_CTRL_2 (0x0204) as the BCN0 head and
	// REG_FIFOPAGE_CTRL_2 + 2 (0x0206) as the BCN1 head.
	fRegisterIO->Write16(kRegTxPktBufBcnQBdy, kFwTxPktBufBoundary);
	fRegisterIO->Write16(kRegMgQPgBndy, kFwTxPktBufBoundary);
	fRegisterIO->Write16(kRegFIFOPage, kFwTxPktBufBoundary);
	fRegisterIO->Write16(kRegFIFOPage + 2, kFwTxPktBufBoundary);

	dprintf(RTL8814AU_DRIVER_NAME ": page allocation set "
		"(HPQ=%u LPQ=%u NPQ=%u EPQ=%u PUB=%u bndy=0x%04x)\n",
		(unsigned)kPageNumHPQ, (unsigned)kPageNumLPQ,
		(unsigned)kPageNumNPQ, (unsigned)kPageNumEPQ,
		(unsigned)kPageNumPUB, (unsigned)kFwTxPktBufBoundary);
	return B_OK;
}


/*! Trigger the hardware Link-List Table auto-init and wait for it to
    complete.

    The LLT is the per-queue linked-list of page descriptors inside the
    chip's TX packet buffer.  Until the hardware builds it, the MAC
    cannot move frames from the USB FIFO into any queue — bulk OUT
    transfers sit in the USB endpoint without being consumed, and the
    host sees every TX time out.

    Writing bit 0 of REG_AUTO_LLT triggers the build; the chip clears
    bit 0 when the linked-list is ready.  We poll up to ~10 seconds
    (200 × 50 ms) as the reference driver does.

    Reference: InitLLTTable8814A() in morrownr/8814au,
               hal/rtl8814a/rtl8814a_hal_init.c.
*/
status_t
RTL8814AUDevice::_InitLLTTable()
{
	uint8 before = fRegisterIO->Read8(kRegAutoLLT);
	uint8 wrote = before | kAutoLLTTrigger;
	fRegisterIO->Write8(kRegAutoLLT, wrote);
	uint8 readback = fRegisterIO->Read8(kRegAutoLLT);

	dprintf(RTL8814AU_DRIVER_NAME ": LLT trigger "
		"(before=0x%02x, wrote=0x%02x, readback=0x%02x)\n",
		(unsigned)before, (unsigned)wrote, (unsigned)readback);

	// If the write didn't take effect (readback still shows bit 0 clear),
	// the register is probably read-only or mapped elsewhere.  Don't
	// falsely claim success — report and let the caller decide.
	if ((readback & kAutoLLTTrigger) == 0 && (before & kAutoLLTTrigger) == 0) {
		dprintf(RTL8814AU_DRIVER_NAME ": LLT trigger write had no effect "
			"— register may not be REG_AUTO_LLT on this chip\n");
		return B_OK;	// Continue — maybe LLT auto-runs on this variant
	}

	for (uint32 i = 0; i < kAutoLLTPollAttempts; i++) {
		uint8 llt = fRegisterIO->Read8(kRegAutoLLT);
		if ((llt & kAutoLLTTrigger) == 0) {
			dprintf(RTL8814AU_DRIVER_NAME ": LLT init complete "
				"(%u polls, REG_AUTO_LLT=0x%02x)\n",
				(unsigned)i, (unsigned)llt);
			return B_OK;
		}
		snooze(kAutoLLTPollDelay);
	}

	uint8 final = fRegisterIO->Read8(kRegAutoLLT);
	dprintf(RTL8814AU_DRIVER_NAME ": LLT init TIMED OUT "
		"(REG_AUTO_LLT=0x%02x)\n", (unsigned)final);
	return B_TIMED_OUT;
}


/*! Program REG_TRXDMA_CTRL (0x010C) to map internal hardware queues to
    the three USB bulk OUT endpoint priority levels.

    The 3EP non-WMM mapping matches the reference driver:
      VOQ → HIGH   (pipe 0, EP2)
      VIQ → NORMAL (pipe 1, EP3)
      BEQ → LOW    (pipe 2, EP4)
      BKQ → LOW
      MGQ → HIGH   (so BCN/MGT/firmware-chunks land on EP2)
      HIQ → HIGH

    Reference: _InitNormalChipRegPriority_8814AUsb() in morrownr/8814au,
               hal/rtl8814a/usb/usb_halinit.c.
*/
status_t
RTL8814AUDevice::_InitQueuePriority()
{
	// Preserve the lower 3 bits of the existing register value
	uint16 existing = fRegisterIO->Read16(kRegTrxDmaCfg);
	uint16 preserved = existing & 0x07;

	uint16 priorityMap
		= (uint16)(kQueueHigh   << kTxDmaVOQShift)
		| (uint16)(kQueueNormal << kTxDmaVIQShift)
		| (uint16)(kQueueLow    << kTxDmaBEQShift)
		| (uint16)(kQueueLow    << kTxDmaBKQShift)
		| (uint16)(kQueueHigh   << kTxDmaMGQShift)
		| (uint16)(kQueueHigh   << kTxDmaHIQShift);

	uint16 value = preserved | priorityMap | (uint16)kTxDmaPriorityBit2;
	fRegisterIO->Write16(kRegTrxDmaCfg, value);

	dprintf(RTL8814AU_DRIVER_NAME ": queue priority set "
		"(REG_TRXDMA_CTRL: 0x%04x -> 0x%04x)\n",
		(unsigned)existing, (unsigned)value);
	return B_OK;
}


/*! Enable the TX and RX DMA engines and configure USB-specific DMA
    settings (aggregation thresholds, burst lengths).
*/
status_t
RTL8814AUDevice::_EnableDMA()
{
	// Set RX aggregation threshold — controls how many frames the
	// hardware bundles into a single USB bulk IN transfer.
	// Lower = less latency, higher = better throughput.
	uint32 aggThreshold = 0x03;		// 3 pages before sending
	uint32 aggTimeout = 0x08;		// 8 × 32 µs = 256 µs timeout
	fRegisterIO->Write8(kRegRxDmaAggPgTh,
		(uint8)aggThreshold);
	fRegisterIO->Write8(kRegRxDmaAggPgTh + 1,
		(uint8)aggTimeout);

	return B_OK;
}


// ---------------------------------------------------------------------------
// MAC address
// ---------------------------------------------------------------------------


/*! Read the MAC address from the EFUSE map into fMacAddress[]. */
status_t
RTL8814AUDevice::_ReadMacAddress()
{
	const uint8* efuseMap = fEfuseReader->Map();
	if (efuseMap == NULL)
		return B_NO_INIT;

	memcpy(fMacAddress, efuseMap + kEfuseMacAddr, 6);

	dprintf(RTL8814AU_DRIVER_NAME ": EFUSE MAC at offset 0x%03x: "
		"%02x:%02x:%02x:%02x:%02x:%02x\n",
		kEfuseMacAddr, fMacAddress[0], fMacAddress[1], fMacAddress[2],
		fMacAddress[3], fMacAddress[4], fMacAddress[5]);

	// Sanity check — a zeroed or broadcast MAC is invalid
	bool allZero = true;
	bool allOnes = true;
	for (int32 i = 0; i < 6; i++) {
		if (fMacAddress[i] != 0x00)
			allZero = false;
		if (fMacAddress[i] != 0xFF)
			allOnes = false;
	}

	// Also reject multicast MACs (bit 0 of first byte must be 0 for unicast)
	bool isMulticast = (fMacAddress[0] & 0x01) != 0;

	if (allZero || allOnes || isMulticast) {
		dprintf(RTL8814AU_DRIVER_NAME ": invalid MAC address in EFUSE "
			"(zero=%d ones=%d mcast=%d), using random address\n",
			allZero, allOnes, isMulticast);
		// Generate a locally-administered random MAC
		fMacAddress[0] = 0x02;	// Locally administered, unicast
		fMacAddress[1] = 0x81;
		fMacAddress[2] = 0x4A;
		fMacAddress[3] = (uint8)(system_time() & 0xFF);
		fMacAddress[4] = (uint8)((system_time() >> 8) & 0xFF);
		fMacAddress[5] = (uint8)((system_time() >> 16) & 0xFF);
	}

	return B_OK;
}


/*! Write the MAC address from fMacAddress[] to the hardware register. */
status_t
RTL8814AUDevice::_WriteMacAddress()
{
	for (int32 i = 0; i < 6; i++) {
		status_t status = fRegisterIO->Write8(kRegMAC_ADDR + i,
			fMacAddress[i]);
		if (status != B_OK)
			return status;
	}
	return B_OK;
}


/*! Shut down the hardware — stop data paths, disable DMA, power down. */
void
RTL8814AUDevice::_Shutdown()
{
	dprintf(RTL8814AU_DRIVER_NAME ": shutting down hardware\n");

	// Reap the scan-notifier thread before tearing down WiFiManager —
	// otherwise it could dereference fWiFiManager after delete.
	thread_id notifier;
	{
		MutexLocker locker(fLock);
		notifier = fScanNotifierThread;
		fScanNotifierThread = -1;
	}
	if (notifier >= 0) {
		status_t threadResult;
		wait_for_thread(notifier, &threadResult);
	}

	// Reap the post-assoc worker before WiFiManager goes away.  Setting
	// fPostAssocStop = true and releasing the sem lets the worker exit
	// its loop on the next iteration.
	thread_id postAssoc = fPostAssocThread;
	fPostAssocThread = -1;
	fPostAssocStop = true;
	if (fPostAssocSem >= 0)
		release_sem_etc(fPostAssocSem, 1, B_DO_NOT_RESCHEDULE);
	if (postAssoc >= 0) {
		status_t threadResult;
		wait_for_thread(postAssoc, &threadResult);
	}

	// Reap the EAPOL 4-way worker.  Same pattern as post-assoc.
	thread_id eapol = fEapolThread;
	fEapolThread = -1;
	fEapolStop = true;
	if (fEapolReady >= 0)
		release_sem_etc(fEapolReady, 1, B_DO_NOT_RESCHEDULE);
	if (eapol >= 0) {
		status_t threadResult;
		wait_for_thread(eapol, &threadResult);
	}

	// Stop the WiFi management module (interrupt IN)
	if (fWiFiManager != NULL)
		fWiFiManager->Stop();

	// Stop the RX receive loop (bulk IN)
	if (fRxPath != NULL)
		fRxPath->Stop();

	// Cancel any pending TX transfers (bulk OUT)
	if (fTxPath != NULL)
		fTxPath->CancelAll();

	if (!fRemoved) {
		// Disable RX/TX DMA and MAC
		fRegisterIO->Write32(kRegCR, 0);

		// Disable the MCU (halt firmware)
		uint16 sysFuncEn = fRegisterIO->Read16(kRegSysFuncEn);
		sysFuncEn &= ~kSysFuncEnCpuEn;
		fRegisterIO->Write16(kRegSysFuncEn, sysFuncEn);
	}

	fHardwareInitialized = false;
}


/*! 802.11 set-parameter ioctl handler.  Dispatched from Control() for
    SIOCS80211.  args is a USER-space pointer to a struct ieee80211req;
    we copy it in, switch on i_type, and dispatch to a per-command helper.
*/
status_t
RTL8814AUDevice::_Set80211(void* userArgs, size_t length)
{
	if (length < sizeof(struct ieee80211req))
		return B_BAD_VALUE;

	struct ieee80211req request;
	if (user_memcpy(&request, userArgs, sizeof(request)) != B_OK)
		return B_BAD_ADDRESS;

	switch (request.i_type) {
		case IEEE80211_IOC_HAIKU_COMPAT_WLAN_UP:
		case IEEE80211_IOC_HAIKU_COMPAT_WLAN_DOWN:
			// Haiku-compat marker that the network stack uses to flip
			// IFF_UP without re-opening the device.  Driver open/close
			// already manages hardware state — just acknowledge.
			return B_OK;

		case IEEE80211_IOC_SCAN_REQ:
			// We currently ignore the user-supplied scan_req parameters
			// (active vs passive, ssid filters, dwell times) and just
			// kick a full-channel scan via the firmware.  Refining this
			// is straightforward once the basic flow is verified.
			return _DoScanRequest();

		case IEEE80211_IOC_SCAN_CANCEL:
			// Best-effort: not yet implemented in WiFiManager, treat as
			// a no-op.  ifconfig only issues this on user interrupt.
			return B_OK;

		case IEEE80211_IOC_SSID:
		{
			// net_server / wpa_supplicant sets the SSID we want to join.
			// The data buffer is at request.i_data (USER pointer) and
			// length is request.i_len.  Store it for use during MLME ops.
			uint32 ssidLen = request.i_len > sizeof(fJoinSsid) - 1
				? sizeof(fJoinSsid) - 1 : request.i_len;
			if (ssidLen > 0) {
				if (user_memcpy(fJoinSsid, request.i_data, ssidLen) != B_OK)
					return B_BAD_ADDRESS;
			}
			fJoinSsid[ssidLen] = 0;
			fJoinSsidLength = ssidLen;
			dprintf(RTL8814AU_DRIVER_NAME ": IOC_SSID set: '%s' (%u bytes)\n",
				fJoinSsid, (unsigned)ssidLen);
			return B_OK;
		}

		case IEEE80211_IOC_BSSID:
		{
			// Set the target BSSID we want to associate with.  Program it
			// into the chip's BSSID register so the MAC starts accepting
			// frames from this AP.
			if (request.i_len < 6)
				return B_BAD_VALUE;
			uint8 bssid[6];
			if (user_memcpy(bssid, request.i_data, 6) != B_OK)
				return B_BAD_ADDRESS;
			for (uint32 i = 0; i < 6; i++)
				fRegisterIO->Write8(kRegBSSID + i, bssid[i]);
			memcpy(fJoinBssid, bssid, 6);
			dprintf(RTL8814AU_DRIVER_NAME ": IOC_BSSID set: "
				"%02x:%02x:%02x:%02x:%02x:%02x\n",
				bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
			return B_OK;
		}

		case IEEE80211_IOC_MLME:
		{
			// Auth/Assoc/Deauth/Disassoc command from wpa_supplicant.
			struct ieee80211req_mlme mlme;
			memset(&mlme, 0, sizeof(mlme));
			uint32 copyLen = request.i_len < sizeof(mlme)
				? request.i_len : sizeof(mlme);
			if (copyLen > 0 && request.i_data != NULL)
				user_memcpy(&mlme, request.i_data, copyLen);
			static const char* opNames[] = {
				"???", "ASSOC", "DISASSOC", "DEAUTH",
				"AUTHORIZE", "UNAUTHORIZE", "AUTH"
			};
			const char* opName = mlme.im_op <= 6
				? opNames[mlme.im_op] : "???";
			dprintf(RTL8814AU_DRIVER_NAME ": IOC_MLME op=%u(%s) "
				"reason=%u mac=%02x:%02x:%02x:%02x:%02x:%02x ssid_len=%u\n",
				mlme.im_op, opName, mlme.im_reason,
				mlme.im_macaddr[0], mlme.im_macaddr[1], mlme.im_macaddr[2],
				mlme.im_macaddr[3], mlme.im_macaddr[4], mlme.im_macaddr[5],
				mlme.im_ssid_len);

			if (mlme.im_op == IEEE80211_MLME_ASSOC) {
				// wpa_supplicant has already deposited the RSN IE via
				// IOC_APPIE and set IOC_PRIVACY=1 / IOC_WPA=2.
				// _SendAssocRequest will splice fWpaIe into the assoc-req
				// body and set the Privacy bit in the capability info.
				char ssid[33];
				uint32 ssidLen = mlme.im_ssid_len < sizeof(ssid) - 1
					? mlme.im_ssid_len : sizeof(ssid) - 1;
				memcpy(ssid, mlme.im_ssid, ssidLen);
				ssid[ssidLen] = 0;
				return _DoJoin(mlme.im_macaddr, ssid, ssidLen);
			}

			if (mlme.im_op == IEEE80211_MLME_DEAUTH
				|| mlme.im_op == IEEE80211_MLME_DISASSOC) {
				// While the in-driver WPA2 handshake is in flight, ignore
				// external DEAUTH/DISASSOC.  net_server's auth-mode
				// reconciliation tries to tear us down right after our
				// successful ASSOC because it sees an open assoc on a
				// WPA2 BSS (its own state machine never finished, since
				// it routes through wpa_supplicant which can't drive
				// EAPOL on Haiku).  Once fEapolState == kEapolDone the
				// connection is fully ours and DEAUTH means user intent.
				if (fPmkValid && fEapolState != kEapolDone
						&& fEapolState != kEapolIdle) {
					dprintf(RTL8814AU_DRIVER_NAME ": ignoring external "
						"%s during in-driver WPA2 handshake "
						"(eapolState=%d)\n",
						mlme.im_op == IEEE80211_MLME_DEAUTH
							? "DEAUTH" : "DISASSOC",
						(int)fEapolState);
					return B_OK;
				}
				// Otherwise, tear down cleanly so a follow-up ASSOC can
				// start fresh.  No on-air deauth frame yet — the AP
				// times us out on its end.
				fJoinState = kJoinIdle;
				if (fWiFiManager != NULL)
					fWiFiManager->Disconnect();
				return B_OK;
			}

			// AUTH / AUTHORIZE / UNAUTHORIZE — accept and move on.
			// AUTHORIZE is the post-4-way-handshake "this STA is now
			// allowed to send data" hint; we'll wire it up alongside
			// the CCMP enable in a later commit.
			return B_OK;
		}

		case IEEE80211_IOC_APPIE:
		{
			// wpa_supplicant uses this to deposit the RSN IE that must
			// land in our assoc-req.  i_val tags the target frame type;
			// IEEE80211_APPIE_WPA means "the IE to use for WPA/RSN
			// negotiation".  i_len=0 is a clear; otherwise i_data points
			// at the IE bytes (typically <60 bytes for a RSN IE).
			if (request.i_val == IEEE80211_APPIE_WPA) {
				if (request.i_len == 0) {
					fWpaIeLength = 0;
					dprintf(RTL8814AU_DRIVER_NAME
						": IOC_APPIE WPA: cleared\n");
					return B_OK;
				}
				if (request.i_len > sizeof(fWpaIe))
					return B_BUFFER_OVERFLOW;
				if (user_memcpy(fWpaIe, request.i_data, request.i_len) != B_OK)
					return B_BAD_ADDRESS;
				fWpaIeLength = request.i_len;
				dprintf(RTL8814AU_DRIVER_NAME ": IOC_APPIE WPA: stored "
					"%u-byte RSN IE [%02x %02x %02x %02x %02x %02x %02x %02x]\n",
					(unsigned)request.i_len,
					fWpaIe[0], fWpaIe[1], fWpaIe[2], fWpaIe[3],
					fWpaIe[4], fWpaIe[5], fWpaIe[6], fWpaIe[7]);
				return B_OK;
			}
			dprintf(RTL8814AU_DRIVER_NAME ": IOC_APPIE i_val=0x%02x i_len=%u (ignored)\n",
				(unsigned)request.i_val, (unsigned)request.i_len);
			return B_OK;
		}

		case IEEE80211_IOC_DELKEY:
		{
			dprintf(RTL8814AU_DRIVER_NAME ": IOC_DELKEY i_val=%d\n",
				(int)request.i_val);
			return B_OK;
		}

		case IEEE80211_IOC_COUNTERMEASURES:
		{
			dprintf(RTL8814AU_DRIVER_NAME
				": IOC_COUNTERMEASURES SET %d\n", (int)request.i_val);
			return B_OK;
		}

		case IEEE80211_IOC_WPAKEY:
		{
			// Encryption key install — diagnostic dump.  Real
			// implementation will program the chip's security CAM.
			struct ieee80211req_key key;
			memset(&key, 0, sizeof(key));
			uint32 copyLen = request.i_len < sizeof(key)
				? request.i_len : sizeof(key);
			if (copyLen > 0 && request.i_data != NULL)
				user_memcpy(&key, request.i_data, copyLen);
			dprintf(RTL8814AU_DRIVER_NAME ": IOC_WPAKEY stub: type=%u "
				"keyix=%u flags=0x%02x keylen=%u "
				"mac=%02x:%02x:%02x:%02x:%02x:%02x rsc=%llu tsc=%llu "
				"data[0..7]=%02x%02x%02x%02x%02x%02x%02x%02x\n",
				key.ik_type, key.ik_keyix, key.ik_flags, key.ik_keylen,
				key.ik_macaddr[0], key.ik_macaddr[1], key.ik_macaddr[2],
				key.ik_macaddr[3], key.ik_macaddr[4], key.ik_macaddr[5],
				(unsigned long long)key.ik_keyrsc,
				(unsigned long long)key.ik_keytsc,
				key.ik_keydata[0], key.ik_keydata[1], key.ik_keydata[2],
				key.ik_keydata[3], key.ik_keydata[4], key.ik_keydata[5],
				key.ik_keydata[6], key.ik_keydata[7]);
			return B_OK;
		}

		case IEEE80211_IOC_AUTHMODE:
		{
			// 802.11 authentication algorithm: 1=open, 2=shared, 3=8021x.
			// wpa_supplicant SETs this to 1 (open) for WPA2-PSK because
			// the actual key derivation happens in EAPOL after assoc.
			fAuthMode = request.i_val;
			dprintf(RTL8814AU_DRIVER_NAME ": IOC_AUTHMODE SET %d\n",
				(int)request.i_val);
			return B_OK;
		}

		case IEEE80211_IOC_DROPUNENCRYPTED:
		{
			// Drop unencrypted frames once we're associated with privacy
			// on.  Bookkeeping today; chip-level drop is handled implicitly
			// once CCMP is enabled (frames without the Protected bit set
			// fail decryption and are reported as ICV errors).
			fDropUnencrypted = request.i_val;
			dprintf(RTL8814AU_DRIVER_NAME ": IOC_DROPUNENCRYPTED SET %d\n",
				(int)request.i_val);
			return B_OK;
		}

		case IEEE80211_IOC_ROAMING:
		{
			fRoaming = request.i_val;
			dprintf(RTL8814AU_DRIVER_NAME ": IOC_ROAMING SET %d\n",
				(int)request.i_val);
			return B_OK;
		}

		case IEEE80211_IOC_PRIVACY:
		{
			fPrivacy = request.i_val;
			dprintf(RTL8814AU_DRIVER_NAME ": IOC_PRIVACY SET %d\n",
				(int)request.i_val);
			return B_OK;
		}

		case IEEE80211_IOC_WPA:
		{
			fWpaMode = request.i_val;
			dprintf(RTL8814AU_DRIVER_NAME ": IOC_WPA SET %d\n",
				(int)request.i_val);
			return B_OK;
		}

		case IEEE80211_IOC_HAIKU_JOIN:
		{
			// Two callers:
			//
			//   * Legacy / net_server / open-network: i_len == 0.  Drive
			//     _DoJoin against state previously established by IOC_SSID
			//     + IOC_BSSID.  Reject if SSID hasn't been set (net_server
			//     probes the ioctl interface on bring-up before userland
			//     has set anything; we'd otherwise run _DoJoin against
			//     zeroed state and emit a noisy "no BSS matching" log).
			//
			//   * wpa2_join userland tool: i_len == sizeof(rtl_haiku_join_psk).
			//     Carry the SSID + passphrase + (optional) BSSID; we
			//     run PBKDF2-HMAC-SHA1 to derive the PMK, then drive the
			//     full WPA2 path with the in-driver 4-way handshake.
			if (request.i_len == sizeof(struct rtl_haiku_join_psk)
				&& request.i_data != NULL) {
				struct rtl_haiku_join_psk req;
				if (user_memcpy(&req, request.i_data, sizeof(req)) != B_OK)
					return B_BAD_ADDRESS;
				if (req.jp_ssid_len == 0 || req.jp_ssid_len > 32
					|| req.jp_passphrase_len == 0
					|| req.jp_passphrase_len > 63) {
					return B_BAD_VALUE;
				}

				// Derive PMK = PBKDF2(passphrase, SSID, 4096 iters, 32 bytes).
				wpa2_crypto::pbkdf2_hmac_sha1(req.jp_passphrase,
					req.jp_passphrase_len, req.jp_ssid, req.jp_ssid_len,
					4096, fPmk, 32);
				fPmkValid = true;
				fPtkValid = false;
				fEapolState = kEapolWaitM1;

				// Capture SSID / BSSID so _DoJoin's normal lookup path
				// works.  BSSID may be all-zero — _DoJoin will resolve
				// it from the scan list by SSID.
				memcpy(fJoinSsid, req.jp_ssid, req.jp_ssid_len);
				fJoinSsid[req.jp_ssid_len] = 0;
				fJoinSsidLength = req.jp_ssid_len;
				memcpy(fJoinBssid, req.jp_bssid, 6);

				// Synthesize the RSN IE for the assoc-req body.  Without
				// this, _SendAssocRequest sees fWpaIeLength == 0 and emits
				// an open-mode assoc-req — the AP accepts us as an open
				// client but the 4-way handshake never starts.  We hard-
				// code AKM 00-0F-AC:2 (PSK) with CCMP for both pairwise
				// and group cipher; if a future caller wants TKIP or a
				// different AKM we'd extend rtl_haiku_join_psk to carry
				// the cipher selectors.  The IE is identical to what
				// wpa_supplicant deposits via IOC_APPIE today.
				if (fWpaIeLength == 0) {
					static const uint8 sRsnIeCcmpPsk[22] = {
						0x30, 0x14,				// element id 48, length 20
						0x01, 0x00,				// version 1
						0x00, 0x0F, 0xAC, 0x04,	// group cipher = CCMP
						0x01, 0x00,				// pairwise count = 1
						0x00, 0x0F, 0xAC, 0x04,	// pairwise cipher = CCMP
						0x01, 0x00,				// AKM count = 1
						0x00, 0x0F, 0xAC, 0x02,	// AKM = PSK
						0x00, 0x00,				// RSN capabilities
					};
					memcpy(fWpaIe, sRsnIeCcmpPsk, sizeof(sRsnIeCcmpPsk));
					fWpaIeLength = sizeof(sRsnIeCcmpPsk);
				}

				dprintf(RTL8814AU_DRIVER_NAME ": IOC_HAIKU_JOIN (WPA2-PSK): "
					"ssid='%s' bssid=%02x:%02x:%02x:%02x:%02x:%02x "
					"PMK[0..3]=%02x%02x%02x%02x\n",
					fJoinSsid,
					fJoinBssid[0], fJoinBssid[1], fJoinBssid[2],
					fJoinBssid[3], fJoinBssid[4], fJoinBssid[5],
					fPmk[0], fPmk[1], fPmk[2], fPmk[3]);
				return _DoJoin(fJoinBssid, fJoinSsid, fJoinSsidLength);
			}

			if (fJoinSsidLength == 0) {
				dprintf(RTL8814AU_DRIVER_NAME ": IOC_HAIKU_JOIN ignored: "
					"no SSID set yet (net_server pre-config probe)\n");
				return B_NOT_INITIALIZED;
			}
			dprintf(RTL8814AU_DRIVER_NAME ": IOC_HAIKU_JOIN (open): "
				"target_ssid='%s', bssid=%02x:%02x:%02x:%02x:%02x:%02x\n",
				fJoinSsid,
				fJoinBssid[0], fJoinBssid[1], fJoinBssid[2],
				fJoinBssid[3], fJoinBssid[4], fJoinBssid[5]);
			return _DoJoin(fJoinBssid, fJoinSsid, fJoinSsidLength);
		}

		default:
		{
			// Hex-dump the first 32 bytes of i_data so we can
			// see exactly what wpa_supplicant is sending and
			// implement these ioctls one by one.
			uint8 buf[32];
			memset(buf, 0, sizeof(buf));
			uint32 copyLen = request.i_len < sizeof(buf)
				? request.i_len : sizeof(buf);
			if (copyLen > 0 && request.i_data != NULL)
				user_memcpy(buf, request.i_data, copyLen);
			dprintf(RTL8814AU_DRIVER_NAME ": SIOCS80211 unsupported "
				"i_type=%u i_val=%d i_len=%u "
				"data[0..15]=%02x%02x%02x%02x%02x%02x%02x%02x"
				"%02x%02x%02x%02x%02x%02x%02x%02x\n",
				(unsigned)request.i_type, (int)request.i_val,
				(unsigned)request.i_len,
				buf[0], buf[1], buf[2], buf[3],
				buf[4], buf[5], buf[6], buf[7],
				buf[8], buf[9], buf[10], buf[11],
				buf[12], buf[13], buf[14], buf[15]);
			return B_DEV_INVALID_IOCTL;
		}
	}
}


/*! 802.11 get-parameter ioctl handler.  Dispatched from Control() for
    SIOCG80211.  Reads the request header, dispatches on i_type, and
    writes back any updated request fields (i_len, i_val) to user space.
*/
status_t
RTL8814AUDevice::_Get80211(void* userArgs, size_t length)
{
	if (length < sizeof(struct ieee80211req))
		return B_BAD_VALUE;

	struct ieee80211req request;
	if (user_memcpy(&request, userArgs, sizeof(request)) != B_OK)
		return B_BAD_ADDRESS;

	switch (request.i_type) {
		case IEEE80211_IOC_SCAN_RESULTS:
		{
			uint16 reqLen = request.i_len;
			status_t status = _GetScanResults(request.i_data, request.i_len);
			dprintf(RTL8814AU_DRIVER_NAME ": IOC_SCAN_RESULTS GET: "
				"reqlen=%u retlen=%u status=%s\n",
				(unsigned)reqLen, (unsigned)request.i_len,
				strerror(status));
			if (status != B_OK)
				return status;

			// Write the (possibly-shrunk) length back to user space.
			return user_memcpy(userArgs, &request, sizeof(request));
		}

		case IEEE80211_IOC_BSSID:
		{
			if (fWiFiManager == NULL
				|| fWiFiManager->State() != kWiFiStateConnected) {
				return B_ERROR;
			}
			WiFiLinkState linkState;
			fWiFiManager->GetLinkState(&linkState);

			if (request.i_len < 6 || request.i_data == NULL)
				return B_BAD_VALUE;
			return user_memcpy(request.i_data, linkState.bssid, 6);
		}

		case IEEE80211_IOC_STA_INFO:
		{
			// Return basic station-status info.  We're not yet associated
			// to anything, so report zero stations.
			request.i_len = 0;
			return user_memcpy(userArgs, &request, sizeof(request));
		}

		case IEEE80211_IOC_ROAMING:
			// wpa_supplicant's bsd backend GETs this at init via
			// get80211param (which expects -1 on failure).  Returning
			// the current setting here is what unblocks init — the
			// previous "case 16: return 0" path was already correct
			// by accident, but now it's backed by fRoaming so the
			// later SET round-trips cleanly.
			request.i_val = fRoaming;
			return user_memcpy(userArgs, &request, sizeof(request));

		case IEEE80211_IOC_PRIVACY:
			request.i_val = fPrivacy;
			return user_memcpy(userArgs, &request, sizeof(request));

		case IEEE80211_IOC_WPA:
			request.i_val = fWpaMode;
			return user_memcpy(userArgs, &request, sizeof(request));

		case IEEE80211_IOC_AUTHMODE:
			request.i_val = fAuthMode;
			return user_memcpy(userArgs, &request, sizeof(request));

		case IEEE80211_IOC_DROPUNENCRYPTED:
			request.i_val = fDropUnencrypted;
			return user_memcpy(userArgs, &request, sizeof(request));

		case IEEE80211_IOC_DEVCAPS:
		{
			// wpa_supplicant calls wpa_driver_bsd_capa() at init,
			// which GETs DEVCAPS to discover what security modes the
			// driver supports.  Only dc_drivercaps is actually inspected
			// on Haiku (the cipher caps are unconditionally enabled
			// on the Haiku build path).  Advertise WPA1+WPA2 so
			// wpa_supplicant publishes those key_mgmt capabilities.
			struct ieee80211_devcaps_req_min caps;
			memset(&caps, 0, sizeof(caps));
			caps.dc_drivercaps = IEEE80211_C_WPA1 | IEEE80211_C_WPA2;

			if (request.i_len < sizeof(caps) || request.i_data == NULL)
				return B_BUFFER_OVERFLOW;
			if (user_memcpy(request.i_data, &caps, sizeof(caps)) != B_OK)
				return B_BAD_ADDRESS;
			request.i_len = sizeof(caps);
			return user_memcpy(userArgs, &request, sizeof(request));
		}

		default:
			dprintf(RTL8814AU_DRIVER_NAME
				": SIOCG80211 unsupported i_type=%u i_val=%d i_len=%u\n",
				(unsigned)request.i_type, (int)request.i_val,
				(unsigned)request.i_len);
			return B_DEV_INVALID_IOCTL;
	}
}


/*! Kick off a WiFi scan and arrange for B_NETWORK_WLAN_SCANNED to fire
    when the firmware reports completion.

    The scan itself is driven by RTL8814AUWiFiManager::StartScan(); we
    spawn a one-shot kernel thread that blocks on the manager's scan-done
    sem, fires the notification, then exits.  Userland's BNetworkDevice
    ::Scan() implementation creates a ScanListener that watches for
    exactly this notification.
*/
status_t
RTL8814AUDevice::_DoScanRequest()
{
	if (fWiFiManager == NULL)
		return B_ERROR;

	// Reject overlapping scan requests — let the previous one finish.
	{
		MutexLocker locker(fLock);
		if (fScanNotifierThread >= 0)
			return EINPROGRESS;
	}

	status_t status = fWiFiManager->StartScan();
	if (status == B_BUSY)
		return EINPROGRESS;
	if (status != B_OK)
		return status;

	// Spawn the notifier — it owns the work of publishing the scan-done
	// event after the firmware C2H event arrives.  Best-effort: if the
	// thread can't be created we still return B_OK so the scan isn't
	// wasted; the caller just won't get woken via notification (it can
	// still poll IsScanning() / fetch results).
	thread_id thread = spawn_kernel_thread(_ScanNotifierThreadEntry,
		"rtl8814au:scan_notifier", B_NORMAL_PRIORITY, this);
	if (thread >= 0) {
		{
			MutexLocker locker(fLock);
			fScanNotifierThread = thread;
		}
		resume_thread(thread);
	} else {
		dprintf(RTL8814AU_DRIVER_NAME
			": failed to spawn scan notifier: %s\n", strerror(thread));
	}

	return B_OK;
}


/*! Static thread entry — forwards to the instance loop. */
int32
RTL8814AUDevice::_ScanNotifierThreadEntry(void* arg)
{
	RTL8814AUDevice* device = static_cast<RTL8814AUDevice*>(arg);
	device->_ScanNotifierLoop();
	return 0;
}


/*! Wait for the firmware's scan-done event then publish a
    B_NETWORK_WLAN_SCANNED notification on the userland-visible
    network monitor port.

    A 30-second cap protects against a wedged firmware never sending
    the C2H event — we still clear fScanNotifierThread so a future
    scan request can spawn a new notifier.
*/
void
RTL8814AUDevice::_ScanNotifierLoop()
{
	dprintf(RTL8814AU_DRIVER_NAME ": scan-notifier: waiting up to 8s "
		"for firmware C2H scan-complete\n");
	// Wait for the firmware-issued kC2H_ScanComplete; it's likely never
	// going to arrive because our firmware-glue for that event is still
	// stubbed.  After 8 seconds we fall through and fire B_NETWORK_WLAN_SCANNED
	// regardless — by then `_ParseBeaconOrProbe` has populated fBssList
	// from passively-received beacons, which is what userland consumes.
	// Without this, wpa_supplicant blocks forever waiting for the scan
	// notification before driving WPA2 association.
	if (fWiFiManager != NULL && !fRemoved)
		fWiFiManager->WaitForScanComplete(8LL * 1000 * 1000);

	if (gNotificationModule != NULL && !fRemoved) {
		// Build the device path matching what wpa_supplicant expects.
		// driver_haiku_events.cpp's _NotifyNetworkEvent prepends "/dev/"
		// to whatever string we put in "interface", and driver_bsd's
		// drv->ifname is "/dev/net/rtl8814au/<slot>" (set from net_server's
		// BMessage `device` field).  So we need to send "net/rtl8814au/<slot>".
		// Previously we sent fDeviceName which is the friendly USB product
		// name ("ASUS USB-AC68"), causing wpa_supplicant to silently drop
		// the SCAN_RESULTS event after our scan completed and never drive
		// the WPA2 association.
		char ifPath[64];
		snprintf(ifPath, sizeof(ifPath), "%s/%" B_PRIu32,
			RTL8814AU_DEVICE_PATH_BASE, fSlotIndex);

		dprintf(RTL8814AU_DRIVER_NAME ": scan-notifier: firing "
			"B_NETWORK_WLAN_SCANNED for %s\n", ifPath);
		char messageBuffer[512];
		KMessage message;
		message.SetTo(messageBuffer, sizeof(messageBuffer),
			B_NETWORK_MONITOR);
		message.AddInt32("opcode", B_NETWORK_WLAN_SCANNED);
		message.AddString("interface", ifPath);

		status_t status = gNotificationModule->send_notification(&message);
		dprintf(RTL8814AU_DRIVER_NAME ": scan-notifier: notification "
			"sent, status=%s\n", strerror(status));
	} else {
		dprintf(RTL8814AU_DRIVER_NAME ": scan-notifier: not firing "
			"(notification module=%p removed=%d)\n",
			gNotificationModule, (int)fRemoved);
	}

	MutexLocker locker(fLock);
	fScanNotifierThread = -1;
}


/*! Format the current BSS list as a series of ieee80211req_scan_result
    records and write them into the user-supplied buffer.

    Each record is:
       [fixed ieee80211req_scan_result header]
       [SSID bytes]
       [meshid bytes — always zero-length here]
       [IE bytes — full beacon IE block from BssEntry]
       [optional pad to multiple of 4]
*/
status_t
RTL8814AUDevice::_GetScanResults(void* userBuffer, uint16& userLength)
{
	if (fWiFiManager == NULL)
		return B_ERROR;
	if (userBuffer == NULL)
		return B_BAD_VALUE;

	const uint32 kMaxResults = 64;
	BssEntry* entries = new(std::nothrow) BssEntry[kMaxResults];
	if (entries == NULL)
		return B_NO_MEMORY;

	uint32 count = fWiFiManager->GetScanResults(entries, kMaxResults);

	uint8* scratch = new(std::nothrow) uint8[userLength];
	if (scratch == NULL) {
		delete[] entries;
		return B_NO_MEMORY;
	}
	memset(scratch, 0, userLength);

	uint32 written = 0;
	for (uint32 i = 0; i < count; i++) {
		const BssEntry& bss = entries[i];
		if (!bss.valid)
			continue;

		uint32 ssidLen = bss.ssidLength;
		if (ssidLen > 32)
			ssidLen = 32;
		uint32 ieLen = bss.ieLength;
		if (ieLen > kMaxIELength)
			ieLen = kMaxIELength;

		uint32 recordLen = sizeof(struct ieee80211req_scan_result)
			+ ssidLen + ieLen;
		recordLen = (recordLen + 3) & ~3;

		if (written + recordLen > userLength)
			break;

		struct ieee80211req_scan_result* result
			= (struct ieee80211req_scan_result*)(scratch + written);
		result->isr_len = (uint16)recordLen;
		result->isr_ie_off = (uint16)sizeof(*result);
		result->isr_ie_len = (uint16)ieLen;

		// 2.4 GHz: 1..13 follow the 2407 + 5*ch formula; channel 14 is
		// JP-only (2484 MHz).  5 GHz uses 5000 + 5*ch.  The chip is 2.4
		// GHz only today, but the math is here for forward compat.
		uint16 freq = 0;
		if (bss.channel >= 1 && bss.channel <= 13)
			freq = 2407 + bss.channel * 5;
		else if (bss.channel == 14)
			freq = 2484;
		else if (bss.channel >= 36)
			freq = 5000 + bss.channel * 5;
		result->isr_freq = freq;

		result->isr_flags = (bss.channel <= 14)
			? (IEEE80211_CHAN_2GHZ | IEEE80211_CHAN_DYN)
			: (IEEE80211_CHAN_5GHZ | IEEE80211_CHAN_OFDM);
		result->isr_noise = -95;
		result->isr_rssi = bss.rssi;
		result->isr_intval = bss.beaconInterval;
		result->isr_capinfo = (uint8)(bss.capability & 0xff);
		result->isr_erp = 0;
		memcpy(result->isr_bssid, bss.bssid, 6);
		result->isr_nrates = 0;
		result->isr_ssid_len = (uint8)ssidLen;
		result->isr_meshid_len = 0;

		uint8* tail = (uint8*)(result + 1);
		memcpy(tail, bss.ssid, ssidLen);
		tail += ssidLen;
		memcpy(tail, bss.ieData, ieLen);

		written += recordLen;
	}

	status_t status = B_OK;
	if (written > 0)
		status = user_memcpy(userBuffer, scratch, written);

	delete[] entries;
	delete[] scratch;

	if (status != B_OK)
		return status;

	userLength = (uint16)written;
	return B_OK;
}


// ---------------------------------------------------------------------------
// Static device_hooks wrappers
//
// The kernel calls these static functions from the device_hooks table.
// Each one resolves the device instance from the cookie or device path,
// then dispatches to the instance. This pattern is standard across Haiku
// USB drivers.
// ---------------------------------------------------------------------------


/*! Open the device. On first open, triggers hardware initialization.
    \param name    Device path (e.g., "net/rtl8814au/0")
    \param flags   Open flags (unused)
    \param cookie  Output: device instance pointer
*/
status_t
RTL8814AUDevice::Open(const char* name, uint32 flags, void** cookie)
{
	// Extract the slot index from the device path
	const char* indexStr = strrchr(name, '/');
	if (indexStr == NULL)
		return B_BAD_VALUE;
	indexStr++;

	uint32 slotIndex = strtoul(indexStr, NULL, 10);
	if (slotIndex >= kMaxDeviceCount)
		return B_BAD_VALUE;

	MutexLocker locker(gDeviceListLock);
	RTL8814AUDevice* device = gDeviceList[slotIndex];
	if (device == NULL)
		return B_BAD_VALUE;

	if (device->fRemoved)
		return B_DEV_NOT_READY;

	// Initialize hardware on first open
	if (!device->fHardwareInitialized) {
		status_t status = device->_InitHardware();
		if (status != B_OK)
			return status;
	}

	atomic_add(&device->fOpenCount, 1);
	*cookie = device;

	dprintf(RTL8814AU_DRIVER_NAME ": device opened (open count: "
		"%" B_PRId32 ")\n", device->fOpenCount);

	return B_OK;
}


/*! Close the device. Does not release resources — that happens in Free(). */
status_t
RTL8814AUDevice::Close(void* cookie)
{
	RTL8814AUDevice* device = static_cast<RTL8814AUDevice*>(cookie);
	if (device == NULL)
		return B_BAD_VALUE;

	dprintf(RTL8814AU_DRIVER_NAME ": device close\n");

	// Disconnect from any AP
	if (device->fWiFiManager != NULL)
		device->fWiFiManager->Disconnect();

	// Stop RX and TX data paths
	if (device->fRxPath != NULL)
		device->fRxPath->Stop();

	if (device->fTxPath != NULL)
		device->fTxPath->CancelAll();

	return B_OK;
}


/*! Release the device. Decrements the open count and cleans up if
    this was the last reference and the device has been unplugged.
*/
status_t
RTL8814AUDevice::Free(void* cookie)
{
	RTL8814AUDevice* device = static_cast<RTL8814AUDevice*>(cookie);
	if (device == NULL)
		return B_BAD_VALUE;

	int32 previousCount = atomic_add(&device->fOpenCount, -1);

	dprintf(RTL8814AU_DRIVER_NAME ": device free (open count: "
		"%" B_PRId32 " → %" B_PRId32 ")\n",
		previousCount, previousCount - 1);

	// If this was the last close and the device was unplugged, clean up
	if (previousCount == 1 && device->fRemoved) {
		MutexLocker locker(gDeviceListLock);
		uint32 slot = device->fSlotIndex;
		gDeviceList[slot] = NULL;
		gDeviceCount--;
		locker.Unlock();
		delete device;
	}

	return B_OK;
}


/*! Ioctl dispatch. Handles network-related queries from the stack.
    \param cookie  Device instance
    \param op      Ioctl command (ETHER_GETADDR, etc.)
    \param args    Command-specific argument buffer
    \param length  Size of args buffer
*/
status_t
RTL8814AUDevice::Control(void* cookie, uint32 op, void* args, size_t length)
{
	RTL8814AUDevice* device = static_cast<RTL8814AUDevice*>(cookie);
	if (device == NULL || device->fRemoved)
		return B_DEV_NOT_READY;

	// Control() runs on every ifconfig/net_server poll — once
	// the link is up this can fire many times per second.  Log
	// only the first 4 invocations of each opcode so we still
	// catch unknown ops at startup but don't flood syslog.
	{
		static uint32 sControlLogTotal = 0;
		if (sControlLogTotal < 16) {
			sControlLogTotal++;
			dprintf(RTL8814AU_DRIVER_NAME ": Control op=0x%" B_PRIx32
				" len=%" B_PRIuSIZE "\n", (uint32)op, length);
		}
	}

	switch (op) {
		case ETHER_INIT:
			// ethernet_up() calls this before ETHER_GETADDR.  Treated
			// as obsolete in headers but the network stack still issues
			// it; returning anything < B_OK aborts interface bring-up.
			return B_OK;

		case ETHER_GETADDR:
		{
			// Return our MAC address
			if (length < 6)
				return B_BAD_VALUE;
			memcpy(args, device->fMacAddress, 6);
			return B_OK;
		}

		case ETHER_GETFRAMESIZE:
		{
			// Return maximum frame size (standard Ethernet MTU)
			if (length < sizeof(uint32))
				return B_BAD_VALUE;
			*(uint32*)args = 1514;	// 6+6+2 header + 1500 payload
			return B_OK;
		}

		case ETHER_SET_LINK_STATE_SEM:
		{
			// The network stack provides a semaphore that we should
			// release whenever the link state changes (connect/disconnect).
			if (length < sizeof(sem_id))
				return B_BAD_VALUE;
			device->fLinkStateSem = *(sem_id*)args;
			return B_OK;
		}

		case ETHER_GET_LINK_STATE:
		{
			// Report the current WiFi link state (media type, quality, speed)
			if (length < sizeof(ether_link_state_t))
				return B_BAD_VALUE;

			ether_link_state_t* linkState = (ether_link_state_t*)args;

			if (device->fWiFiManager != NULL
				&& device->fWiFiManager->State() == kWiFiStateConnected) {
				WiFiLinkState wifiState;
				device->fWiFiManager->GetLinkState(&wifiState);

				linkState->media = IFM_IEEE80211 | IFM_ACTIVE;

				// Convert RSSI to quality percentage (0-1000).
				// Typical range: -90 dBm (worst) to -30 dBm (best)
				int32 quality = (wifiState.rssi + 90) * 1000 / 60;
				if (quality < 0) quality = 0;
				if (quality > 1000) quality = 1000;
				linkState->quality = (uint32)quality;

				// Estimate link speed from the data rate index.
				// Use approximate rates for common modes.
				uint64 speed = 24000000;	// Default: 24 Mbps
				if (wifiState.dataRate >= kRateVHT_1SS_MCS0)
					speed = 866700000;		// VHT: up to ~867 Mbps
				else if (wifiState.dataRate >= kRateHT_MCS0)
					speed = 300000000;		// HT: up to 300 Mbps
				else if (wifiState.dataRate >= kRateOFDM54)
					speed = 54000000;
				else if (wifiState.dataRate >= kRateOFDM6) {
					static const uint64 kOfdmRates[] =
						{ 6, 9, 12, 18, 24, 36, 48, 54 };
					uint32 rateIdx = wifiState.dataRate - kRateOFDM6;
					if (rateIdx < 8)
						speed = kOfdmRates[rateIdx] * 1000000;
				}
				linkState->speed = speed;
			} else {
				// Not connected
				linkState->media = IFM_IEEE80211;
				linkState->quality = 0;
				linkState->speed = 0;
			}
			return B_OK;
		}

		case ETHER_SETPROMISC:
		{
			// Enable or disable promiscuous mode by adjusting the RCR
			int32 enabled = *(int32*)args;
			uint32 rcr = device->fRegisterIO->Read32(kRegRCR);
			if (enabled)
				rcr |= kRCR_AAP;	// Accept all packets
			else
				rcr &= ~kRCR_AAP;
			device->fRegisterIO->Write32(kRegRCR, rcr);
			return B_OK;
		}

		case ETHER_NONBLOCK:
		{
			// Non-blocking mode toggle — not currently supported
			// (Read() always blocks on the RX semaphore)
			return B_OK;
		}

		case SIOCSIFMEDIA:
			// wpa_supplicant's bsd_set_mediaopt calls this before each
			// scan to flip the interface to STA mode (IFM_OMASK -> 0).
			// On Haiku our wireless driver is always effectively in STA
			// mode, so the actual media value doesn't matter.  Returning
			// failure here makes wpa_driver_bsd_scan early-exit and
			// IOC_SCAN_REQ never fires, which is what blocked WPA2
			// progress through this commit.  Accept the call.
			return B_OK;

		case SIOCS80211:
			// 802.11 set-parameter ioctl from userland.  args is a USER
			// pointer to a struct ieee80211req — the per-i_type handler
			// is responsible for any further user_memcpy()s.
			return device->_Set80211(args, length);

		case SIOCG80211:
			// 802.11 get-parameter ioctl from userland.  Same USER-pointer
			// rules as SIOCS80211; handler may also write back into the
			// caller-supplied i_data buffer.
			return device->_Get80211(args, length);

		default:
		{
			// First few unknown ops get logged so we can identify which
			// SIOC*/ETHER_* codes the kernel forwards but we don't yet
			// handle.  Currently chasing SIOCGIFMEDIA (8925) and
			// SIOCSIFMEDIA (8924) which wpa_supplicant's bsd_set_mediaopt
			// requires before it'll issue IOC_SCAN_REQ.
			static uint32 sUnknownLogged = 0;
			if (sUnknownLogged < 32) {
				sUnknownLogged++;
				dprintf(RTL8814AU_DRIVER_NAME ": Control unknown op=0x%"
					B_PRIx32 " (%" B_PRIu32 ") len=%" B_PRIuSIZE "\n",
					(uint32)op, (uint32)op, length);
			}
			return B_DEV_INVALID_IOCTL;
		}
	}
}


/*! Read data from the device (RX path). Blocks until a frame is available
    in the RX ring buffer, then copies it to the caller's buffer.

    The RX ring is filled by _RxFrameReceived() which is called from
    the RxPath USB bulk IN callback. This function dequeues the next
    frame and returns it.
*/
status_t
RTL8814AUDevice::Read(void* cookie, off_t position, void* buffer,
	size_t* numBytes)
{
	RTL8814AUDevice* device = static_cast<RTL8814AUDevice*>(cookie);
	if (device == NULL || device->fRemoved)
		return B_DEV_NOT_READY;

	if (buffer == NULL || numBytes == NULL)
		return B_BAD_VALUE;

	// Block until a frame is available or the device is removed.
	// B_CAN_INTERRUPT allows the read to be canceled when the device
	// is closed or the thread is killed.
	status_t status = acquire_sem_etc(device->fRxDataReady, 1,
		B_CAN_INTERRUPT, 0);
	if (status != B_OK) {
		*numBytes = 0;
		return status;
	}

	// Check again after waking — the device may have been removed
	if (device->fRemoved) {
		*numBytes = 0;
		return B_DEV_NOT_READY;
	}

	// Dequeue the next frame from the ring buffer
	MutexLocker locker(device->fLock);

	uint32 tail = device->fRxRingTail;
	RxRingEntry* entry = &device->fRxRing[tail];

	if (entry->length == 0) {
		// Ring was empty despite the semaphore — shouldn't happen,
		// but handle gracefully
		locker.Unlock();
		*numBytes = 0;
		return B_OK;
	}

	// Copy the frame to the caller's buffer
	uint32 copyLength = entry->length;
	if (copyLength > *numBytes)
		copyLength = (uint32)*numBytes;

	memcpy(buffer, entry->data, copyLength);
	*numBytes = copyLength;

	// Mark this slot as consumed and advance the tail
	entry->length = 0;
	device->fRxRingTail = (tail + 1) % kRxRingSlots;

	return B_OK;
}


/*! Write data to the device (TX path). The network stack calls this
    to transmit frames. The frame is wrapped in a TX descriptor and
    submitted on the appropriate bulk OUT endpoint.
*/
status_t
RTL8814AUDevice::Write(void* cookie, off_t position, const void* buffer,
	size_t* numBytes)
{
	RTL8814AUDevice* device = static_cast<RTL8814AUDevice*>(cookie);
	if (device == NULL || device->fRemoved)
		return B_DEV_NOT_READY;

	if (device->fTxPath == NULL)
		return B_NO_INIT;

	if (buffer == NULL || numBytes == NULL || *numBytes == 0)
		return B_BAD_VALUE;

	// The network stack hands us an Ethernet frame:
	//   [6] dst MAC, [6] src MAC, [2] ethertype, [N] payload
	// Convert to an 802.11 infrastructure-mode data frame:
	//   FrameControl (Data, ToDS=1), Dur, Addr1=BSSID, Addr2=our MAC,
	//   Addr3=real dst, SeqCtrl, LLC/SNAP, payload.
	// Without this conversion the chip TX's our ethernet frame as a
	// raw 802.11 payload and the AP can't parse it — DHCP DISCOVERs go
	// out but the gateway never sees them.
	if (*numBytes < 14) {
		*numBytes = 0;
		return B_BAD_VALUE;
	}
	if (device->fJoinState != kJoinConnected) {
		// No association — drop the frame.
		*numBytes = 0;
		return B_DEV_NOT_READY;
	}

	const uint8* eth = static_cast<const uint8*>(buffer);
	const uint8* dstMAC = &eth[0];
	const uint8* srcMAC = &eth[6];
	uint16 etherType = ((uint16)eth[12] << 8) | eth[13];
	const uint8* payload = &eth[14];
	uint32 payloadLen = (uint32)*numBytes - 14;

	// EAPOL frame — log so we can see the 4-way handshake from
	// our side too.
	if (etherType == 0x888E) {
		dprintf(RTL8814AU_DRIVER_NAME ": TX EAPOL %u bytes payload "
			"[%02x %02x %02x %02x %02x %02x %02x %02x]\n",
			(unsigned)payloadLen,
			payloadLen > 0 ? payload[0] : 0,
			payloadLen > 1 ? payload[1] : 0,
			payloadLen > 2 ? payload[2] : 0,
			payloadLen > 3 ? payload[3] : 0,
			payloadLen > 4 ? payload[4] : 0,
			payloadLen > 5 ? payload[5] : 0,
			payloadLen > 6 ? payload[6] : 0,
			payloadLen > 7 ? payload[7] : 0);
	}

	bool isBroadcast = (dstMAC[0] & 0x01) != 0;

	// Build the 802.11 + LLC/SNAP header on the stack.  Max payload
	// fits comfortably under the chip's TX buffer (kUsbTxBufferSize)
	// after the 24+8 byte header and 40-byte TX descriptor.
	uint8 frame[24 + 8 + 1600];
	if (payloadLen > sizeof(frame) - 32) {
		*numBytes = 0;
		return B_BUFFER_OVERFLOW;
	}

	// Encrypt outbound data frames once CCMP is set up.  EAPOL frames
	// (etherType 0x888E) stay unencrypted — the only EAPOL we'd TX
	// here is a rekey M2/M4, and those go in clear by 802.11i rule.
	//
	// We do CCMP encryption *in software* (wpa2_crypto::ccmp_encrypt)
	// because the chip's HW encrypt engine fails the same way HW
	// decrypt does — outbound CCMP frames go on-air either unencrypted
	// or garbled, and the AP drops them.  TX descriptor's SecType is
	// kSecurityNone so the chip just transmits whatever we hand it.
	bool encryptThisFrame = device->fCcmpEnabled
		&& etherType != 0x888E;

	// 802.11 header.  Protected bit gets set by ccmp_encrypt below
	// after the body is encrypted; for plaintext frames we leave it 0.
	frame[0] = 0x08;			// FC[0]: Data frame (type 2, subtype 0)
	frame[1] = 0x01;			// FC[1]: ToDS=1
	frame[2] = 0;				// Duration low
	frame[3] = 0;				// Duration high
	memcpy(&frame[4], device->fJoinBssid, 6);	// Address1 = BSSID
	memcpy(&frame[10], srcMAC, 6);				// Address2 = source
	memcpy(&frame[16], dstMAC, 6);				// Address3 = real dest
	frame[22] = 0;			// SeqCtrl low (HW will fill via HWSEQ_EN)
	frame[23] = 0;			// SeqCtrl high

	// LLC/SNAP encapsulation for IP-over-802.11
	frame[24] = 0xAA;		// LLC DSAP
	frame[25] = 0xAA;		// LLC SSAP
	frame[26] = 0x03;		// LLC control
	frame[27] = 0x00;		// SNAP OUI[0]
	frame[28] = 0x00;		// SNAP OUI[1]
	frame[29] = 0x00;		// SNAP OUI[2]
	frame[30] = (uint8)(etherType >> 8);
	frame[31] = (uint8)(etherType & 0xFF);

	memcpy(&frame[32], payload, payloadLen);
	uint32 txFrameLen = 32 + payloadLen;

	// SW CCMP encrypt: replace the plaintext body with CCMP IV +
	// encrypted body + MIC, set the Protected bit.  Frame buffer
	// already has plenty of headroom (sizeof(frame) - 32 > payloadLen
	// + 16) so the in-place expansion fits.
	if (encryptThisFrame) {
		static uint32 sTxEnter = 0;
		if (++sTxEnter <= 6) {
			dprintf(RTL8814AU_DRIVER_NAME ": TX encrypt entry #%u "
				"ethertype=%04x bcast=%d gtkValid=%d ptkValid=%d "
				"ccmpEnabled=%d txFrameLen=%u\n",
				(unsigned)sTxEnter, etherType, isBroadcast ? 1 : 0,
				device->fGtkValid ? 1 : 0,
				device->fPtkValid ? 1 : 0,
				device->fCcmpEnabled ? 1 : 0,
				(unsigned)txFrameLen);
		}

		if (txFrameLen + 16 > sizeof(frame)) {
			dprintf(RTL8814AU_DRIVER_NAME ": TX encrypt: buffer overflow\n");
			*numBytes = 0;
			return B_BUFFER_OVERFLOW;
		}
		// Per IEEE 802.11i, a STA always uses the *pairwise* key for
		// its own outbound TX, even when the logical destination
		// (Addr3) is broadcast/multicast.  The frame is sent unicast
		// at the link layer (Addr1 = BSSID) so only the AP needs to
		// decrypt it, and it does so with the pairwise key.  The AP
		// then re-broadcasts using the group key.  Using GTK here
		// would make the AP fail decrypt and silently drop the
		// frame — exactly the symptom we were seeing with DHCP.
		if (!device->fPtkValid) {
			dprintf(RTL8814AU_DRIVER_NAME ": TX encrypt: no PTK\n");
			*numBytes = 0;
			return B_DEV_NOT_READY;
		}
		const uint8* key = device->fPtk + 32;	// TK = PTK[32..47]
		uint64 pn = device->fTxPnPairwise++;
		uint8 keyId = 0;
		// hdrLen = 24 (802.11 header only).  LLC/SNAP at offset 24..31
		// is part of the body that gets encrypted along with the IP
		// payload — passing 32 here would skip past the LLC and
		// ccmp_encrypt would treat it as if there were no body to
		// encrypt past offset 32, plus fail the 24/26 hdrLen check.
		uint32 newLen = wpa2_crypto::ccmp_encrypt(key, frame, txFrameLen,
			24, pn, keyId);
		if (newLen == 0) {
			dprintf(RTL8814AU_DRIVER_NAME ": TX encrypt: ccmp_encrypt "
				"returned 0 (bad hdrLen?)\n");
			*numBytes = 0;
			return B_ERROR;
		}
		txFrameLen = newLen;

		static uint32 sCcmpEncOk = 0;
		if (++sCcmpEncOk <= 4) {
			dprintf(RTL8814AU_DRIVER_NAME ": SW CCMP encrypt OK #%u "
				"len=%u bcast=%d ethertype=%04x pn=%llu\n",
				(unsigned)sCcmpEncOk, (unsigned)txFrameLen,
				isBroadcast ? 1 : 0, etherType,
				(unsigned long long)pn);
		}
	}

	status_t status = device->fTxPath->Transmit(frame,
		txFrameLen, kTxQueueBE, kRateCCK1, 0,
		kSecurityNone, isBroadcast);

	if (status != B_OK)
		*numBytes = 0;

	return status;
}


/*! Static RX frame callback — called by the RX path module for each
    successfully received frame.

    This callback runs in the USB completion thread context. It must
    copy the frame data into the ring buffer before returning, since the
    USB receive buffer will be re-submitted immediately after.

    Management frames (beacons, probe responses) are parsed for BSS info
    and passed to the WiFi manager. Data frames are queued into the ring
    buffer for Read() to deliver to the network stack.
*/
void
RTL8814AUDevice::_RxFrameReceived(void* cookie, const uint8* frameData,
	uint32 frameLength, const RxFrameInfo* info)
{
	RTL8814AUDevice* device = static_cast<RTL8814AUDevice*>(cookie);
	if (device == NULL || frameLength < 24)
		return;

	// The first two bytes of the 802.11 frame contain the frame control.
	// Bits [3:2] of the first byte indicate the frame type:
	//   0 = management, 1 = control, 2 = data

	uint8 frameType = (frameData[0] >> 2) & 0x03;
	uint8 frameSubtype = (frameData[0] >> 4) & 0x0F;


	if (frameType == 0 && device->fWiFiManager != NULL) {
		// Diag: count mgmt subtypes
		static uint32 sMgmtStats[16] = {0};
		sMgmtStats[frameSubtype]++;
		// In a busy environment with ~20 networks beaconing, mgmt
		// frames arrive at ~50/sec.  Log every 5000 frames (~every
		// 100 sec) so this stays a heartbeat, not a fire hose.
		static uint32 sMgmtTick = 0;
		if (++sMgmtTick >= 5000) {
			sMgmtTick = 0;
			dprintf(RTL8814AU_DRIVER_NAME ": mgmt subtypes: beacon(8)=%u probe(5)=%u auth(11)=%u assoc(1)=%u disasoc(10)=%u\n",
				(unsigned)sMgmtStats[8], (unsigned)sMgmtStats[5],
				(unsigned)sMgmtStats[11], (unsigned)sMgmtStats[1],
				(unsigned)sMgmtStats[10]);
		}
		// Management frames:
		//   8 = beacon, 5 = probe-resp -> BSS list
		//   11 = authentication -> auth state machine
		//   1 = associate-resp -> assoc state machine
		if (frameSubtype == 8 || frameSubtype == 5)
			device->_ParseBeaconOrProbe(frameData, frameLength, info);
		else if (frameSubtype == 11)
			device->_HandleAuthResponse(frameData, frameLength);
		else if (frameSubtype == 1)
			device->_HandleAssocResponse(frameData, frameLength);
		// Don't queue management frames into the data ring buffer
		return;
	}

	if (frameType != 2) {
		// Control frames (type 1) are not queued — they're handled
		// entirely in hardware/firmware
		return;
	}

	// Diag: count data frames + log details for frames *from our AP*
	// (Addr2 = BSSID matches our fJoinBssid).  Other BSSes don't help
	// us debug our own crypto.  Heartbeat every 64 total data frames.
	{
		static uint32 sDataTotal = 0;
		static uint32 sDataProtected = 0;
		static uint32 sDataBroadcast = 0;
		static uint32 sFromOurAp = 0;
		static uint32 sFromApLogged = 0;
		uint8 fc1 = frameData[1];
		bool prot = (fc1 & 0x40) != 0;
		bool bcast = (frameData[4] & 0x01) != 0;	// Addr1 (RA) multicast bit
		bool fromDS = (fc1 & 0x02) != 0;
		// Addr2 in FromDS=1 is the BSSID; in FromDS=0 it's the source.
		// For frames the AP sends to us (FromDS=1), Addr2 = BSSID.
		const uint8* a2 = &frameData[10];
		bool fromOurAp = (memcmp(a2, device->fJoinBssid, 6) == 0)
			&& fromDS;
		sDataTotal++;
		if (prot) sDataProtected++;
		if (bcast) sDataBroadcast++;
		if (fromOurAp) sFromOurAp++;
		if (fromOurAp && sFromApLogged < 16) {
			sFromApLogged++;
			dprintf(RTL8814AU_DRIVER_NAME ": RX from AP #%u fc=%02x:%02x "
				"prot=%d bcast=%d swdec=%d icverr=%d len=%u "
				"@24:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
				(unsigned)sFromOurAp, frameData[0], fc1,
				prot ? 1 : 0, bcast ? 1 : 0,
				info != NULL && info->swDecNeeded ? 1 : 0,
				info != NULL && info->icvError ? 1 : 0,
				(unsigned)frameLength,
				frameData[24], frameData[25], frameData[26],
				frameData[27], frameData[28], frameData[29],
				frameData[30], frameData[31]);
		}
		if ((sDataTotal & 0xFF) == 0) {
			dprintf(RTL8814AU_DRIVER_NAME ": data RX heartbeat: "
				"total=%u protected=%u bcast=%u fromOurAp=%u\n",
				(unsigned)sDataTotal, (unsigned)sDataProtected,
				(unsigned)sDataBroadcast, (unsigned)sFromOurAp);
		}
	}

	// Data frame — convert from 802.11 + LLC/SNAP to ethernet so
	// the network stack (which sees us as Hardware type: Ethernet)
	// can parse it.  Without this, the kernel tries to read bytes
	// 0-13 of the 802.11 header as an ethernet header, fails, and
	// silently drops every DHCP / ARP / IP packet we receive.
	//
	// 802.11 frame layout (FromDS=1, what we get from the AP):
	//   [2] FC, [2] Dur, [6] Addr1=our MAC, [6] Addr2=BSSID,
	//   [6] Addr3=original src, [2] SeqCtrl, [8] LLC/SNAP, [N] payload
	//
	// Ethernet output:
	//   [6] dst=Addr1, [6] src=Addr3, [2] ethertype, [N] payload
	const uint32 k80211HeaderLen = 24;
	const uint32 kLLCSnapLen = 8;
	if (frameLength < k80211HeaderLen + kLLCSnapLen)
		return;

	// SW CCMP decrypt fallback.  The chip's HW crypto engine refuses
	// to decrypt CCMP frames in our setup (signaled via the SWDEC bit
	// in the RX descriptor) — every CAM bit and SECCFG bit is right
	// per morrownr's reference but the engine still bails.  When it
	// hands us an encrypted frame, decrypt in software using the keys
	// we derived during the 4-way handshake.
	//
	// Decrypted output goes into `decryptBuf`, and we re-point
	// `frameData` / `frameLength` at the (shorter, plaintext) frame.
	// If decrypt fails we just drop the frame here.
	uint8 decryptBuf[2400];
	if ((frameData[1] & 0x40) != 0 && device->fCcmpEnabled) {
		if (frameLength > sizeof(decryptBuf))
			return;
		if (frameLength < k80211HeaderLen + 8 + 8)
			return;
		memcpy(decryptBuf, frameData, frameLength);

		bool isBcast = (decryptBuf[4] & 0x01) != 0;
		const uint8* key;
		if (isBcast) {
			if (!device->fGtkValid)
				return;
			key = device->fGtk;
		} else {
			if (!device->fPtkValid)
				return;
			key = device->fPtk + 32;	// TK = PTK[32..47]
		}

		if (!wpa2_crypto::ccmp_decrypt(key, decryptBuf, frameLength,
				k80211HeaderLen)) {
			static uint32 sCcmpFails = 0;
			if (++sCcmpFails <= 4) {
				dprintf(RTL8814AU_DRIVER_NAME ": SW CCMP decrypt "
					"failed (#%u, len=%u, bcast=%d)\n",
					(unsigned)sCcmpFails, (unsigned)frameLength,
					isBcast ? 1 : 0);
			}
			return;
		}

		// Strip the 8-byte CCMP IV header by collapsing it: shift
		// plaintext (now sitting at [hdr+8 .. frameLen-9]) up over
		// the IV.  Trim the trailing 8-byte MIC by reducing length.
		uint32 payloadStart = k80211HeaderLen + 8;
		uint32 payloadLen = frameLength - payloadStart - 8;
		memmove(decryptBuf + k80211HeaderLen,
			decryptBuf + payloadStart, payloadLen);
		decryptBuf[1] &= ~0x40;		// clear Protected bit
		frameData = decryptBuf;
		frameLength = k80211HeaderLen + payloadLen;

		static uint32 sCcmpOk = 0;
		if (++sCcmpOk <= 4) {
			dprintf(RTL8814AU_DRIVER_NAME ": SW CCMP decrypt OK "
				"#%u len=%u bcast=%d ethertype=%02x:%02x\n",
				(unsigned)sCcmpOk, (unsigned)frameLength,
				isBcast ? 1 : 0,
				frameData[k80211HeaderLen + 6],
				frameData[k80211HeaderLen + 7]);
		}
	}

	// Verify LLC/SNAP encapsulation; if it isn't RFC1042-style,
	// the frame isn't IP/ARP and we'd be feeding garbage upstream.
	const uint8* llc = frameData + k80211HeaderLen;
	if (llc[0] != 0xAA || llc[1] != 0xAA || llc[2] != 0x03) {
		// Diag: log first few frames that fail LLC check.  Could be
		// CCMP frames the chip didn't decrypt (IV header at offset
		// 24 instead of LLC), or some other unexpected layout.
		static uint32 sLlcFailLogged = 0;
		if (sLlcFailLogged < 6) {
			sLlcFailLogged++;
			dprintf(RTL8814AU_DRIVER_NAME ": RX LLC-fail #%u "
				"fc=%02x:%02x swdec=%d icverr=%d len=%u "
				"a3=%02x:%02x:%02x:%02x:%02x:%02x "
				"@24:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x "
				"@32:%02x:%02x:%02x:%02x\n",
				(unsigned)sLlcFailLogged,
				frameData[0], frameData[1],
				info != NULL && info->swDecNeeded ? 1 : 0,
				info != NULL && info->icvError ? 1 : 0,
				(unsigned)frameLength,
				frameData[16], frameData[17], frameData[18],
				frameData[19], frameData[20], frameData[21],
				frameData[24], frameData[25], frameData[26],
				frameData[27], frameData[28], frameData[29],
				frameData[30], frameData[31],
				frameData[32], frameData[33], frameData[34],
				frameData[35]);
		}
		return;
	}

	uint32 payloadLen = frameLength - k80211HeaderLen - kLLCSnapLen;
	uint32 ethLen = 14 + payloadLen;

	// EAPOL frame (ethertype 0x888E) — divert to the in-driver WPA2
	// state machine instead of pushing into the data ring.  Haiku's
	// network stack does not deliver non-IP / non-ARP ethertypes to
	// AF_LINK packet sockets, so wpa_supplicant cannot see EAPOL via
	// the normal path.  We run the 4-way handshake in-kernel using the
	// PMK supplied via IOC_HAIKU_JOIN's rich struct, then program the
	// derived PTK + GTK directly into the chip's security CAM.
	if (llc[6] == 0x88 && llc[7] == 0x8E) {
		MutexLocker locker(device->fLock);

		uint8 fcByte0 = frameData[0];
		uint8 protectedBit = (frameData[1] >> 6) & 0x01;

		if (payloadLen > sizeof(device->fEapolInbox.payload)) {
			dprintf(RTL8814AU_DRIVER_NAME ": RX EAPOL too large for "
				"inbox: %u bytes (max %u) — dropping\n",
				(unsigned)payloadLen,
				(unsigned)sizeof(device->fEapolInbox.payload));
			return;
		}

		// Source MAC is Addr3 of the 802.11 header (the AP's BSSID
		// for an AP-to-STA EAPOL frame in FromDS=1).
		memcpy(device->fEapolInbox.senderMac, &frameData[16], 6);
		memcpy(device->fEapolInbox.payload,
			&frameData[k80211HeaderLen + kLLCSnapLen], payloadLen);
		device->fEapolInbox.length = payloadLen;
		device->fEapolPending = true;

		// Heavy diagnostic dropped.  The EAPOL worker logs the parsed
		// frame in _HandleEapolFrame; logging here too produces enough
		// per-frame syslog volume during AP retries to expose a latent
		// BFS race in Haiku's vnode finalization (the
		// acquire_vnode-wasn't-used panic from 2026-05-04).
		(void)protectedBit;
		(void)fcByte0;

		locker.Unlock();

		// Wake any thread blocked on the EAPOL state machine.  The
		// state machine itself is added in a follow-up commit; for now
		// the sem release is just a future hook and harmless if nothing
		// is waiting.
		release_sem_etc(device->fEapolReady, 1, B_DO_NOT_RESCHEDULE);
		return;
	}

	if (ethLen > kRxRingFrameMaxSize) {
		dprintf(RTL8814AU_DRIVER_NAME ": RX frame too large for ring: "
			"%" B_PRIu32 " bytes\n", ethLen);
		return;
	}

	MutexLocker locker(device->fLock);

	uint32 head = device->fRxRingHead;
	uint32 nextHead = (head + 1) % kRxRingSlots;

	// Check if the ring is full (head would wrap around to tail)
	if (nextHead == device->fRxRingTail) {
		// Ring full — drop the oldest frame to make room
		device->fRxRing[device->fRxRingTail].length = 0;
		device->fRxRingTail = (device->fRxRingTail + 1) % kRxRingSlots;
		// Consume the semaphore count for the dropped frame
		acquire_sem_etc(device->fRxDataReady, 1, B_RELATIVE_TIMEOUT, 0);
	}

	// Build the ethernet frame in-place in the ring slot.
	RxRingEntry* entry = &device->fRxRing[head];
	memcpy(&entry->data[0], &frameData[4], 6);		// dst = Addr1
	memcpy(&entry->data[6], &frameData[16], 6);		// src = Addr3
	entry->data[12] = llc[6];		// ethertype hi
	entry->data[13] = llc[7];		// ethertype lo
	memcpy(&entry->data[14], &frameData[k80211HeaderLen + kLLCSnapLen],
		payloadLen);
	entry->length = ethLen;

	device->fRxRingHead = nextHead;
	locker.Unlock();

	// Wake up any thread blocked in Read()
	release_sem_etc(device->fRxDataReady, 1, B_DO_NOT_RESCHEDULE);
}


// ---------------------------------------------------------------------------
// Beacon / Probe Response parsing
//
// 802.11 management frame layout for beacons and probe responses:
//   Bytes 0-1:   Frame Control
//   Bytes 2-3:   Duration
//   Bytes 4-9:   Destination Address (DA)
//   Bytes 10-15: Source Address (SA)
//   Bytes 16-21: BSSID
//   Bytes 22-23: Sequence Control
//   --- Fixed fields (beacon body) ---
//   Bytes 24-31: Timestamp (8 bytes)
//   Bytes 32-33: Beacon Interval (2 bytes, in TUs = 1024 µs)
//   Bytes 34-35: Capability Information (2 bytes)
//   Bytes 36+:   Information Elements (tagged parameters)
//
// Each Information Element (IE) has:
//   Byte 0: Element ID
//   Byte 1: Length
//   Bytes 2+: Element data
//
// We extract: SSID (IE 0), DS Parameter Set (IE 3, channel),
// RSN (IE 48, WPA2), and vendor-specific (IE 221, WPA1).
// ---------------------------------------------------------------------------


/*! Parse a beacon or probe response management frame and update the
    WiFi manager's BSS list with the access point information.
*/
void
RTL8814AUDevice::_ParseBeaconOrProbe(const uint8* frameData,
	uint32 frameLength, const RxFrameInfo* rxInfo)
{
	// Minimum size: 24-byte MAC header + 12-byte fixed fields = 36 bytes
	if (frameLength < 36)
		return;

	if (fWiFiManager == NULL)
		return;

	// Extract the BSSID from the MAC header (bytes 16-21)
	const uint8* bssid = frameData + 16;

	// Extract fixed fields from the beacon body
	uint16 beaconInterval = (uint16)frameData[32]
		| ((uint16)frameData[33] << 8);
	uint16 capability = (uint16)frameData[34]
		| ((uint16)frameData[35] << 8);

	// Walk the Information Elements starting at byte 36
	const uint8* ieStart = frameData + 36;
	uint32 ieTotal = frameLength - 36;

	char ssid[kMaxSSIDLength + 1];
	uint8 ssidLength = 0;
	uint8 channel = 0;
	SecurityType security = kSecurityNone;
	bool foundSSID = false;

	memset(ssid, 0, sizeof(ssid));

	uint32 offset = 0;
	while (offset + 2 <= ieTotal) {
		uint8 ieID = ieStart[offset];
		uint8 ieLen = ieStart[offset + 1];

		// Bounds check
		if (offset + 2 + ieLen > ieTotal)
			break;

		const uint8* ieData = ieStart + offset + 2;

		switch (ieID) {
			case 0:		// SSID
				ssidLength = ieLen;
				if (ssidLength > kMaxSSIDLength)
					ssidLength = kMaxSSIDLength;
				memcpy(ssid, ieData, ssidLength);
				ssid[ssidLength] = '\0';
				foundSSID = true;
				break;

			case 3:		// DS Parameter Set (channel)
				if (ieLen >= 1)
					channel = ieData[0];
				break;

			case 48:	// RSN (Robust Security Network) — WPA2
				security = kSecurityAESCCMP;
				break;

			case 221:	// Vendor-specific — check for WPA1 OUI
				// Microsoft WPA OUI: 00:50:F2:01
				if (ieLen >= 4 && ieData[0] == 0x00 && ieData[1] == 0x50
					&& ieData[2] == 0xF2 && ieData[3] == 0x01) {
					if (security == kSecurityNone)
						security = kSecurityTKIP;
				}
				break;
		}

		offset += 2 + ieLen;
	}

	// Skip hidden networks (empty SSID)
	if (!foundSSID || ssidLength == 0)
		return;

	// If no channel was in the DS Parameter IE, use the channel from
	// the RX descriptor
	if (channel == 0 && rxInfo != NULL)
		channel = 0;	// RxFrameInfo doesn't carry channel; leave 0

	// Forward parsed data to the WiFi manager's BSS list
	_UpdateBssEntry(bssid, ssid, ssidLength, channel, beaconInterval,
		capability, security, rxInfo != NULL ? rxInfo->rssi : -80,
		ieStart, ieTotal);
}


/*! Forward parsed BSS info to the WiFi manager's UpdateBssEntry(). */
void
RTL8814AUDevice::_UpdateBssEntry(const uint8* bssid, const char* ssid,
	uint8 ssidLength, uint8 channel, uint16 beaconInterval,
	uint16 capability, SecurityType security, int8 rssi,
	const uint8* ieData, uint32 ieLength)
{
	if (fWiFiManager != NULL) {
		fWiFiManager->UpdateBssEntry(bssid, ssid, ssidLength, channel,
			beaconInterval, capability, security, rssi, ieData, ieLength);
	}
}


// ---------------------------------------------------------------------------
// Open-network join state machine (auth + assoc)
// ---------------------------------------------------------------------------

/*! Entry point for IOC_HAIKU_JOIN — drives an open-network auth+assoc
    handshake to the supplied BSSID.  Returns immediately after
    submitting the auth request; completion is signaled asynchronously
    when _HandleAuthResponse / _HandleAssocResponse run from the RX
    path (which run on the USB callback thread).
*/
status_t
RTL8814AUDevice::_DoJoin(const uint8* bssid, const char* ssid,
	uint32 ssidLen)
{
	// Reset chip security state before starting a fresh association.
	// USB device state persists across host OS reboot, so a prior
	// session may have left SECCFG with TX/RX-encrypt bits enabled and
	// stale CAM entries with VALID set.  If we don't clear them, our
	// own EAPOL M2 will get mangled by the chip's TX path (it tries
	// to encrypt with whatever pairwise key it last had), the AP
	// never sees a valid M2, and the handshake stalls in M1 retries.
	// Use the full CAMCMD CLR command (invalidates all 32 entries in
	// hardware in a single write) — per-entry CTL0=0 wasn't resetting
	// the chip's internal "destination MAC -> CAM index" cache.  Safe
	// here because the chip is fully initialized (firmware loaded,
	// MAC/PHY up); doing this at _InitHardware time appeared to wedge
	// the MAC scheduler.
	if (fRegisterIO != NULL) {
		(void)fRegisterIO->Write8(kRegSecCfg, 0);
		(void)fRegisterIO->Write32(kRegCamCmd,
			kCamCmdPolling | kCamCmdClear);
		// Wait briefly for the clear to commit before issuing more
		// CAM ops.  No need to poll explicitly — subsequent writes
		// will happen well after the chip's internal latency.
		snooze(500);
	}
	fCcmpEnabled = false;

	// If we got stuck mid-handshake from a prior attempt that the AP
	// ignored, allow a fresh start — we have no auth-timeout timer yet.
	if (fJoinState == kJoinAuthenticating
		|| fJoinState == kJoinAssociating) {
		dprintf(RTL8814AU_DRIVER_NAME ": _DoJoin: clearing stuck state %d\n",
			(int)fJoinState);
		fJoinState = kJoinIdle;
	}

	// If caller didn't supply a real BSSID (zeros, broadcast, or never
	// set), look it up by SSID from the recent scan results.
	uint8 zeros[6] = { 0, 0, 0, 0, 0, 0 };
	uint8 broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	uint8 resolved[6];
	uint8 apChannel = 0;
	// Always look up the matching BSS so we can park on its channel.
	// After a scan the chip is on whatever channel the scan ended on,
	// which is rarely the AP's — so without this auth-req goes out on
	// the wrong channel and the AP never hears us.
	if (fWiFiManager != NULL) {
		const BssEntry* match = fWiFiManager->FindBssBySsid(ssid, ssidLen);
		if (match != NULL)
			apChannel = match->channel;
	}
	if (memcmp(bssid, zeros, 6) == 0 || memcmp(bssid, broadcast, 6) == 0
		|| (bssid[0] == 0xCC && bssid[1] == 0xCC)) {
		if (fWiFiManager == NULL) {
			dprintf(RTL8814AU_DRIVER_NAME ": _DoJoin: no BSSID, no manager\n");
			return B_BAD_VALUE;
		}
		const BssEntry* match = fWiFiManager->FindBssBySsid(ssid, ssidLen);
		if (match == NULL) {
			dprintf(RTL8814AU_DRIVER_NAME ": _DoJoin: no BSS matching "
				"'%s' in scan list — run a scan first\n", ssid);
			return B_NAME_NOT_FOUND;
		}
		memcpy(resolved, match->bssid, 6);
		apChannel = match->channel;
		bssid = resolved;
	}

	dprintf(RTL8814AU_DRIVER_NAME ": _DoJoin '%s' -> "
		"%02x:%02x:%02x:%02x:%02x:%02x ch=%u\n", ssid,
		bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
		(unsigned)apChannel);

	// Park the chip on the AP's channel before any TX.  Bandwidth left
	// at 20 MHz — narrowest, always supported, and sufficient for mgmt.
	if (apChannel != 0 && fPhyConfig != NULL) {
		status_t chStatus = fPhyConfig->SetChannel(apChannel,
			kBandwidth20MHz);
		if (chStatus != B_OK) {
			dprintf(RTL8814AU_DRIVER_NAME ": _DoJoin: SetChannel(%u) "
				"failed: %s\n", (unsigned)apChannel, strerror(chStatus));
		}
	}

	// Tell the chip we're operating as a STA in an infrastructure BSS.
	// Without this the MAC won't auto-ACK frames addressed to us, so
	// the AP times us out after sending assoc-resp without an ACK and
	// silently drops us from its client table — even though we logged
	// the assoc-resp and locally believe we're associated.
	uint8 msrBefore = fRegisterIO->Read8(kRegMSR);
	fRegisterIO->Write8(kRegMSR, kMSR_Infra);
	uint8 msrAfter = fRegisterIO->Read8(kRegMSR);
	dprintf(RTL8814AU_DRIVER_NAME ": MSR before=0x%02x after=0x%02x"
		" (want 0x%02x)\n", msrBefore, msrAfter, kMSR_Infra);

	// Set chip BSSID register so MAC frame filter accepts AP traffic
	for (uint32 i = 0; i < 6; i++)
		fRegisterIO->Write8(kRegBSSID + i, bssid[i]);
	memcpy(fJoinBssid, bssid, 6);
	strlcpy(fJoinSsid, ssid, sizeof(fJoinSsid));
	fJoinSsidLength = ssidLen;
	fJoinSeqCounter = 0;
	fJoinState = kJoinAuthenticating;

	return _SendAuthRequest();
}


/*! Build and transmit an open-system authentication request frame
    (subtype 11, auth_alg=0, auth_seq=1, status=0).
*/
status_t
RTL8814AUDevice::_SendAuthRequest()
{
	uint8 frame[30];
	frame[0] = 0xB0;	// FC byte 0: type=mgmt(0), subtype=Auth(11)
	frame[1] = 0x00;	// FC byte 1: no flags
	frame[2] = 0x00; frame[3] = 0x3A;	// duration 314us
	memcpy(frame + 4, fJoinBssid, 6);	// addr1 = DA = BSSID
	memcpy(frame + 10, fMacAddress, 6);	// addr2 = SA = us
	memcpy(frame + 16, fJoinBssid, 6);	// addr3 = BSSID
	uint16 seq = (++fJoinSeqCounter) << 4;
	frame[22] = seq & 0xFF;
	frame[23] = (seq >> 8) & 0xFF;
	frame[24] = 0x00; frame[25] = 0x00;	// auth alg = 0 (open)
	frame[26] = 0x01; frame[27] = 0x00;	// auth seq = 1
	frame[28] = 0x00; frame[29] = 0x00;	// status = 0 (reserved in req)

	dprintf(RTL8814AU_DRIVER_NAME ": TX auth request seq=1\n");
	return fTxPath->Transmit(frame, sizeof(frame), kTxQueueMGT,
		0, 0, kSecurityNone, false);
}


/*! Build and transmit an association request frame (subtype 0).
    Body: capability info, listen interval, SSID IE, supported rates IE,
    and (when joining a WPA2 network) the RSN IE wpa_supplicant deposited
    via IOC_APPIE.  Capability's Privacy bit is set when fPrivacy != 0.
*/
status_t
RTL8814AUDevice::_SendAssocRequest()
{
	uint8 frame[256];
	uint32 i = 0;

	frame[i++] = 0x00;	// FC byte 0: type=mgmt(0), subtype=AssocReq(0)
	frame[i++] = 0x00;
	frame[i++] = 0x00; frame[i++] = 0x3A;	// duration
	memcpy(frame + i, fJoinBssid, 6); i += 6;	// DA = BSSID
	memcpy(frame + i, fMacAddress, 6); i += 6;	// SA = us
	memcpy(frame + i, fJoinBssid, 6); i += 6;	// BSSID
	uint16 seq = (++fJoinSeqCounter) << 4;
	frame[i++] = seq & 0xFF;
	frame[i++] = (seq >> 8) & 0xFF;

	// Capability info: ESS bit always.  Privacy bit set on WPA2 so the
	// AP knows we want encrypted frames.
	uint16 capInfo = 0x0001;
	if (fPrivacy != 0)
		capInfo |= 0x0010;
	frame[i++] = capInfo & 0xFF;
	frame[i++] = (capInfo >> 8) & 0xFF;

	frame[i++] = 0x14; frame[i++] = 0x00;	// listen interval = 20

	// SSID IE
	frame[i++] = 0;	// IE id
	frame[i++] = (uint8)fJoinSsidLength;
	memcpy(frame + i, fJoinSsid, fJoinSsidLength);
	i += fJoinSsidLength;

	// Supported Rates IE — 1, 2, 5.5, 11, 6, 9, 12, 18 Mbps
	frame[i++] = 1;	// IE id
	frame[i++] = 8;	// length
	frame[i++] = 0x82; frame[i++] = 0x84;	// 1, 2 (basic)
	frame[i++] = 0x8B; frame[i++] = 0x96;	// 5.5, 11 (basic)
	frame[i++] = 0x0C; frame[i++] = 0x12;	// 6, 9
	frame[i++] = 0x18; frame[i++] = 0x24;	// 12, 18

	// Extended Supported Rates IE — 24, 36, 48, 54 Mbps
	frame[i++] = 50;	// IE id
	frame[i++] = 4;
	frame[i++] = 0x30; frame[i++] = 0x48;
	frame[i++] = 0x60; frame[i++] = 0x6C;

	// RSN IE (WPA2) — verbatim from IOC_APPIE.  fWpaIe already includes
	// the IE header (id 0x30 + length), so just memcpy.
	if (fWpaIeLength > 0 && i + fWpaIeLength <= sizeof(frame)) {
		memcpy(frame + i, fWpaIe, fWpaIeLength);
		i += fWpaIeLength;
		dprintf(RTL8814AU_DRIVER_NAME ": TX assoc request (WPA2) "
			"len=%u, with %u-byte RSN IE\n",
			(unsigned)i, (unsigned)fWpaIeLength);
	} else {
		dprintf(RTL8814AU_DRIVER_NAME ": TX assoc request (open) "
			"len=%u\n", (unsigned)i);
	}

	return fTxPath->Transmit(frame, i, kTxQueueMGT, 0, 0,
		kSecurityNone, false);
}


/*! Handle an authentication response (subtype 11) from the AP. */
void
RTL8814AUDevice::_HandleAuthResponse(const uint8* frame, uint32 length)
{
	if (length < 30 || fJoinState != kJoinAuthenticating)
		return;

	// addr2 (SA) at offset 10 must match our target BSSID
	if (memcmp(frame + 10, fJoinBssid, 6) != 0)
		return;

	uint16 authAlg = frame[24] | (frame[25] << 8);
	uint16 authSeq = frame[26] | (frame[27] << 8);
	uint16 statusCode = frame[28] | (frame[29] << 8);

	dprintf(RTL8814AU_DRIVER_NAME ": RX auth response alg=%u seq=%u "
		"status=%u\n", (unsigned)authAlg, (unsigned)authSeq,
		(unsigned)statusCode);

	if (authAlg != 0 || authSeq != 2 || statusCode != 0) {
		dprintf(RTL8814AU_DRIVER_NAME ": auth failed, returning to idle\n");
		fJoinState = kJoinIdle;
		return;
	}

	// Auth OK — send assoc-req
	fJoinState = kJoinAssociating;
	status_t status = _SendAssocRequest();
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": assoc-req TX failed: %s\n",
			strerror(status));
		fJoinState = kJoinIdle;
	}
}


/*! Handle an association response (subtype 1) from the AP. */
void
RTL8814AUDevice::_HandleAssocResponse(const uint8* frame, uint32 length)
{
	if (length < 30 || fJoinState != kJoinAssociating)
		return;
	if (memcmp(frame + 10, fJoinBssid, 6) != 0)
		return;

	uint16 capability = frame[24] | (frame[25] << 8);
	uint16 statusCode = frame[26] | (frame[27] << 8);
	uint16 aid = (frame[28] | (frame[29] << 8)) & 0x3FFF;

	dprintf(RTL8814AU_DRIVER_NAME ": RX assoc response cap=0x%04x "
		"status=%u aid=%u\n", (unsigned)capability,
		(unsigned)statusCode, (unsigned)aid);

	if (statusCode != 0) {
		dprintf(RTL8814AU_DRIVER_NAME ": assoc rejected, returning to idle\n");
		fJoinState = kJoinIdle;
		return;
	}

	// Associated!  Notify the WiFi manager so it can send the
	// MediaStatusReport H2C and set the connection state for upper
	// layers to consume via GetLinkState.
	fJoinState = kJoinConnected;
	dprintf(RTL8814AU_DRIVER_NAME ": ASSOCIATED to '%s' AID=%u\n",
		fJoinSsid, (unsigned)aid);
	// Flip the manager state to connected so userland (ETHER_GET_LINK_
	// STATE, GetLinkState) sees the link as up.  Skip the
	// MediaStatusReport H2C — it deadlocks when issued from this RX
	// bulk-callback context, so it'll have to move onto a worker
	// thread later.  The MAC association itself already happened on
	// the air; this just makes the network stack believe it.
	if (fWiFiManager != NULL)
		fWiFiManager->MarkConnected(fJoinBssid, fJoinSsid);
	// Wake net_server so it re-polls ETHER_GET_LINK_STATE immediately
	// instead of waiting for its next periodic check.
	if (fLinkStateSem >= 0)
		release_sem_etc(fLinkStateSem, 1, B_DO_NOT_RESCHEDULE);

	// Publish B_NETWORK_WLAN_JOINED so wpa_supplicant's
	// driver_haiku_events BLooper sees it and dispatches EVENT_ASSOC
	// into driver_bsd's state machine.  Without this wpa_supplicant
	// stays in CONNECTING — it never moves into the EAPOL-handshake
	// state, never processes incoming EAPOL frames, and times out
	// into IOC_MLME DEAUTH after a few seconds.  fLinkStateSem above
	// only wakes net_server's link-state poller; it does not reach
	// wpa_supplicant.
	if (gNotificationModule != NULL && !fRemoved) {
		char ifPath[64];
		snprintf(ifPath, sizeof(ifPath), "%s/%" B_PRIu32,
			RTL8814AU_DEVICE_PATH_BASE, fSlotIndex);

		char messageBuffer[512];
		KMessage message;
		message.SetTo(messageBuffer, sizeof(messageBuffer),
			B_NETWORK_MONITOR);
		message.AddInt32("opcode", B_NETWORK_WLAN_JOINED);
		message.AddString("interface", ifPath);

		gNotificationModule->send_notification(&message);
		dprintf(RTL8814AU_DRIVER_NAME ": fired B_NETWORK_WLAN_JOINED "
			"for %s\n", ifPath);
	}
	// Wake the post-assoc worker so it issues the firmware H2C setup
	// (RA_INFO + MEDIA_STATUS_RPT) from a context that can safely
	// block on USB control transfers.
	if (fPostAssocSem >= 0)
		release_sem_etc(fPostAssocSem, 1, B_DO_NOT_RESCHEDULE);
}


/*! Post-associate worker thread.  Fires once per assoc-resp via
    fPostAssocSem.  Runs the H2C sequence the firmware needs in order
    to actually TX our data frames on-air:

      1. RA_INFO (H2C 0x40):  rate-adaptation table for MACID 1 —
         without this, the chip's MAC scheduler has no rate set for
         our STA and silently discards every queued data frame.
      2. MEDIA_STATUS_RPT (H2C 0x01): tells the firmware MACID 1 is
         a connected peer so it manages keepalives / power-save.

    These can't be issued from _HandleAssocResponse directly because
    that runs on the USB bulk-callback thread; submitting another
    USB control transfer from there deadlocks the stack.
*/
int32
RTL8814AUDevice::_PostAssocThreadEntry(void* arg)
{
	RTL8814AUDevice* device = static_cast<RTL8814AUDevice*>(arg);
	device->_PostAssocLoop();
	return 0;
}


// ---------------------------------------------------------------------------
// EAPOL 4-way handshake worker.
//
// Drains fEapolInbox.  Frames are deposited there by _RxFrameReceived
// when an incoming frame's ethertype is 0x888E.  We can't process EAPOL
// inline in the RX callback (it needs USB control transfers to install
// keys to the chip's CAM, which would deadlock the bulk-IN context),
// hence the dedicated thread.
//
// The state machine itself is a placeholder this commit — it parses
// what it sees, but doesn't TX M2 yet.  The crypto plumbing (PRF-384
// for PTK derivation, HMAC-SHA1 for MIC, AES key wrap for M3 GTK
// extraction) is all in WPA2Crypto.cpp and ready to wire up next.
// ---------------------------------------------------------------------------
int32
RTL8814AUDevice::_Eapol4WayThreadEntry(void* arg)
{
	RTL8814AUDevice* device = static_cast<RTL8814AUDevice*>(arg);
	device->_Eapol4WayLoop();
	return 0;
}


void
RTL8814AUDevice::_Eapol4WayLoop()
{
	while (!fEapolStop) {
		status_t status = acquire_sem(fEapolReady);
		if (status != B_OK || fEapolStop)
			break;
		if (fRemoved)
			continue;

		// Drain whatever's pending under fLock.  Single-slot inbox so
		// at most one frame per wakeup; release_sem in _RxFrameReceived
		// signals one wakeup per arrival.
		uint8 payload[300];
		uint32 length;
		uint8 senderMac[6];
		bool drained;
		{
			MutexLocker locker(fLock);
			drained = fEapolPending;
			if (drained) {
				memcpy(payload, fEapolInbox.payload, fEapolInbox.length);
				memcpy(senderMac, fEapolInbox.senderMac, 6);
				length = fEapolInbox.length;
				fEapolPending = false;
			}
		}
		if (drained)
			_HandleEapolFrame(payload, length, senderMac);
	}
}


// EAPOL frame parser + state machine driver.  Called from the EAPOL
// worker thread under no locks (state mutated only from this thread,
// inbox already copied out).
//
// EAPOL frame format per IEEE 802.1X-2010 §11.3:
//   uint8  version     (typically 2 = 802.1X-2004)
//   uint8  type        (3 = Key)
//   uint16 length      (BE, body length excluding this 4-byte header)
//   <body of `length` bytes>
//
// EAPOL-Key body for descriptor type 2 (RSN, used by WPA2):
//   uint8  desc_type    (0x02)
//   uint16 key_info     (BE) - bit-mask: pairwise/install/ack/MIC/secure/...
//   uint16 key_length   (BE)
//   uint8  key_replay[8]
//   uint8  key_nonce[32]
//   uint8  key_iv[16]
//   uint8  key_rsc[8]
//   uint8  reserved[8]
//   uint8  key_mic[16]
//   uint16 key_data_len (BE)
//   uint8  key_data[key_data_len]   - holds the RSN IE in M2 / M3, GTK in M3
void
RTL8814AUDevice::_HandleEapolFrame(const uint8* payload, uint32 length,
	const uint8 senderMac[6])
{
	if (length < 99) {
		// Smallest EAPOL-Key with empty key_data is 4 + 95 = 99 bytes.
		dprintf(RTL8814AU_DRIVER_NAME ": EAPOL too short: %u bytes\n",
			(unsigned)length);
		return;
	}

	uint8 version = payload[0];
	uint8 packetType = payload[1];
	uint16 bodyLen = ((uint16)payload[2] << 8) | payload[3];
	if (packetType != 3) {
		dprintf(RTL8814AU_DRIVER_NAME ": EAPOL ignored: not a Key frame "
			"(type=%u)\n", packetType);
		return;
	}
	if (length < 4u + bodyLen) {
		dprintf(RTL8814AU_DRIVER_NAME ": EAPOL truncated: header says %u "
			"bytes, frame is %u\n", (unsigned)bodyLen, (unsigned)length);
		return;
	}

	// Walk the EAPOL-Key body.
	const uint8* body = payload + 4;
	uint8 descType = body[0];
	uint16 keyInfo = ((uint16)body[1] << 8) | body[2];
	uint16 keyLen = ((uint16)body[3] << 8) | body[4];
	const uint8* nonce = body + 13;
	const uint8* keyMic = body + 77;
	uint16 keyDataLen = ((uint16)body[93] << 8) | body[94];

	bool pairwise = (keyInfo & 0x0008) != 0;	// bit 3
	bool install  = (keyInfo & 0x0040) != 0;	// bit 6
	bool keyAck   = (keyInfo & 0x0080) != 0;	// bit 7
	bool keyMicSet = (keyInfo & 0x0100) != 0;	// bit 8
	bool secure   = (keyInfo & 0x0200) != 0;	// bit 9
	uint8 keyDescVer = keyInfo & 0x0007;

	// Identify message: M1 = Ack only; M3 = Ack + MIC + Install.
	const char* whichMsg = "?";
	if (pairwise) {
		if (keyAck && !keyMicSet && !install) whichMsg = "M1";
		else if (!keyAck && keyMicSet && !install) whichMsg = "M2 (from us?!)";
		else if (keyAck && keyMicSet && install) whichMsg = "M3";
		else if (!keyAck && keyMicSet && !install && secure) whichMsg = "M4 (from us?!)";
	}

	// Single-line classifier — verbose hex dumps removed to keep syslog
	// volume low (BFS acquire_vnode panics correlate with log floods).
	dprintf(RTL8814AU_DRIVER_NAME ": EAPOL %s keyInfo=0x%04x keyDataLen=%u\n",
		whichMsg, keyInfo, keyDataLen);
	(void)descType;
	(void)keyMic;
	(void)nonce;
	(void)version;
	(void)keyDescVer;
	(void)keyLen;
	(void)secure;

	// M1 -> M2: capture ANonce, derive PTK, build M2 with HMAC-SHA1 MIC.
	if (pairwise && keyAck && !keyMicSet && !install) {
		memcpy(fAnonce, nonce, 32);

		// Echo back the M1 replay counter in M2.  Stored big-endian
		// directly in body[5..12]; we keep it in host order for clarity
		// and re-encode when building M2.
		fM1ReplayCounter = 0;
		for (uint32 i = 0; i < 8; i++)
			fM1ReplayCounter = (fM1ReplayCounter << 8) | body[5 + i];

		if (!fPmkValid) {
			dprintf(RTL8814AU_DRIVER_NAME ": M1 received but no PMK — "
				"call IOC_HAIKU_JOIN with rtl_haiku_join_psk first\n");
			return;
		}

		// SNonce — kernel-side random.  Mix system_time, real-time
		// clock, our MAC, and a counter through SHA-1 twice.  Not a
		// CSPRNG but adequate freshness: the AP only needs SNonce
		// to be unique per handshake, not unpredictable.
		_GenerateSnonce(fSnonce);

		// PTK = PRF-384(PMK, "Pairwise key expansion",
		//               min(AA,SPA) || max(AA,SPA)
		//            || min(ANonce,SNonce) || max(ANonce,SNonce))
		uint8 ptkData[76];
		const uint8* aa = senderMac;	// AP's MAC
		const uint8* spa = fMacAddress;	// our MAC
		bool aaIsMin = memcmp(aa, spa, 6) < 0;
		memcpy(ptkData, aaIsMin ? aa : spa, 6);
		memcpy(ptkData + 6, aaIsMin ? spa : aa, 6);
		bool aNonceIsMin = memcmp(fAnonce, fSnonce, 32) < 0;
		memcpy(ptkData + 12, aNonceIsMin ? fAnonce : fSnonce, 32);
		memcpy(ptkData + 44, aNonceIsMin ? fSnonce : fAnonce, 32);
		wpa2_crypto::prf_384(fPmk, 32,
			"Pairwise key expansion", 22,
			ptkData, 76, fPtk);
		fPtkValid = true;

		dprintf(RTL8814AU_DRIVER_NAME ": M1 processed, PTK derived\n");

		// Build EAPOL-Key M2 frame in a stack buffer.  EAPOL header
		// (4 bytes) + EAPOL-Key fixed (95 bytes) + RSN IE (key_data_len)
		// = 99 + fWpaIeLength.
		uint8 m2[256];
		uint32 keyDataLength = fWpaIeLength;
		if (keyDataLength == 0 || keyDataLength > 80) {
			dprintf(RTL8814AU_DRIVER_NAME ": M1 processed but no RSN "
				"IE stored — skipping M2 build\n");
			return;
		}
		uint32 m2Len = 99 + keyDataLength;
		memset(m2, 0, m2Len);

		// EAPOL header
		m2[0] = 0x02;	// version 2
		m2[1] = 0x03;	// type Key
		uint32 bodyLen2 = m2Len - 4;
		m2[2] = (uint8)(bodyLen2 >> 8);
		m2[3] = (uint8)bodyLen2;

		// EAPOL-Key body
		m2[4] = 0x02;	// descriptor type RSN
		// Key Info: descVer=2 (HMAC-SHA1, AES key wrap),
		//           pair=1 (bit 3), MIC=1 (bit 8)
		uint16 m2KeyInfo = 0x010A;
		m2[5] = (uint8)(m2KeyInfo >> 8);
		m2[6] = (uint8)m2KeyInfo;
		// Key Length = 0 in M2 (per IEEE 802.11i, M2 leaves key length
		// at zero; AP sets the negotiated value in M3).
		m2[7] = 0; m2[8] = 0;
		// Replay counter — echo M1's value verbatim.
		for (uint32 i = 0; i < 8; i++)
			m2[9 + i] = (uint8)(fM1ReplayCounter >> (56 - i * 8));
		// Key Nonce = our SNonce
		memcpy(m2 + 17, fSnonce, 32);
		// IV (16), RSC (8), Reserved (8) — already zero from memset.
		// Key MIC (16) — computed below; leave as zeros for now.
		// Key Data Length (big-endian)
		m2[97] = (uint8)(keyDataLength >> 8);
		m2[98] = (uint8)keyDataLength;
		// Key Data — our RSN IE verbatim
		memcpy(m2 + 99, fWpaIe, fWpaIeLength);

		// Compute MIC = HMAC-SHA1(KCK, entire M2 with MIC field zero)[0..15]
		uint8 mic[20];
		const uint8* kck = fPtk;	// PTK[0..15]
		wpa2_crypto::hmac_sha1(kck, 16, m2, m2Len, mic);
		memcpy(m2 + 81, mic, 16);

		dprintf(RTL8814AU_DRIVER_NAME ": built M2 (%u bytes)\n",
			(unsigned)m2Len);

		// Wrap M2 in an 802.11 data frame + LLC/SNAP and TX it.  Layout
		// matches the eth -> 802.11 conversion in Write() except the
		// ethertype is fixed (EAPOL = 0x888E) and we splice the EAPOL
		// bytes directly without going through the data ring.  Protected
		// bit stays clear because the chip has no keys yet.
		status_t txStatus = _TxEapolDataFrame(senderMac, m2, m2Len);
		if (txStatus != B_OK) {
			dprintf(RTL8814AU_DRIVER_NAME ": M2 TX failed: %s\n",
				strerror(txStatus));
			// Stay in WaitM1 so an AP retry of M1 gets us another shot.
			return;
		}

		fEapolState = kEapolWaitM3;
		dprintf(RTL8814AU_DRIVER_NAME ": EAPOL state -> WaitM3 "
			"(M2 sent on-air)\n");
		return;
	}

	// M3: pair=1, install=1, ack=1, mic=1.  AP delivers the GTK
	// (encrypted with KEK via RFC 3394 AES key wrap) in key_data.
	if (pairwise && keyAck && keyMicSet && install
			&& fEapolState == kEapolWaitM3 && fPtkValid) {
		// Validate the MIC over the EAPOL frame (exactly 4 + bodyLen
		// bytes — anything trailing is FCS/pad and the AP excluded it
		// from its MIC computation).  M3's MIC is
		// HMAC-SHA1(KCK, frame_with_mic_zeroed)[0..15].
		uint32 hashLen = 4u + bodyLen;
		uint8 m3copy[300];
		if (hashLen > sizeof(m3copy)) {
			dprintf(RTL8814AU_DRIVER_NAME ": M3 too large (%u bytes), "
				"dropping\n", (unsigned)hashLen);
			return;
		}
		memcpy(m3copy, payload, hashLen);
		uint8 receivedMic[16];
		memcpy(receivedMic, m3copy + 4 + 77, 16);	// 4 EAPOL hdr + 77 to MIC
		memset(m3copy + 4 + 77, 0, 16);
		uint8 computedMic[20];
		const uint8* kck = fPtk;	// PTK[0..15]
		wpa2_crypto::hmac_sha1(kck, 16, m3copy, hashLen, computedMic);
		if (memcmp(receivedMic, computedMic, 16) != 0) {
			dprintf(RTL8814AU_DRIVER_NAME ": M3 MIC mismatch "
				"(received[0..3]=%02x%02x%02x%02x "
				"computed[0..3]=%02x%02x%02x%02x) — dropping\n",
				receivedMic[0], receivedMic[1], receivedMic[2], receivedMic[3],
				computedMic[0], computedMic[1], computedMic[2], computedMic[3]);
			return;
		}

		// Decrypt key_data with KEK (PTK[16..31]) using RFC 3394 AES
		// key unwrap.  M3 with CCMP typically delivers 56 bytes of
		// wrapped key data = 8-byte IV + 48 bytes wrapped (= 6 blocks)
		// which unwraps to 48 bytes of plaintext containing the GTK
		// KDE (and possibly an RSN IE echo).
		uint8 plaintext[256];
		if (keyDataLen < 16 || (keyDataLen % 8) != 0
				|| keyDataLen - 8 > sizeof(plaintext)) {
			dprintf(RTL8814AU_DRIVER_NAME ": M3 keyDataLen=%u not a "
				"valid AES-key-wrap size — dropping\n",
				(unsigned)keyDataLen);
			return;
		}
		const uint8* keyData = body + 95;	// 95 = bytes before key_data
		const uint8* kek = fPtk + 16;
		bool unwrapOk = wpa2_crypto::aes_unwrap(kek, keyData, keyDataLen,
			plaintext);
		uint32 ptLen = keyDataLen - 8;
		if (!unwrapOk) {
			dprintf(RTL8814AU_DRIVER_NAME ": M3 aes_unwrap failed "
				"(IV mismatch) — dropping\n");
			return;
		}
		dprintf(RTL8814AU_DRIVER_NAME ": M3 MIC OK, %u plaintext bytes\n",
			(unsigned)ptLen);

		// Walk the unwrapped plaintext for KDEs.  Each KDE:
		//   uint8 type    = 0xDD (vendor-specific)
		//   uint8 length  = N (excludes type+length)
		//   uint8 oui[3]  = 00 0F AC
		//   uint8 kdeType = 1 = GTK, 2 = MAC, ...
		//   uint8 data[N - 4]
		// Some elements are RSN IEs (type 0x30) instead.  Skip those.
		uint32 off = 0;
		uint8 gtkKey[32];
		uint32 gtkLen = 0;
		uint8 gtkKeyId = 0;
		while (off + 2 <= ptLen) {
			uint8 elementId = plaintext[off];
			uint8 elementLen = plaintext[off + 1];
			if (off + 2 + elementLen > ptLen)
				break;
			if (elementId == 0xDD && elementLen >= 6
					&& plaintext[off + 2] == 0x00
					&& plaintext[off + 3] == 0x0F
					&& plaintext[off + 4] == 0xAC
					&& plaintext[off + 5] == 0x01) {
				// GTK KDE: bytes after OUI+kdeType:
				//   [2] KeyID flags (lower 2 bits = key id)
				//   [N-2] GTK
				gtkKeyId = plaintext[off + 6] & 0x03;
				gtkLen = elementLen - 6;
				if (gtkLen > sizeof(gtkKey))
					gtkLen = sizeof(gtkKey);
				memcpy(gtkKey, &plaintext[off + 8], gtkLen);
			}
			off += 2 + elementLen;
		}
		if (gtkLen == 0) {
			dprintf(RTL8814AU_DRIVER_NAME ": M3 unwrap OK but no GTK KDE "
				"found in plaintext — dropping\n");
			return;
		}
		dprintf(RTL8814AU_DRIVER_NAME ": M3 GTK extracted: keyId=%u len=%u\n",
			gtkKeyId, (unsigned)gtkLen);

		// Build M4: same skeleton as M2 but with no key_data, secure
		// bit set in Key Info, replay counter echoed from M3.
		uint8 m4[99];
		memset(m4, 0, sizeof(m4));
		m4[0] = 0x02;	// EAPOL ver
		m4[1] = 0x03;	// EAPOL type Key
		uint16 m4Body = sizeof(m4) - 4;
		m4[2] = (uint8)(m4Body >> 8);
		m4[3] = (uint8)m4Body;
		m4[4] = 0x02;	// desc type RSN
		// Key Info: descVer=2, pair=1, MIC=1, secure=1.  Bits set:
		// 1 (descVer 2), 3 (pair), 8 (MIC), 9 (secure) -> 0x030A
		uint16 m4KeyInfo = 0x030A;
		m4[5] = (uint8)(m4KeyInfo >> 8);
		m4[6] = (uint8)m4KeyInfo;
		m4[7] = 0; m4[8] = 0;	// key_length = 0 in M4
		// Replay counter — echo M3's value verbatim.
		memcpy(m4 + 9, body + 5, 8);
		// nonce, iv, rsc, reserved already zero from memset.
		// MIC zeroed; computed below.
		// key_data_len = 0 already.

		// Compute MIC = HMAC-SHA1(KCK, M4)[0..15].
		uint8 m4Mic[20];
		wpa2_crypto::hmac_sha1(kck, 16, m4, sizeof(m4), m4Mic);
		memcpy(m4 + 81, m4Mic, 16);

		dprintf(RTL8814AU_DRIVER_NAME ": built M4 (%u bytes)\n",
			(unsigned)sizeof(m4));

		status_t txStatus = _TxEapolDataFrame(senderMac, m4, sizeof(m4));
		if (txStatus != B_OK) {
			dprintf(RTL8814AU_DRIVER_NAME ": M4 TX failed: %s\n",
				strerror(txStatus));
			return;
		}

		// Program TK (PTK[32..47]) and GTK into the chip's security
		// CAM, then flip kRegSecCfg to enable hardware AES-CCMP.  After
		// this lands the AP's encrypted unicast + broadcast traffic
		// decrypts on-chip and our TX path encrypts outbound data.
		const uint8* tk = fPtk + 32;
		status_t keyStatus = _InstallSessionKeys(tk, gtkKey, gtkLen,
			gtkKeyId, senderMac);
		if (keyStatus != B_OK) {
			dprintf(RTL8814AU_DRIVER_NAME ": chip CAM install failed: "
				"%s\n", strerror(keyStatus));
			// Don't bail — the handshake is logically complete; data
			// will just fail at the link layer and DHCP will time out.
		}

		if (keyStatus == B_OK) {
			fCcmpEnabled = true;
			// Stash GTK for SW decrypt fallback (chip is unable to HW
			// decrypt these frames — see _RxFrameReceived).
			memcpy(fGtk, gtkKey, gtkLen > 16 ? 16 : gtkLen);
			fGtkKeyId = gtkKeyId;
			fGtkValid = true;
			// Reset TX PN counters with fresh keys.  IEEE 802.11i
			// disallows reusing a PN with a given key, so we start
			// at 1 every time keys are reinstalled.
			fTxPnPairwise = 1;
			fTxPnGroup = 1;
		}

		fEapolState = kEapolDone;
		dprintf(RTL8814AU_DRIVER_NAME ": EAPOL state -> Done "
			"(M4 sent; CCMP %s)\n",
			fCcmpEnabled ? "enabled" : "DISABLED");
		return;
	}
}


// Low-level CAM dword write.  Stages \a data in CAMWRITE then issues a
// CAMCMD write at \a addr (= entry_index << 3 | word_index).  Polls
// CAMCMD until the chip clears the POLLING bit, indicating the entry
// was committed to the security engine.  Timeout is generous — typical
// USB control transfer round trip is ~1 ms; CAM commit is microseconds
// chip-side, so 100 polls (~100 ms) is several orders of magnitude.
status_t
RTL8814AUDevice::_CamWrite(uint8 addr, uint32 data)
{
	if (fRegisterIO == NULL)
		return B_NO_INIT;

	status_t status = fRegisterIO->Write32(kRegCamWrite, data);
	if (status != B_OK)
		return status;
	status = fRegisterIO->Write32(kRegCamCmd,
		kCamCmdPolling | kCamCmdWrite | (uint32)addr);
	if (status != B_OK)
		return status;

	for (uint32 i = 0; i < 100; i++) {
		uint32 cmd = fRegisterIO->Read32(kRegCamCmd);
		if ((cmd & kCamCmdPolling) == 0)
			return B_OK;
		snooze(100);
	}
	dprintf(RTL8814AU_DRIVER_NAME ": _CamWrite addr=0x%02x data=0x%08x "
		"polling never cleared\n", addr, (unsigned)data);
	return B_TIMED_OUT;
}


// Program one CAM entry: 8 dwords laid out as
//   word 0 (CTL0): VALID | (algo<<2) | keyid | (mac[0]<<16) | (mac[1]<<24)
//   word 1 (CTL1): mac[2..5]   little-endian
//   word 2..5    : key bytes [0..15] little-endian, 4 bytes per dword
//   word 6, 7    : reserved (zero)
// Per the morrownr 8814au reference (hal/hal_com.c rtw_sec_write_cam_ent)
// and rtwn (rtl8192c) — same layout across all Realtek MAC variants.
//
// Writes the entry from word 7 down to word 0; CTL0 is written LAST so
// the VALID bit only goes hot once the rest of the entry is staged.
status_t
RTL8814AUDevice::_ProgramCamEntry(uint8 entry, uint8 algo, uint8 keyId,
	bool isGroupKey, const uint8 mac[6], const uint8* key, uint32 keyLen)
{
	if (entry >= 32)
		return B_BAD_VALUE;
	if (keyLen > 16)
		return B_BAD_VALUE;

	uint32 base = (uint32)entry << 3;

	uint8 keyBuf[16];
	memset(keyBuf, 0, sizeof(keyBuf));
	if (key != NULL && keyLen > 0)
		memcpy(keyBuf, key, keyLen);

	// Word 7, 6 — reserved, zero.
	status_t status = _CamWrite(base + 7, 0);
	if (status != B_OK)
		return status;
	status = _CamWrite(base + 6, 0);
	if (status != B_OK)
		return status;

	// Words 5..2 — key bytes, little-endian, four per dword.
	for (int j = 5; j >= 2; j--) {
		uint32 i = (uint32)(j - 2) << 2;
		uint32 word = (uint32)keyBuf[i]
			| ((uint32)keyBuf[i + 1] << 8)
			| ((uint32)keyBuf[i + 2] << 16)
			| ((uint32)keyBuf[i + 3] << 24);
		status = _CamWrite(base + (uint8)j, word);
		if (status != B_OK)
			return status;
	}

	// Word 1 — MAC[2..5].
	uint32 ctl1 = (uint32)mac[2]
		| ((uint32)mac[3] << 8)
		| ((uint32)mac[4] << 16)
		| ((uint32)mac[5] << 24);
	status = _CamWrite(base + 1, ctl1);
	if (status != B_OK)
		return status;

	// Word 0 — CTL0 with VALID set.  This commits the entry.  Group
	// keys also need the BIT(6) "group key" flag — without it the
	// chip's HW decrypt pipeline doesn't engage for incoming
	// CCMP-encrypted broadcast frames (verified empirically against
	// morrownr's setkey_hdl: GTK ctrl word always sets BIT(6)).
	uint16 ctrl = (uint16)kCamValid
		| (uint16)(((uint32)algo & 0x7u) << kCamAlgoShift)
		| (uint16)((uint32)keyId & 0x3u);
	if (isGroupKey)
		ctrl |= (uint16)kCamGroupKey;
	uint32 ctl0 = (uint32)ctrl
		| ((uint32)mac[0] << 16)
		| ((uint32)mac[1] << 24);
	return _CamWrite(base + 0, ctl0);
}


// Install pairwise + group session keys after the 4-way handshake and
// turn on hardware AES-CCMP via kRegSecCfg.
//
// Layout choice (matches rtwn's CRYPTO_FULL convention):
//   - Group keys at CAM index = gtkKeyId (0..3)
//   - Pairwise key at CAM index 4
// SECCFG is configured with TX/RX UC + BC default-key lookup and TX
// encrypt + RX decrypt enables — i.e., trust the CAM's MAC-indexed
// key for both unicast and broadcast paths.
status_t
RTL8814AUDevice::_InstallSessionKeys(const uint8* tk, const uint8* gtk,
	uint32 gtkLen, uint8 gtkKeyId, const uint8 apMac[6])
{
	if (fRegisterIO == NULL)
		return B_NO_INIT;
	if (tk == NULL || gtk == NULL || gtkLen != 16)
		return B_BAD_VALUE;
	if (gtkKeyId > 3)
		return B_BAD_VALUE;

	// CAM[gtkKeyId] = group key, MAC = 00:00:00:00:00:00.  The chip
	// uses the broadcast-default-key path (kSecCfgRxBcKeyDef) to find
	// this entry by KEYID rather than by source MAC.  The group-key
	// CTL0 flag (BIT 6) tells the chip's decrypt engine that this is
	// a group key — required for HW decrypt to engage on inbound
	// CCMP-encrypted broadcast frames.
	const uint8 zeroMac[6] = { 0, 0, 0, 0, 0, 0 };
	status_t status = _ProgramCamEntry(gtkKeyId, kCamAlgoAES, gtkKeyId,
		true, zeroMac, gtk, gtkLen);
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": GTK CAM[%u] write failed: %s\n",
			gtkKeyId, strerror(status));
		return status;
	}

	// CAM[4] = pairwise key, MAC = AP BSSID.  The chip indexes the
	// unicast key by peer MAC for both TX (we send to AP) and RX
	// (frames from AP).  No group flag.
	status = _ProgramCamEntry(4, kCamAlgoAES, 0, false, apMac, tk, 16);
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": TK CAM[4] write failed: %s\n",
			strerror(status));
		return status;
	}

	// Enable hardware crypto.  Match the morrownr 8814au reference's
	// WPA2 setup (hal/hal_com.c HW_VAR_SEC_CFG + HW_VAR_SEC_DK_CFG):
	// TX/RX enable + broadcast default-key + CHK_KEYID for proper
	// RX broadcast keyid lookup.  Direct 16-bit write (CHK_KEYID is
	// bit 8); don't OR with prior state — TxUseDK/RxUseDK from a
	// stale prior session would force unicast through default-key
	// lookup and break per-MAC pairwise matching.
	// SW CCMP for both directions: chip's HW crypto engine doesn't
	// engage for our setup (RxDecEnable silently dropped Protected
	// frames; HW encrypt produced ciphertext the AP couldn't decrypt
	// either, hence DHCP_DISCOVER going unanswered).  Disable all
	// chip-side crypto by writing SECCFG=0 and do AES-CCMP entirely
	// in software via wpa2_crypto::ccmp_encrypt / ccmp_decrypt.  CAM
	// entries are still written above (harmless when SECCFG=0) so
	// that if the chip's HW crypto suddenly cooperates in some
	// future test we won't have to re-derive them.
	uint16 oldCfg = fRegisterIO->Read16(kRegSecCfg);
	uint16 newCfg = 0;
	status = fRegisterIO->Write16(kRegSecCfg, newCfg);
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": kRegSecCfg write failed: %s\n",
			strerror(status));
		return status;
	}

	dprintf(RTL8814AU_DRIVER_NAME ": CAM programmed: TK@4 (AES, peer "
		"%02x:%02x:%02x:%02x:%02x:%02x) GTK@%u (AES, keyid=%u) "
		"SecCfg 0x%04x->0x%04x\n",
		apMac[0], apMac[1], apMac[2], apMac[3], apMac[4], apMac[5],
		gtkKeyId, gtkKeyId, oldCfg, newCfg);

	// Accept ALL multicast addresses by setting REG_MAR (multicast
	// hash filter) to all-ones.  Without this, the chip drops
	// multicast frames at MAC level before decryption — even with
	// kRCR_AM set, the per-address hash filter still gates them.
	// morrownr's _InitWMACSetting_8814A does this; we missed it.
	fRegisterIO->Write32(kRegMAR + 0, 0xFFFFFFFFu);
	fRegisterIO->Write32(kRegMAR + 4, 0xFFFFFFFFu);

	// Match morrownr's 8814au post-assoc RCR (usb_halinit.c:444):
	//   APM | AM | AB | CBSSID_DATA | CBSSID_BCN | APP_ICV |
	//   AMF | HTC_LOC_CTRL | APP_MIC | APP_PHYST_RXFF | APPFCS
	// The APP_ICV / APP_MIC / APP_PHYST_RXFF bits aren't actually
	// "append after decrypt" choices — on this chip family they're
	// required for the HW crypto engine's RX pipeline to stage
	// frames through the decrypt unit at all.  Without them, CCMP
	// frames bypass decrypt and arrive at the host still encrypted
	// (verified empirically: SECCFG=0x01EC + valid CAM but RX
	// frames at offset 24 still showed CCMP IV header).
	uint32 oldRcr = fRegisterIO->Read32(kRegRCR);
	uint32 newRcr = oldRcr
		| kRCR_CBSSID_DATA | kRCR_CBSSID_BCN
		| kRCR_APP_ICV | kRCR_APP_MIC
		| kRCR_APP_PHYST_RXFF | kRCR_HTC_LOC_CTRL;
	fRegisterIO->Write32(kRegRCR, newRcr);
	dprintf(RTL8814AU_DRIVER_NAME ": RCR 0x%08x->0x%08x "
		"(CBSSID + APP_ICV/MIC/PHYST + HTC_LOC for HW decrypt)\n",
		(unsigned)oldRcr, (unsigned)newRcr);

	// Read back CAM[1] CTL0 and CAM[4] CTL0 to confirm what the chip
	// actually stored — useful since wrong byte-order or wrong VALID
	// bit would cause silent decrypt failures.  CAM read is the same
	// CMD/RWD pattern as write but without the WRITE bit.
	for (uint8 entry = 1; entry <= 4; entry += 3) {
		uint32 addr = (uint32)entry << 3;
		fRegisterIO->Write32(kRegCamCmd, kCamCmdPolling | addr);
		for (uint32 i = 0; i < 100; i++) {
			if ((fRegisterIO->Read32(kRegCamCmd) & kCamCmdPolling) == 0)
				break;
			snooze(50);
		}
		uint32 ctl0 = fRegisterIO->Read32(kRegCamRead);
		fRegisterIO->Write32(kRegCamCmd, kCamCmdPolling | (addr + 1));
		for (uint32 i = 0; i < 100; i++) {
			if ((fRegisterIO->Read32(kRegCamCmd) & kCamCmdPolling) == 0)
				break;
			snooze(50);
		}
		uint32 ctl1 = fRegisterIO->Read32(kRegCamRead);
		dprintf(RTL8814AU_DRIVER_NAME ": CAM[%u] readback: "
			"CTL0=0x%08x CTL1=0x%08x (expect VALID=0x8000 in CTL0 low)\n",
			entry, (unsigned)ctl0, (unsigned)ctl1);
	}

	return B_OK;
}


// Send EAPOL bytes as an 802.11 data frame (ToDS=1, Protected=0).
// Used by the in-driver 4-way handshake to put M2/M4 on the air.
//
// Wire layout:
//   [24] 802.11 header (FC=0x08 0x01, addrs ours/BSSID/AP)
//   [8]  LLC/SNAP (AA AA 03 00 00 00 88 8E)
//   [N]  EAPOL bytes verbatim
status_t
RTL8814AUDevice::_TxEapolDataFrame(const uint8* apMac,
	const uint8* eapol, uint32 eapolLen)
{
	if (fTxPath == NULL)
		return B_NO_INIT;
	if (eapolLen > 256)
		return B_BUFFER_OVERFLOW;

	uint8 wireFrame[24 + 8 + 256];
	uint32 i = 0;

	// 802.11 data-frame header
	wireFrame[i++] = 0x08;	// FC[0]: type=Data (2), subtype=0
	wireFrame[i++] = 0x01;	// FC[1]: ToDS=1, FromDS=0, Protected=0
	wireFrame[i++] = 0x00;	// Duration low
	wireFrame[i++] = 0x3A;	// Duration high (314us)
	memcpy(wireFrame + i, fJoinBssid, 6); i += 6;	// Addr1 = BSSID (RA)
	memcpy(wireFrame + i, fMacAddress, 6); i += 6;	// Addr2 = our MAC (TA)
	memcpy(wireFrame + i, apMac, 6); i += 6;		// Addr3 = AP MAC (DA)
	wireFrame[i++] = 0;		// SeqCtrl low (HW fills via HWSEQ_EN)
	wireFrame[i++] = 0;		// SeqCtrl high

	// LLC/SNAP encapsulation, ethertype = EAPOL (0x888E)
	wireFrame[i++] = 0xAA;
	wireFrame[i++] = 0xAA;
	wireFrame[i++] = 0x03;
	wireFrame[i++] = 0x00;
	wireFrame[i++] = 0x00;
	wireFrame[i++] = 0x00;
	wireFrame[i++] = 0x88;
	wireFrame[i++] = 0x8E;

	// EAPOL payload
	memcpy(wireFrame + i, eapol, eapolLen);
	i += eapolLen;

	return fTxPath->Transmit(wireFrame, i, kTxQueueBE,
		kRateCCK1, 0, kSecurityNone, false);
}


// SNonce generator — not a CSPRNG.  Mixes system_time, real-time
// clock, our MAC, and a per-call counter through two SHA-1 rounds
// to fill 32 bytes.  Adequate freshness: the AP only needs SNonce
// to be unique per handshake, not strongly unpredictable.
void
RTL8814AUDevice::_GenerateSnonce(uint8 nonce[32])
{
	static uint32 sCounter = 0;
	sCounter++;

	uint8 mix[64];
	memset(mix, 0, sizeof(mix));
	bigtime_t t1 = system_time();
	bigtime_t t2 = real_time_clock_usecs();
	memcpy(mix +  0, &t1, sizeof(t1));
	memcpy(mix +  8, &t2, sizeof(t2));
	memcpy(mix + 16, fMacAddress, 6);
	memcpy(mix + 22, &sCounter, 4);
	memset(mix + 26, 0x5a, 38);

	uint8 hash1[20];
	wpa2_crypto::sha1(mix, 64, hash1);
	mix[0] ^= 0xFF;
	uint8 hash2[20];
	wpa2_crypto::sha1(mix, 64, hash2);
	memcpy(nonce + 0, hash1, 20);
	memcpy(nonce + 20, hash2, 12);
}


void
RTL8814AUDevice::_PostAssocLoop()
{
	while (!fPostAssocStop) {
		status_t status = acquire_sem(fPostAssocSem);
		if (status != B_OK || fPostAssocStop)
			break;
		if (fRemoved)
			continue;
		status_t setupStatus = _DoPostAssocSetup();
		dprintf(RTL8814AU_DRIVER_NAME ": post-assoc setup: %s\n",
			strerror(setupStatus));
	}
}


/*! Issue the firmware-side connection setup.  Called from the worker
    thread, so synchronous USB control transfers are safe here.

    Sequence (matching the rtw88 reference driver):
      1. Write the AP's BSSID into kRegBSSID so the chip's MAC frame
         filter recognises traffic from / to this BSS.  (We already
         wrote it in _DoJoin, but Associate() in WiFiManager does it
         too — keep consistent.)
      2. Send RA_INFO H2C — MACID 1, rate_id 8 (OFDM), BW 20 MHz,
         rate mask covering OFDM 6–54 Mbps.  The rate adaptation
         engine uses this to pick a rate per data-frame TX.
      3. Send MEDIA_STATUS_RPT H2C — connect=1, MACID=1.
*/
status_t
RTL8814AUDevice::_DoPostAssocSetup()
{
	if (fWiFiManager == NULL || fRegisterIO == NULL)
		return B_NO_INIT;

	// Snapshot the BSSID under lock; the worker may run after a leave/
	// rejoin race so don't trust live fJoinBssid without serialising.
	uint8 bssid[6];
	{
		MutexLocker locker(fLock);
		memcpy(bssid, fJoinBssid, 6);
	}

	dprintf(RTL8814AU_DRIVER_NAME ": post-assoc: BSSID %02x:%02x:%02x:"
		"%02x:%02x:%02x\n", bssid[0], bssid[1], bssid[2],
		bssid[3], bssid[4], bssid[5]);

	// 1. Re-write BSSID register — idempotent with _DoJoin's write.
	// Use 32-bit + 16-bit writes (not byte-by-byte) so the chip sees
	// the full BSSID atomically; some chips latch only on aligned
	// 32-bit writes and ignore byte writes to MAC-like registers.
	uint32 bssidLo = (uint32)bssid[0]
		| ((uint32)bssid[1] << 8)
		| ((uint32)bssid[2] << 16)
		| ((uint32)bssid[3] << 24);
	uint16 bssidHi = (uint16)bssid[4] | ((uint16)bssid[5] << 8);
	fRegisterIO->Write32(kRegBSSID, bssidLo);
	fRegisterIO->Write16(kRegBSSID + 4, bssidHi);

	// 2. RA_INFO (H2C 0x40 = kH2C_MacIDCfg).  Field layout (from rtw88
	//    rtw_fw_send_ra_info, payload bytes after the 1-byte cmd ID):
	//      byte 0:        MACID
	//      byte 1 [0:4]:  rate_id (5 bits)
	//      byte 2 [0:1]:  BW_MODE (00=20MHz, 01=40MHz, 10=80MHz)
	//      bytes 3..6:    rate mask (32 bits, low first) — we send the
	//                     low 24 bits since H2C payload is 6 bytes.
	const uint8 kMacID = 0;
	const uint8 kRateId = 8;	// OFDM-only rate group
	uint32 rateMask = 0x000FF0u;	// OFDM 6–54 Mbps (bits 4-11)
	uint8 raPayload[6] = { 0 };
	raPayload[0] = kMacID;
	raPayload[1] = kRateId & 0x1F;
	raPayload[2] = 0;			// BW_MODE = 20 MHz
	raPayload[3] = (uint8)(rateMask & 0xFF);
	raPayload[4] = (uint8)((rateMask >> 8) & 0xFF);
	raPayload[5] = (uint8)((rateMask >> 16) & 0xFF);
	status_t raStatus = fWiFiManager->SendH2C(kH2C_MacIDCfg,
		raPayload, sizeof(raPayload));
	dprintf(RTL8814AU_DRIVER_NAME ": post-assoc RA_INFO: %s\n",
		strerror(raStatus));

	// 3. MEDIA_STATUS_RPT (H2C 0x01) — connect=1, MACID=1.
	uint8 msPayload[2] = { 1, kMacID };
	status_t msStatus = fWiFiManager->SendH2C(kH2C_MediaStatusRpt,
		msPayload, sizeof(msPayload));
	dprintf(RTL8814AU_DRIVER_NAME ": post-assoc MEDIA_STATUS_RPT: %s\n",
		strerror(msStatus));

	return (raStatus == B_OK && msStatus == B_OK) ? B_OK : B_ERROR;
}




