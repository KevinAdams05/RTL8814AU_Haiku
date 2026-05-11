/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * WiFiManagement.cpp — WiFi management implementation for RTL8814AU.
 *
 * This module orchestrates the WiFi connection lifecycle using the
 * firmware's built-in MLME (MAC Layer Management Entity). Instead of
 * implementing 802.11 authentication and association frame exchanges
 * in the driver, we send high-level commands to the Lexra 3081 firmware
 * via the H2C (Host-to-Card) mailbox and receive results via C2H
 * (Card-to-Host) events.
 *
 * H2C mailbox protocol:
 *   - 4 rotating mailboxes (HMEBOX_0..3), each 7 bytes usable
 *   - Byte 0: command ID
 *   - Bytes 1-3: standard payload (written to kRegHMEBox[n])
 *   - Bytes 4-6: extended payload (written to kRegHMEBoxExt[n])
 *   - After writing, the driver bumps fH2CMailboxIndex modulo 4
 *
 * C2H event protocol:
 *   - Delivered via USB interrupt IN endpoint
 *   - Byte 0: event ID
 *   - Byte 1: payload length
 *   - Bytes 2+: event-specific payload
 *
 * Scan flow:
 *   1. Driver sends kH2C_ScanEn with channel list
 *   2. Firmware scans channels, sending probe requests
 *   3. Firmware sends kC2H_ScanComplete with BSS count
 *   4. Received beacons/probe responses during scan are delivered via
 *      the normal RX path — the driver parses them into BssEntry records
 *
 * Association flow:
 *   1. Driver sends kH2C_MediaStatusRpt to indicate we want to connect
 *   2. Driver configures security keys (if WPA2)
 *   3. Firmware handles the 802.11 auth/assoc exchange
 *   4. Firmware sends kC2H_ConnectionStatus on success or failure
 *
 * Reference: rtl8814a_cmd.c, core/rtw_mlme.c, core/rtw_cmd.c in
 * ulli-kroll/rtl8814au.
 */

#include "WiFiManagement.h"

#include <new>
#include <string.h>

#include <KernelExport.h>
#include <OS.h>
#include <util/AutoLock.h>

#include "RegisterIO.h"


// Stale BSS entries are purged after this interval (30 seconds)
static const bigtime_t kBssPurgeInterval = 30000000;


// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------


RTL8814AUWiFiManager::RTL8814AUWiFiManager(
	RTL8814AURegisterIO* registerIO, usb_module_info* usbModule,
	usb_device usbDevice, usb_pipe interruptIn)
	:
	fRegisterIO(registerIO),
	fUSBModule(usbModule),
	fUSBDevice(usbDevice),
	fInterruptIn(interruptIn),
	fH2CMailboxIndex(0),
	fBssCount(0),
	fState(kWiFiStateDisconnected),
	fCurrentChannel(0),
	fCurrentRssi(0),
	fCurrentDataRate(0),
	fRunning(false),
	fInitStatus(B_NO_INIT)
{
	memset(fBssList, 0, sizeof(fBssList));
	memset(fConnectedBssid, 0, sizeof(fConnectedBssid));
	memset(fConnectedSsid, 0, sizeof(fConnectedSsid));
	memset(fInterruptBuffer, 0, sizeof(fInterruptBuffer));
	mutex_init(&fLock, "rtl8814au:wifi");

	fScanCompleteSem = create_sem(0, "rtl8814au:scan_done");
	if (fScanCompleteSem < 0) {
		fInitStatus = fScanCompleteSem;
		return;
	}

	fInitStatus = B_OK;
	dprintf(RTL8814AU_DRIVER_NAME ": WiFi manager initialized\n");
}


RTL8814AUWiFiManager::~RTL8814AUWiFiManager()
{
	Stop();

	if (fScanCompleteSem >= 0)
		delete_sem(fScanCompleteSem);

	mutex_destroy(&fLock);
}


// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------


