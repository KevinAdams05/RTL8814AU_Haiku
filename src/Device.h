/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Device.h — RTL8814AUDevice class declaration.
 *
 * This is the central device class that owns all subsystem modules and
 * coordinates the hardware lifecycle:
 *   1. USB endpoint discovery and configuration
 *   2. Hardware power-on sequence
 *   3. EFUSE reading (MAC address, calibration data)
 *   4. Firmware loading (Lexra 3081 DMEM + IRAM)
 *   5. PHY/RF initialization (4-path configuration)
 *   6. TX/RX data path management
 *   7. WiFi management (scan, associate, authenticate)
 *
 * The device is created in DeviceAdded() and destroyed either in
 * DeviceRemoved() (if not open) or in the last Free() call.
 */
#ifndef RTL8814AU_DEVICE_H
#define RTL8814AU_DEVICE_H


#include <Drivers.h>
#include <USB3.h>
#include <lock.h>

#include "RTL8814AU.h"
#include "RegisterIO.h"
#include "Firmware.h"
#include "EfuseReader.h"
#include "PhyConfig.h"
#include "TxPath.h"
#include "RxPath.h"
#include "WiFiManagement.h"


class RTL8814AUDevice {
public:
								RTL8814AUDevice(usb_device device,
									uint32 slotIndex,
									const char* deviceName);
								~RTL8814AUDevice();

	// Post-construction status check
	status_t					InitCheck() const { return fInitStatus; }

	// Device identity
	const char*					Name() const { return fDeviceName; }
	uint32						SlotIndex() const { return fSlotIndex; }

	// Device state
	bool						IsOpen() const { return fOpenCount > 0; }
	bool						IsRemoved() const { return fRemoved; }
	void						SetRemoved();

	// --------------- Static device_hooks wrappers ---------------
	// These are called by the kernel via the device_hooks struct
	// returned from find_device(). They resolve the device instance
	// from the cookie and dispatch to instance methods.

	static status_t				Open(const char* name, uint32 flags,
									void** cookie);
	static status_t				Close(void* cookie);
	static status_t				Free(void* cookie);
	static status_t				Control(void* cookie, uint32 op,
									void* args, size_t length);
	static status_t				Read(void* cookie, off_t position,
									void* buffer, size_t* numBytes);
	static status_t				Write(void* cookie, off_t position,
									const void* buffer, size_t* numBytes);

private:
	// Hardware initialization — called on first open()
	status_t					_InitHardware();
	status_t					_PowerOnSequence();
	status_t					_InitMAC();
	status_t					_InitRxAggregation();
	void						_DumpRxState(const char* tag);
	status_t					_ConfigTrxPath();

	// Open-network join sequencing: TX of auth+assoc requests and RX
	// dispatch for the responses.  Driven by IOC_HAIKU_JOIN.
	status_t					_DoJoin(const uint8* bssid,
									const char* ssid, uint32 ssidLen);
	status_t					_SendAuthRequest();
	status_t					_SendAssocRequest();
	void						_HandleAuthResponse(const uint8* frame,
									uint32 length);
	void						_HandleAssocResponse(const uint8* frame,
									uint32 length);

	// Post-associate worker — fires the H2C sequence (RA_INFO and
	// MEDIA_STATUS_RPT) that primes the chip's firmware to actually
	// TX our data frames on-air.  Runs off the RX bulk-callback path
	// because synchronous USB control transfers from there deadlock.
	static int32				_PostAssocThreadEntry(void* arg);
	void						_PostAssocLoop();
	status_t					_DoPostAssocSetup();
	status_t					_InitPageAllocation();
	status_t					_InitLLTTable();
	status_t					_InitQueuePriority();
	status_t					_EnableDMA();

	// USB endpoint setup — called from constructor
	status_t					_SetupEndpoints();

	// MAC address management
	status_t					_ReadMacAddress();
	status_t					_WriteMacAddress();

	// RX frame callback — called by the RX path for each received frame
	static void					_RxFrameReceived(void* cookie,
									const uint8* frameData,
									uint32 frameLength,
									const RxFrameInfo* info);

	// Parse a beacon or probe response to update the BSS list
	void						_ParseBeaconOrProbe(
									const uint8* frameData,
									uint32 frameLength,
									const RxFrameInfo* rxInfo);

	// Forward parsed BSS info to the WiFi manager
	void						_UpdateBssEntry(const uint8* bssid,
									const char* ssid, uint8 ssidLength,
									uint8 channel, uint16 beaconInterval,
									uint16 capability,
									SecurityType security, int8 rssi,
									const uint8* ieData, uint32 ieLength);

