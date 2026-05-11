/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * WiFiManagement.h — WiFi MLME management for the RTL8814AU.
 *
 * The RTL8814AU firmware (running on the Lexra 3081 MCU) handles the
 * low-level 802.11 MLME state machine. The host driver communicates
 * with it through two mechanisms:
 *
 *   H2C (Host-to-Card) mailbox:
 *     4 rotating 7-byte mailboxes at registers HMEBOX_0..3. The driver
 *     writes commands (scan, associate, set power mode, etc.) and the
 *     firmware processes them asynchronously.
 *
 *   C2H (Card-to-Host) events:
 *     Delivered via the USB interrupt IN endpoint. The firmware sends
 *     events (scan complete, connection status change, TX report, etc.)
 *     which the driver parses and acts on.
 *
 * This module provides a high-level WiFi management interface:
 *   - Trigger and retrieve scan results
 *   - Associate with an access point
 *   - Disconnect from the current AP
 *   - Report link state changes
 *   - Configure power management
 *
 * The BSS (Basic Service Set) list is maintained as a simple array of
 * recently seen access points, updated during scanning.
 *
 * Reference: rtl8814a_cmd.c, core/rtw_mlme.c in ulli-kroll/rtl8814au.
 */
#ifndef RTL8814AU_WIFI_MANAGEMENT_H
#define RTL8814AU_WIFI_MANAGEMENT_H


#include <USB3.h>
#include <lock.h>

#include "RTL8814AU.h"


class RTL8814AURegisterIO;


// Maximum number of BSS entries (access points) we track from scans
static const uint32 kMaxBssEntries = 64;

// Maximum SSID length (IEEE 802.11 standard)
static const uint32 kMaxSSIDLength = 32;

// Maximum length of a raw scan result IE (Information Element) block
static const uint32 kMaxIELength = 768;


// ---------------------------------------------------------------------------
// BSS entry — one scanned access point
// ---------------------------------------------------------------------------

struct BssEntry {
	uint8		bssid[6];				// AP MAC address
	char		ssid[kMaxSSIDLength + 1]; // Network name (NUL-terminated)
	uint8		ssidLength;				// Actual SSID length (0-32)
	uint8		channel;				// Operating channel
	int8		rssi;					// Signal strength in dBm
	uint16		beaconInterval;			// Beacon interval in TUs
	uint16		capability;				// Capability info field
	SecurityType security;				// Best security type detected

	// Raw Information Elements from the beacon/probe response.
	// Needed by userland to extract full AP capabilities.
	uint8		ieData[kMaxIELength];
	uint32		ieLength;

	// Timestamp of when this entry was last updated (system_time)
	bigtime_t	lastSeen;

	bool		valid;					// true if this entry is populated
};


// ---------------------------------------------------------------------------
// Connection state
// ---------------------------------------------------------------------------

enum WiFiState {
	kWiFiStateDisconnected	= 0,
	kWiFiStateScanning		= 1,
	kWiFiStateAuthenticating = 2,
	kWiFiStateAssociating	= 3,
	kWiFiStateConnected		= 4,
};


// ---------------------------------------------------------------------------
// Link state info — passed to the network stack via ioctl
// ---------------------------------------------------------------------------

struct WiFiLinkState {
	WiFiState	state;
	uint8		bssid[6];			// Connected AP's BSSID (if connected)
	char		ssid[kMaxSSIDLength + 1];
	uint8		channel;
	int8		rssi;				// Current signal strength
	uint8		dataRate;			// Current TX data rate index
	ChannelBandwidth bandwidth;		// Current operating bandwidth
};


// ---------------------------------------------------------------------------
// WiFi manager class
// ---------------------------------------------------------------------------

class RTL8814AUWiFiManager {
public:
								RTL8814AUWiFiManager(
									RTL8814AURegisterIO* registerIO,
									usb_module_info* usbModule,
									usb_device usbDevice,
									usb_pipe interruptIn);
								~RTL8814AUWiFiManager();

	status_t					InitCheck() const { return fInitStatus; }

	// Start the C2H event listener (interrupt IN polling).
	// Must be called after hardware init is complete.
	status_t					Start();
	void						Stop();

	// --- Scanning ---

	// Trigger a scan on the specified channels. If channelList is NULL,
	// scans all supported channels. Returns immediately — results arrive
	// asynchronously via C2H events.
	status_t					StartScan(const uint8* channelList = NULL,
									uint32 channelCount = 0);

	// Block until the current scan completes (or timeout fires).
	// Multiple threads may wait simultaneously; all are woken when
	// the scan-done C2H event arrives.
	status_t					WaitForScanComplete(bigtime_t timeout);