/*! Start the C2H event listener by submitting the first interrupt IN
    transfer. The completion callback re-submits automatically.
*/
status_t
RTL8814AUWiFiManager::Start()
{
	if (fRunning)
		return B_OK;

	dprintf(RTL8814AU_DRIVER_NAME ": starting WiFi management\n");
	fRunning = true;

	// Submit the interrupt IN transfer to start receiving C2H events
	if (fInterruptIn != 0) {
		status_t status = _SubmitInterruptTransfer();
		if (status != B_OK) {
			dprintf(RTL8814AU_DRIVER_NAME ": interrupt IN submit "
				"failed: %s\n", strerror(status));
			fRunning = false;
			return status;
		}
	}

	return B_OK;
}


void
RTL8814AUWiFiManager::Stop()
{
	if (!fRunning)
		return;

	dprintf(RTL8814AU_DRIVER_NAME ": stopping WiFi management\n");
	fRunning = false;

	// Cancel pending interrupt IN transfers
	if (fInterruptIn != 0)
		fUSBModule->cancel_queued_transfers(fInterruptIn);
}


// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------


/*! Trigger a WiFi scan. If channelList is NULL, scans all supported
    channels (2.4 GHz + 5 GHz).

    Returns B_OK if the scan command was sent successfully. The scan runs
    asynchronously — listen for kC2H_ScanComplete or call IsScanning().
*/
status_t
RTL8814AUWiFiManager::StartScan(const uint8* channelList,
	uint32 channelCount)
{
	MutexLocker locker(fLock);

	if (fState == kWiFiStateScanning) {
		dprintf(RTL8814AU_DRIVER_NAME ": scan already in progress\n");
		return B_BUSY;
	}

	dprintf(RTL8814AU_DRIVER_NAME ": starting scan\n");

	// Purge old entries before starting a new scan
	_PurgeStaleBssEntries();

	fState = kWiFiStateScanning;
	locker.Unlock();

	// If no channel list provided, scan all channels
	if (channelList == NULL || channelCount == 0) {
		// Build a combined channel list from 2.4 GHz and 5 GHz
		uint8 allChannels[sizeof(kChannelList2G) + sizeof(kChannelList5G)];
		uint32 totalChannels = 0;

		memcpy(allChannels, kChannelList2G, sizeof(kChannelList2G));
		totalChannels += sizeof(kChannelList2G);

		memcpy(allChannels + totalChannels, kChannelList5G,
			sizeof(kChannelList5G));
		totalChannels += sizeof(kChannelList5G);

		return _SendScanCommand(allChannels, totalChannels);
	}

	return _SendScanCommand(channelList, channelCount);
}


/*! Public wrapper around the internal H2C mailbox send path so the
	driver's post-assoc worker thread can fire RA_INFO and
	MEDIA_STATUS_RPT commands without poking _SendH2CCommand directly.
*/
status_t
RTL8814AUWiFiManager::SendH2C(uint8 commandID, const uint8* payload,
	uint32 payloadLength)
{
	return _SendH2CCommand(commandID, payload, payloadLength);
}


/*! Mark the manager as connected without any USB I/O.  Called from
	the driver's in-driver MLME state machine after the on-air assoc-
	resp is received, so userland sees the link as up via
	ETHER_GET_LINK_STATE.  Until we move the post-assoc H2C onto a
	worker thread, we can't do the full Associate() path here.
*/
void
RTL8814AUWiFiManager::MarkConnected(const uint8* bssid, const char* ssid)
{
	MutexLocker locker(fLock);
	if (bssid != NULL)
		memcpy(fConnectedBssid, bssid, 6);
	if (ssid != NULL)
		strlcpy(fConnectedSsid, ssid, sizeof(fConnectedSsid));
	else
		fConnectedSsid[0] = '\0';
	fState = kWiFiStateConnected;
}


/*! Look up a scan-discovered BSS by SSID.  Returns NULL if not found.
	Caller must hold or accept that fBssList may change after this call.
*/
const BssEntry*
RTL8814AUWiFiManager::FindBssBySsid(const char* ssid, uint32 ssidLen) const
{
	if (ssid == NULL || ssidLen == 0)
		return NULL;
	for (uint32 i = 0; i < kMaxBssEntries; i++) {
		const BssEntry& e = fBssList[i];
		if (!e.valid)
			continue;
		if (e.ssidLength != ssidLen)
			continue;
		if (memcmp(e.ssid, ssid, ssidLen) == 0)
			return &e;
	}
	return NULL;
}