	// 802.11 ioctl handlers — dispatched from Control() for
	// SIOCS80211 / SIOCG80211.  args is a USER pointer to a
	// struct ieee80211req; the helpers do their own user_memcpy().
	status_t					_Set80211(void* userArgs, size_t length);
	status_t					_Get80211(void* userArgs, size_t length);
	status_t					_DoScanRequest();
	status_t					_GetScanResults(void* userBuffer,
									uint16& userLength);

	// Background thread that waits for the C2H scan-done event and
	// publishes B_NETWORK_WLAN_SCANNED via the net-notifications
	// module so userland scan listeners wake up.
	static int32				_ScanNotifierThreadEntry(void* arg);
	void						_ScanNotifierLoop();

	// Cleanup
	void						_Shutdown();

	// USB device and endpoints
	usb_device					fUSBDevice;
	usb_pipe					fBulkIn;
	usb_pipe					fBulkOut[kBulkOutEndpointCount];
	usb_pipe					fInterruptIn;

	// Device identity and state
	uint32						fSlotIndex;
	char						fDeviceName[64];
	status_t					fInitStatus;
	bool						fRemoved;
	bool						fHardwareInitialized;

	// Pending-join state populated by IEEE80211_IOC_SSID / _BSSID, used by
	// _MLME when wpa_supplicant orchestrates the actual auth/assoc.
	// All zero-initialized in the constructor — net_server probes the
	// ioctl interface before userland sets state, and we must never run
	// _DoJoin against uninitialized memory.
	char						fJoinSsid[33];
	uint32						fJoinSsidLength;
	uint8						fJoinBssid[6];

	// Per-interface 802.11 parameters that wpa_supplicant queries at
	// init (driver_bsd's IEEE80211_IOC_ROAMING / _PRIVACY / _WPA GETs)
	// and toggles later in the WPA2 sequence.  Stored as state today;
	// the chip programming happens elsewhere (privacy goes hand-in-hand
	// with the CCMP cipher enable, WPA mode shapes the assoc-req IEs).
	int32						fRoaming;
	int32						fPrivacy;
	int32						fWpaMode;
	int32						fAuthMode;
	int32						fDropUnencrypted;

	// RSN information element supplied by wpa_supplicant via
	// IEEE80211_IOC_APPIE / IEEE80211_APPIE_WPA.  When the assoc-req
	// is built for a WPA2 join, these bytes get inserted verbatim
	// after the SSID + supported-rates IEs.
	uint8						fWpaIe[256];
	uint32						fWpaIeLength;

	// Open-network auth+assoc state machine driven by IOC_HAIKU_JOIN.
	enum JoinState {
		kJoinIdle,
		kJoinAuthenticating,	// auth-req sent, waiting for auth-resp
		kJoinAssociating,	// assoc-req sent, waiting for assoc-resp
		kJoinConnected		// associated; data path active
	};
	JoinState					fJoinState;
	uint16						fJoinSeqCounter;

	// Worker thread + semaphore for the post-associate H2C sequence.
	sem_id						fPostAssocSem;
	thread_id					fPostAssocThread;
	bool						fPostAssocStop;
	int32						fOpenCount;

	// Synchronization
	mutex						fLock;

	// RX ring buffer — received data frames are queued here by the
	// _RxFrameReceived callback and dequeued by Read().
	static const uint32			kRxRingSlots = 64;
	static const uint32			kRxRingFrameMaxSize = 2400;

	struct RxRingEntry {
		uint8	data[2400];		// Frame payload
		uint32	length;			// Actual frame length (0 = empty)
	};

	RxRingEntry					fRxRing[kRxRingSlots];
	uint32						fRxRingHead;	// Next slot to write
	uint32						fRxRingTail;	// Next slot to read
	sem_id						fRxDataReady;	// Signaled when frame available

	// EAPOL inbox — diverted from the data ring in _RxFrameReceived
	// when an incoming frame's ethertype is 0x888E.  Single-slot:
	// the 4-way handshake is strictly one-message-at-a-time per peer
	// and we have only one BSS to talk to.  fEapolReady fires when
	// the inbox transitions empty -> filled; the in-driver WPA2 state
	// machine acquires it and drains the slot under fLock.
	struct EapolFrame {
		uint8	senderMac[6];	// 802.11 Addr3 (real source = AP's MAC)
		uint32	length;			// payload length (EAPOL body, no ether hdr)
		uint8	payload[300];	// EAPOL bytes starting at version
	};
	EapolFrame					fEapolInbox;
	bool						fEapolPending;
	sem_id						fEapolReady;