	// Check if a scan is currently in progress.
	bool						IsScanning() const
									{ return fState == kWiFiStateScanning; }

	// Get the current BSS list. Caller must hold the lock (or call
	// from a context where the list won't be modified).
	const BssEntry*				BssList() const { return fBssList; }
	uint32						BssCount() const { return fBssCount; }

	// Copy scan results into a caller-provided buffer. Thread-safe.
	// Returns the number of entries copied.
	const BssEntry*				FindBssBySsid(const char* ssid,
									uint32 ssidLen) const;

	// Flip the manager into the connected state from data we already
	// have on hand.  No USB writes, safe to call from the RX bulk
	// callback context.  Used by the in-driver MLME after a successful
	// on-air assoc-resp until we have a worker-thread H2C path.
	void						MarkConnected(const uint8* bssid,
									const char* ssid);

	// Public thin wrapper around the H2C send path.  Used by the
	// device's post-assoc worker thread to fire RA_INFO + MEDIA_
	// STATUS_RPT after on-air assoc.  Don't call from contexts that
	// hold the USB bulk endpoint mutex (RX/TX callbacks).
	status_t					SendH2C(uint8 commandID,
									const uint8* payload,
									uint32 payloadLength);
	uint32						GetScanResults(BssEntry* results,
									uint32 maxEntries);

	// --- Association ---

	// Associate with the AP identified by BSSID. The SSID and security
	// parameters must match what was seen in the scan.
	status_t					Associate(const uint8* bssid,
									const char* ssid,
									const uint8* passphrase,
									uint32 passphraseLength);

	// Disconnect from the current AP.
	status_t					Disconnect();

	// Get current connection state and link info.
	WiFiState					State() const { return fState; }
	status_t					GetLinkState(WiFiLinkState* state);

	// --- BSS list update ---

	// Update or create a BSS entry from a parsed beacon/probe response.
	// Called by the device's RX frame handler.
	void						UpdateBssEntry(const uint8* bssid,
									const char* ssid, uint8 ssidLength,
									uint8 channel, uint16 beaconInterval,
									uint16 capability,
									SecurityType security, int8 rssi,
									const uint8* ieData, uint32 ieLength);

	// --- C2H event processing ---

	// Called by the RX path or interrupt handler when a C2H event
	// arrives. Dispatches to the appropriate handler.
	void						HandleC2HEvent(uint8 eventID,
									const uint8* payload,
									uint32 payloadLength);

private:
	// H2C mailbox interface — send a command to the firmware
	status_t					_SendH2CCommand(uint8 commandID,
									const uint8* payload,
									uint32 payloadLength);

	// H2C command builders for specific operations
	status_t					_SendScanCommand(
									const uint8* channelList,
									uint32 channelCount);
	status_t					_SendMediaStatusCommand(bool connected);
	status_t					_SendSetPowerModeCommand(uint8 mode);
	status_t					_SendRateAdaptiveCommand(uint8 macID,
									uint8 rateID);

	// C2H event handlers
	void						_HandleScanComplete(const uint8* payload,
									uint32 length);
	void						_HandleConnectionStatus(const uint8* payload,
									uint32 length);
	void						_HandleRateAdaptive(const uint8* payload,
									uint32 length);
	void						_HandleTxReport(const uint8* payload,
									uint32 length);

	// BSS list management
	BssEntry*					_FindOrCreateBssEntry(const uint8* bssid);
	void						_PurgeStaleBssEntries();

	// Interrupt IN handling for C2H events
	status_t					_SubmitInterruptTransfer();
	static void					_InterruptCallback(void* cookie,
									status_t status,
									void* data,
									size_t actualLength);

	RTL8814AURegisterIO*		fRegisterIO;
	usb_module_info*			fUSBModule;
	usb_device					fUSBDevice;
	usb_pipe					fInterruptIn;

	// H2C mailbox write index — rotates 0..3
	uint8						fH2CMailboxIndex;

	// Interrupt IN receive buffer
	uint8						fInterruptBuffer[kUsbInterruptBufferSize];

	// BSS list from scanning
	BssEntry					fBssList[kMaxBssEntries];
	uint32						fBssCount;

	// Current connection state
	WiFiState					fState;
	uint8						fConnectedBssid[6];
	char						fConnectedSsid[kMaxSSIDLength + 1];
	uint8						fCurrentChannel;
	int8						fCurrentRssi;
	uint8						fCurrentDataRate;

	// Synchronization
	mutex						fLock;
	sem_id						fScanCompleteSem;

	bool						fRunning;
	status_t					fInitStatus;
};


#endif	// RTL8814AU_WIFI_MANAGEMENT_H