/*! Copy scan results to a caller-provided buffer. Thread-safe.

    \param results     Output buffer for BSS entries
    \param maxEntries  Maximum number of entries to copy
    \return Number of entries actually copied.
*/
uint32
RTL8814AUWiFiManager::GetScanResults(BssEntry* results, uint32 maxEntries)
{
	MutexLocker locker(fLock);

	uint32 count = 0;
	for (uint32 i = 0; i < kMaxBssEntries && count < maxEntries; i++) {
		if (fBssList[i].valid) {
			memcpy(&results[count], &fBssList[i], sizeof(BssEntry));
			count++;
		}
	}

	return count;
}


/*! Update or create a BSS entry from a parsed beacon or probe response.
    Called from the device's RX frame handler.
*/
void
RTL8814AUWiFiManager::UpdateBssEntry(const uint8* bssid, const char* ssid,
	uint8 ssidLength, uint8 channel, uint16 beaconInterval,
	uint16 capability, SecurityType security, int8 rssi,
	const uint8* ieData, uint32 ieLength)
{
	MutexLocker locker(fLock);

	BssEntry* entry = _FindOrCreateBssEntry(bssid);
	if (entry == NULL)
		return;

	// Update the entry with the latest data from this beacon/probe
	if (ssid != NULL && ssidLength > 0) {
		uint32 copyLen = ssidLength;
		if (copyLen > kMaxSSIDLength)
			copyLen = kMaxSSIDLength;
		memcpy(entry->ssid, ssid, copyLen);
		entry->ssid[copyLen] = '\0';
		entry->ssidLength = (uint8)copyLen;
	}

	entry->channel = channel;
	entry->rssi = rssi;
	entry->beaconInterval = beaconInterval;
	entry->capability = capability;
	entry->security = security;
	entry->lastSeen = system_time();

	// Store the raw IEs for userland to parse
	if (ieData != NULL && ieLength > 0) {
		uint32 ieCopyLen = ieLength;
		if (ieCopyLen > kMaxIELength)
			ieCopyLen = kMaxIELength;
		memcpy(entry->ieData, ieData, ieCopyLen);
		entry->ieLength = ieCopyLen;
	}
}


// ---------------------------------------------------------------------------
// Association
// ---------------------------------------------------------------------------


/*! Associate with an access point identified by BSSID.

    The SSID and passphrase are used to configure security. For open
    networks, pass NULL/0 for the passphrase.

    This function sends the H2C commands and returns. The actual
    association completes asynchronously — monitor the connection state
    via GetLinkState() or wait for a kC2H_ConnectionStatus event.
*/
status_t
RTL8814AUWiFiManager::Associate(const uint8* bssid, const char* ssid,
	const uint8* passphrase, uint32 passphraseLength)
{
	MutexLocker locker(fLock);

	if (fState == kWiFiStateConnected) {
		dprintf(RTL8814AU_DRIVER_NAME ": already connected, "
			"disconnecting first\n");
		locker.Unlock();
		Disconnect();
		locker.Lock();
	}

	dprintf(RTL8814AU_DRIVER_NAME ": associating with "
		"%02x:%02x:%02x:%02x:%02x:%02x (%s)\n",
		bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
		ssid != NULL ? ssid : "<hidden>");

	// Write the target BSSID to the hardware register so the MAC
	// filter knows which AP we're talking to
	for (int32 i = 0; i < 6; i++) {
		status_t status = fRegisterIO->Write8(kRegBSSID + i, bssid[i]);
		if (status != B_OK)
			return status;
	}

	// Store the target for state tracking
	memcpy(fConnectedBssid, bssid, 6);
	if (ssid != NULL)
		strlcpy(fConnectedSsid, ssid, sizeof(fConnectedSsid));
	else
		fConnectedSsid[0] = '\0';

	fState = kWiFiStateAuthenticating;

	// TODO: Configure security keys if WPA2 passphrase is provided.
	// This requires computing the PMK from the passphrase and SSID,
	// then writing it to the security CAM via kRegCamCmd/kRegCamWrite.

	locker.Unlock();

	// Tell the firmware we want to connect
	status_t status = _SendMediaStatusCommand(true);
	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": media status H2C failed: %s\n",
			strerror(status));
		MutexLocker relock(fLock);
		fState = kWiFiStateDisconnected;
		return status;
	}

	return B_OK;
}