	// 4-way handshake state machine.  Spawned at device open, runs
	// for the lifetime of the open.  Drains fEapolInbox under fLock,
	// drives M1 -> M2 -> M3 -> M4, derives PTK, programs chip CAM.
	thread_id					fEapolThread;
	bool						fEapolStop;
	enum EapolState {
		kEapolIdle,			// no handshake in progress / done
		kEapolWaitM1,		// associated, waiting for AP's M1
		kEapolWaitM3,		// sent M2, waiting for M3
		kEapolDone			// keys installed, link is encrypted
	};
	EapolState					fEapolState;
	uint8						fAnonce[32];	// from M1
	uint8						fSnonce[32];	// our random, computed before M2
	uint8						fPmk[32];		// pairwise master key (from PBKDF2)
	bool						fPmkValid;
	uint8						fPtk[48];		// KCK[0..15] || KEK[16..31] || TK[32..47]
	bool						fPtkValid;
	uint64						fM1ReplayCounter;	// echoed back in M2

	// Set true once _InstallSessionKeys has programmed the chip CAM
	// and flipped kRegSecCfg.  When true, Write() marks outbound data
	// frames as CCMP-encrypted (Protected=1, secType=AES) so the chip's
	// hardware crypto engine encrypts the payload before TX.  Stays
	// false during the 4-way handshake itself — EAPOL frames must go
	// out unencrypted because the AP has no keys for us yet.
	bool						fCcmpEnabled;

	// Stash of the GTK (group temporal key) extracted from M3.  The
	// chip's HW decrypt engine declines to decrypt incoming CCMP
	// broadcasts (signaled via the SWDEC bit in the RX descriptor —
	// chip is asking us to do SW decrypt) so we keep the key around
	// to decrypt those frames in software in _RxFrameReceived.
	// Pairwise key for unicast SW decrypt = fPtk[32..47] (TK).
	uint8						fGtk[16];
	uint8						fGtkKeyId;
	bool						fGtkValid;

	// TX-side packet number counters for SW CCMP encrypt.  IEEE 802.11i
	// requires a strictly-monotonic PN per (key, peer) pair so the AP
	// can replay-detect; we maintain a separate counter for unicast
	// (pairwise key TK) and multicast (group key GTK) TX.  Both start
	// at 1 — PN=0 is reserved.  Width is uint64 because the on-air PN
	// is 48 bits; the upper 16 bits are unused.
	uint64						fTxPnPairwise;
	uint64						fTxPnGroup;

	static int32				_Eapol4WayThreadEntry(void* arg);
	void						_Eapol4WayLoop();
	void						_HandleEapolFrame(const uint8* payload,
									uint32 length, const uint8 senderMac[6]);
	void						_GenerateSnonce(uint8 nonce[32]);
	status_t					_TxEapolDataFrame(const uint8* apMac,
									const uint8* eapol, uint32 eapolLen);

	// Security CAM programming.  After M4 we have the pairwise TK
	// (PTK[32..47]) and the group GTK extracted from M3.  We program
	// both into the chip's 32-entry CAM via REG_CAMCMD/REG_CAMWRITE
	// and flip kRegSecCfg to enable hardware AES-CCMP on TX and RX.
	// _CamWrite is the low-level "write one CAM dword" helper; it
	// stages the data in CAMWRITE then issues CAMCMD with POLLING and
	// waits for the chip to clear POLLING (typically ~5 µs).
	status_t					_CamWrite(uint8 addr, uint32 data);
	status_t					_ProgramCamEntry(uint8 entry, uint8 algo,
									uint8 keyId, bool isGroupKey,
									const uint8 mac[6],
									const uint8* key, uint32 keyLen);
	status_t					_InstallSessionKeys(const uint8* tk,
									const uint8* gtk, uint32 gtkLen,
									uint8 gtkKeyId,
									const uint8 apMac[6]);

	// Link state change notification
	sem_id						fLinkStateSem;	// Provided by network stack

	// Scan-complete notifier thread.  Spawned by _DoScanRequest(),
	// joins on the WiFiManager's scan-done sem then publishes a
	// B_NETWORK_WLAN_SCANNED event.  -1 when no notifier is in flight.
	thread_id					fScanNotifierThread;

	// MAC address (read from EFUSE during init)
	uint8						fMacAddress[6];

	// Owned subsystem modules — created in constructor, initialized
	// lazily on first open(). Each module handles a specific aspect
	// of the hardware.
	RTL8814AURegisterIO*		fRegisterIO;
	RTL8814AUFirmware*			fFirmware;
	RTL8814AUEfuseReader*		fEfuseReader;
	RTL8814AUPhyConfig*			fPhyConfig;
	RTL8814AUTxPath*			fTxPath;
	RTL8814AURxPath*			fRxPath;
	RTL8814AUWiFiManager*		fWiFiManager;
};


#endif	// RTL8814AU_DEVICE_H