/*! Disconnect from the current access point. */
status_t
RTL8814AUWiFiManager::Disconnect()
{
	MutexLocker locker(fLock);

	if (fState == kWiFiStateDisconnected)
		return B_OK;

	dprintf(RTL8814AU_DRIVER_NAME ": disconnecting\n");

	fState = kWiFiStateDisconnected;
	memset(fConnectedBssid, 0, sizeof(fConnectedBssid));
	fConnectedSsid[0] = '\0';
	fCurrentRssi = 0;
	fCurrentDataRate = 0;
	fCurrentChannel = 0;

	locker.Unlock();

	// Tell firmware we've disconnected
	return _SendMediaStatusCommand(false);
}


/*! Fill a WiFiLinkState structure with the current connection info. */
status_t
RTL8814AUWiFiManager::GetLinkState(WiFiLinkState* state)
{
	if (state == NULL)
		return B_BAD_VALUE;

	MutexLocker locker(fLock);

	state->state = fState;
	memcpy(state->bssid, fConnectedBssid, 6);
	strlcpy(state->ssid, fConnectedSsid, sizeof(state->ssid));
	state->channel = fCurrentChannel;
	state->rssi = fCurrentRssi;
	state->dataRate = fCurrentDataRate;
	state->bandwidth = kBandwidth20MHz;	// TODO: track actual BW

	return B_OK;
}


// ---------------------------------------------------------------------------
// C2H event processing
// ---------------------------------------------------------------------------


/*! Dispatch a C2H event to the appropriate handler.

    \param eventID       C2H event identifier
    \param payload       Event payload (after the event ID and length bytes)
    \param payloadLength Number of payload bytes
*/
void
RTL8814AUWiFiManager::HandleC2HEvent(uint8 eventID, const uint8* payload,
	uint32 payloadLength)
{
	switch (eventID) {
		case kC2H_ScanComplete:
			_HandleScanComplete(payload, payloadLength);
			break;

		case kC2H_ConnectionStatus:
			_HandleConnectionStatus(payload, payloadLength);
			break;

		case kC2H_RateAdaptive:
			_HandleRateAdaptive(payload, payloadLength);
			break;

		case kC2H_TxReport:
			_HandleTxReport(payload, payloadLength);
			break;

		case kC2H_Debug:
			// Firmware debug message — log it
			dprintf(RTL8814AU_DRIVER_NAME ": FW debug: ");
			for (uint32 i = 0; i < payloadLength; i++)
				dprintf("%02x ", payload[i]);
			dprintf("\n");
			break;

		default:
			dprintf(RTL8814AU_DRIVER_NAME ": unknown C2H event: 0x%02x\n",
				eventID);
			break;
	}
}


// ---------------------------------------------------------------------------
// H2C mailbox interface
// ---------------------------------------------------------------------------


/*! Send a command to the firmware via the H2C mailbox system.

    The mailbox protocol:
      1. Write extended payload (bytes 4-6) to kRegHMEBoxExt[index]
      2. Write standard payload (bytes 0-3, where byte 0 = commandID)
         to kRegHMEBox[index]
      3. Advance fH2CMailboxIndex to the next slot

    The firmware reads the standard register first, which triggers
    command processing. The extended register must be written BEFORE
    the standard register.

    \param commandID      H2C command identifier
    \param payload        Command-specific payload (max 6 bytes)
    \param payloadLength  Number of payload bytes (0-6)
    \return B_OK on success.
*/
status_t
RTL8814AUWiFiManager::_SendH2CCommand(uint8 commandID,
	const uint8* payload, uint32 payloadLength)
{
	if (payloadLength > 6) {
		dprintf(RTL8814AU_DRIVER_NAME ": H2C payload too large: "
			"%" B_PRIu32 " (max 6)\n", payloadLength);
		return B_BAD_VALUE;
	}

	MutexLocker locker(fLock);

	uint8 boxIndex = fH2CMailboxIndex;

	// Build the 7-byte H2C command buffer:
	//   Byte 0: command ID
	//   Bytes 1-3: standard payload
	//   Bytes 4-6: extended payload
	uint8 cmd[kH2CCommandSize];
	memset(cmd, 0, sizeof(cmd));
	cmd[0] = commandID;
	if (payload != NULL && payloadLength > 0) {
		uint32 copyLen = payloadLength;
		if (copyLen > 6)
			copyLen = 6;
		memcpy(cmd + 1, payload, copyLen);
	}

	// Write extended payload first (bytes 4-6 → kRegHMEBoxExt)
	uint32 extValue = (uint32)cmd[4]
		| ((uint32)cmd[5] << 8)
		| ((uint32)cmd[6] << 16);
	status_t status = fRegisterIO->Write32(kRegHMEBoxExt[boxIndex],
		extValue);
	if (status != B_OK)
		return status;

	// Write standard payload (bytes 0-3 → kRegHMEBox) — this triggers
	// firmware processing
	uint32 stdValue = (uint32)cmd[0]
		| ((uint32)cmd[1] << 8)
		| ((uint32)cmd[2] << 16)
		| ((uint32)cmd[3] << 24);
	status = fRegisterIO->Write32(kRegHMEBox[boxIndex], stdValue);
	if (status != B_OK)
		return status;

	// Advance to next mailbox slot
	fH2CMailboxIndex = (fH2CMailboxIndex + 1) % kH2CMailboxCount;

	return B_OK;
}


/*! Send a scan command to the firmware.

    H2C format for kH2C_ScanEn:
      Byte 0: 1 = start scan, 0 = abort scan
      Byte 1: number of channels
      Bytes 2+: channel numbers (limited by 6-byte payload)

    For more than 6 channels, the firmware scans all supported channels
    when byte 1 is set to 0 (scan all).
*/
status_t
RTL8814AUWiFiManager::_SendScanCommand(const uint8* channelList,
	uint32 channelCount)
{
	uint8 payload[6];
	memset(payload, 0, sizeof(payload));

	payload[0] = 1;	// Start scan

	// If the channel list fits in the H2C payload, send it directly.
	// Otherwise, tell firmware to scan all channels.
	if (channelCount <= 4) {
		payload[1] = (uint8)channelCount;
		for (uint32 i = 0; i < channelCount; i++)
			payload[2 + i] = channelList[i];
	} else {
		// Too many channels for a single H2C — request full scan
		payload[1] = 0;	// 0 = scan all supported channels
	}

	return _SendH2CCommand(kH2C_ScanEn, payload, 6);
}


/*! Send a media status report to the firmware.

    \param connected  true = connected to AP, false = disconnected
*/
status_t
RTL8814AUWiFiManager::_SendMediaStatusCommand(bool connected)
{
	uint8 payload[2];
	payload[0] = connected ? 1 : 0;	// 1 = connected
	payload[1] = 0;						// MACID 0 (self)

	return _SendH2CCommand(kH2C_MediaStatusRpt, payload, 2);
}


/*! Send a power mode command to the firmware.

    \param mode  0 = active, 1 = low power (LPS), 2 = inactive (IPS)
*/
status_t
RTL8814AUWiFiManager::_SendSetPowerModeCommand(uint8 mode)
{
	uint8 payload[3];
	payload[0] = mode;
	payload[1] = 0;		// Smart PS disabled
	payload[2] = 0;		// Awake interval = 0

	return _SendH2CCommand(kH2C_SetPwrMode, payload, 3);
}


/*! Send a rate adaptation configuration command for a given station.

    \param macID   Station MACID (0 for ourselves)
    \param rateID  Rate table ID
*/
status_t
RTL8814AUWiFiManager::_SendRateAdaptiveCommand(uint8 macID, uint8 rateID)
{
	uint8 payload[4];
	payload[0] = macID;
	payload[1] = rateID;
	payload[2] = 0;		// Short GI disabled initially
	payload[3] = 0;		// Reserved

	return _SendH2CCommand(kH2C_MacIDCfg, payload, 4);
}


// ---------------------------------------------------------------------------
// C2H event handlers
// ---------------------------------------------------------------------------


/*! Handle scan completion event from firmware.

    The firmware has finished iterating the channel list. Any beacons or
    probe responses received during the scan were delivered through the
    normal RX path and should already be in the BSS list.
*/
void
RTL8814AUWiFiManager::_HandleScanComplete(const uint8* payload,
	uint32 length)
{
	dprintf(RTL8814AU_DRIVER_NAME ": scan complete\n");

	MutexLocker locker(fLock);
	if (fState == kWiFiStateScanning)
		fState = kWiFiStateDisconnected;
	locker.Unlock();

	// Signal any thread waiting for scan completion.  Use B_RELEASE_ALL
	// so every blocked waiter wakes — there can be more than one when
	// the SIOCS80211 ioctl path spawns a notifier thread alongside an
	// in-driver caller.
	release_sem_etc(fScanCompleteSem, 1,
		B_DO_NOT_RESCHEDULE | B_RELEASE_ALL);
}


/*! Block until the current scan completes (or timeout elapses).
    \param timeout  Relative timeout in microseconds, or B_INFINITE_TIMEOUT.
    
eturn  B_OK if the scan completed before the timeout, B_TIMED_OUT
             if not, or another error.
*/
status_t
RTL8814AUWiFiManager::WaitForScanComplete(bigtime_t timeout)
{
	return acquire_sem_etc(fScanCompleteSem, 1,
		B_RELATIVE_TIMEOUT | B_CAN_INTERRUPT, timeout);
}


/*! Handle connection status change from firmware.

    Payload format (varies by firmware version):
      Byte 0: status code (0 = connected, 1 = disconnected, 2 = auth fail)
*/
void
RTL8814AUWiFiManager::_HandleConnectionStatus(const uint8* payload,
	uint32 length)
{
	if (length < 1)
		return;

	uint8 statusCode = payload[0];

	MutexLocker locker(fLock);

	switch (statusCode) {
		case 0:
			// Connected successfully
			dprintf(RTL8814AU_DRIVER_NAME ": connected to "
				"%02x:%02x:%02x:%02x:%02x:%02x\n",
				fConnectedBssid[0], fConnectedBssid[1],
				fConnectedBssid[2], fConnectedBssid[3],
				fConnectedBssid[4], fConnectedBssid[5]);
			fState = kWiFiStateConnected;
			break;

		case 1:
			// Disconnected (by AP or timeout)
			dprintf(RTL8814AU_DRIVER_NAME ": disconnected by AP\n");
			fState = kWiFiStateDisconnected;
			memset(fConnectedBssid, 0, sizeof(fConnectedBssid));
			fConnectedSsid[0] = '\0';
			break;

		case 2:
			// Authentication failed
			dprintf(RTL8814AU_DRIVER_NAME ": authentication failed\n");
			fState = kWiFiStateDisconnected;
			break;

		default:
			dprintf(RTL8814AU_DRIVER_NAME ": unknown connection "
				"status: %u\n", statusCode);
			break;
	}
}


/*! Handle rate adaptation feedback from firmware. Updates the current
    TX data rate for link state reporting.
*/
void
RTL8814AUWiFiManager::_HandleRateAdaptive(const uint8* payload,
	uint32 length)
{
	if (length < 2)
		return;

	MutexLocker locker(fLock);
	fCurrentDataRate = payload[0];

	// payload[1] contains the MACID this rate applies to
}


/*! Handle TX report from firmware. Contains per-frame TX status
    (success/retry/fail counts). Used for debugging and statistics.
*/
void
RTL8814AUWiFiManager::_HandleTxReport(const uint8* payload,
	uint32 length)
{
	// TX reports are primarily useful for debugging and advanced
	// rate adaptation tuning. Log at debug level only.
	if (length >= 4) {
		dprintf(RTL8814AU_DRIVER_NAME ": TX report: MACID %u, "
			"ok %u, retry %u, fail %u\n",
			payload[0], payload[1], payload[2], payload[3]);
	}
}


// ---------------------------------------------------------------------------
// BSS list management
// ---------------------------------------------------------------------------


/*! Find an existing BSS entry by BSSID, or allocate a new one.
    Called with fLock held.

    \param bssid  6-byte BSSID to look up
    \return Pointer to the BSS entry, or NULL if the list is full.
*/
BssEntry*
RTL8814AUWiFiManager::_FindOrCreateBssEntry(const uint8* bssid)
{
	// First pass: look for an existing entry with this BSSID
	for (uint32 i = 0; i < kMaxBssEntries; i++) {
		if (fBssList[i].valid
			&& memcmp(fBssList[i].bssid, bssid, 6) == 0) {
			return &fBssList[i];
		}
	}

	// Second pass: find an empty slot
	for (uint32 i = 0; i < kMaxBssEntries; i++) {
		if (!fBssList[i].valid) {
			memset(&fBssList[i], 0, sizeof(BssEntry));
			fBssList[i].valid = true;
			memcpy(fBssList[i].bssid, bssid, 6);
			fBssCount++;
			return &fBssList[i];
		}
	}

	// List is full — replace the weakest (lowest RSSI) entry
	int32 weakestIndex = -1;
	int8 weakestRssi = 0;
	for (uint32 i = 0; i < kMaxBssEntries; i++) {
		if (weakestIndex < 0 || fBssList[i].rssi < weakestRssi) {
			weakestIndex = (int32)i;
			weakestRssi = fBssList[i].rssi;
		}
	}

	if (weakestIndex >= 0) {
		memset(&fBssList[weakestIndex], 0, sizeof(BssEntry));
		fBssList[weakestIndex].valid = true;
		memcpy(fBssList[weakestIndex].bssid, bssid, 6);
		return &fBssList[weakestIndex];
	}

	return NULL;
}


/*! Remove BSS entries that haven't been updated recently.
    Called with fLock held.
*/
void
RTL8814AUWiFiManager::_PurgeStaleBssEntries()
{
	bigtime_t now = system_time();

	for (uint32 i = 0; i < kMaxBssEntries; i++) {
		if (fBssList[i].valid
			&& (now - fBssList[i].lastSeen) > kBssPurgeInterval) {
			fBssList[i].valid = false;
			fBssCount--;
		}
	}
}


// ---------------------------------------------------------------------------
// Interrupt IN handling
// ---------------------------------------------------------------------------


/*! Submit the interrupt IN transfer to receive C2H events from firmware. */
status_t
RTL8814AUWiFiManager::_SubmitInterruptTransfer()
{
	return fUSBModule->queue_interrupt(fInterruptIn, fInterruptBuffer,
		kUsbInterruptBufferSize, _InterruptCallback, this);
}


/*! USB interrupt IN completion callback. Called when the firmware sends
    a C2H event via the interrupt endpoint.
*/
void
RTL8814AUWiFiManager::_InterruptCallback(void* cookie, status_t status,
	void* data, size_t actualLength)
{
	RTL8814AUWiFiManager* manager
		= static_cast<RTL8814AUWiFiManager*>(cookie);
	if (manager == NULL)
		return;

	// Don't process if shutting down or device removed
	if (status == B_CANCELED || status == B_DEV_NOT_READY)
		return;

	if (status != B_OK) {
		dprintf(RTL8814AU_DRIVER_NAME ": interrupt IN error: %s\n",
			strerror(status));
	}

	// Parse the C2H event if we received data.
	// Format: byte 0 = event ID, byte 1 = payload length, bytes 2+ = payload
	if (status == B_OK && actualLength >= 2) {
		const uint8* eventData = static_cast<const uint8*>(data);
		uint8 eventID = eventData[0];
		uint8 payloadLen = eventData[1];

		if (payloadLen + 2 <= (uint32)actualLength) {
			manager->HandleC2HEvent(eventID, eventData + 2, payloadLen);
		}
	}

	// Re-submit the interrupt transfer to keep listening
	if (manager->fRunning) {
		status_t resubmit = manager->_SubmitInterruptTransfer();
		if (resubmit != B_OK) {
			dprintf(RTL8814AU_DRIVER_NAME ": interrupt IN re-submit "
				"failed: %s\n", strerror(resubmit));
		}
	}
}
