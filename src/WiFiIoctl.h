/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * WiFiIoctl.h — Minimal ABI for the IEEE 802.11 ioctls that userland uses
 * to talk to WLAN device drivers.
 *
 * This header reproduces just enough of the FreeBSD-derived
 * net80211 ioctl interface (struct ieee80211req, ieee80211_scan_req,
 * ieee80211req_scan_result, the IEEE80211_IOC_* command codes) for the
 * RTL8814AU driver to handle SIOCS80211 / SIOCG80211 without pulling in
 * the entire freebsd_wlan compat layer.
 *
 * Layout MUST match exactly what BNetworkDevice / ifconfig and the rest
 * of net80211_ioctl.h define — they share a single ABI.  See
 * src/libs/compat/freebsd_wlan/net80211/ieee80211_ioctl.h for the
 * canonical definitions.
 */
#ifndef RTL8814AU_WIFI_IOCTL_H
#define RTL8814AU_WIFI_IOCTL_H


#include <SupportDefs.h>

// libnetapi (BNetworkDevice / NetworkDevice.cpp) is built with
// src/libs/compat/freebsd_network/ on its include path, so its
// <compat/sys/ioccom.h> resolves to the FreeBSD compat shim where
//   _IOW(g,n,t)  := SIOCEND + n
//   _IOWR(g,n,t) := SIOCEND + n
// not the BSD-style bit-encoded form (IOC_IN | len<<16 | group<<8 | n).
// We must use the same convention here so SIOCS80211/SIOCG80211 match
// the values userland actually puts on the wire.
#include <sys/sockio.h>


// Sizes that match the canonical ABI exactly.
#ifndef IEEE80211_ADDR_LEN
#	define IEEE80211_ADDR_LEN	6
#endif
#ifndef IEEE80211_NWID_LEN
#	define IEEE80211_NWID_LEN	32
#endif
#ifndef IEEE80211_RATE_MAXSIZE
#	define IEEE80211_RATE_MAXSIZE	15
#endif
#ifndef IFNAMSIZ
#	define IFNAMSIZ	32
#endif


// SIOC*80211 carry a struct ieee80211req describing what to get/set.
struct ieee80211req {
	char		i_name[IFNAMSIZ];	// device name (e.g. \"rtl8814au/0\")
	uint16		i_type;				// IEEE80211_IOC_* command code
	int16		i_val;				// scalar value (set/get)
	uint16		i_len;				// length of i_data buffer
	void*		i_data;				// command-specific extra data
};

#define SIOCS80211	(SIOCEND + 234)
#define SIOCG80211	(SIOCEND + 235)


// Command codes (subset — what we actually handle).
#define IEEE80211_IOC_SSID				1
#define IEEE80211_IOC_BSSID				15
#define IEEE80211_IOC_SCAN_RESULTS		76
#define IEEE80211_IOC_STA_INFO			78
#define IEEE80211_IOC_SCAN_REQ			103
#define IEEE80211_IOC_SCAN_CANCEL		104

#define IEEE80211_IOC_AUTHMODE			7
#define IEEE80211_IOC_ROAMING			16
#define IEEE80211_IOC_PRIVACY			17
#define IEEE80211_IOC_DROPUNENCRYPTED	18
#define IEEE80211_IOC_WPAKEY			19
#define IEEE80211_IOC_DELKEY			20
#define IEEE80211_IOC_MLME				21
#define IEEE80211_IOC_COUNTERMEASURES	25
#define IEEE80211_IOC_WPA				26
#define IEEE80211_IOC_APPIE				95
#define IEEE80211_IOC_DEVCAPS			98


// IOC_APPIE i_val identifying which mgmt frame this IE attaches to.
// FreeBSD's IEEE80211_APPIE_WPA = TYPE_MGT(0x00) | SUBTYPE_BEACON(0x80)
//                               | SUBTYPE_PROBE_RESP(0x50) = 0xD0.
// Despite the name, wpa_supplicant uses this tag for the RSN IE
// that must be inserted into our outgoing assoc-req.
#define IEEE80211_APPIE_WPA				0xD0


// Driver-capability bits used in ieee80211_devcaps_req::dc_drivercaps.
// Mirror of net80211/ieee80211_var.h.  We advertise only the subset
// wpa_supplicant's bsd backend looks for.
#define IEEE80211_C_HOSTAP				0x00000400
#define IEEE80211_C_WPA1				0x00800000
#define IEEE80211_C_WPA2				0x01000000


// Returned via i_data when i_type == IEEE80211_IOC_DEVCAPS.  We answer
// only the four scalar capability words; userland's bsd backend never
// inspects dc_chaninfo on Haiku, so we leave it zero-length.
struct ieee80211_devcaps_req_min {
	uint32	dc_drivercaps;
	uint32	dc_cryptocaps;
	uint32	dc_htcaps;
	uint32	dc_vhtcaps;
};

// Haiku-specific extensions (see freebsd_wlan/net80211/ieee80211_ioctl.h).
#define IEEE80211_IOC_HAIKU_COMPAT_WLAN_UP		0x6000
#define IEEE80211_IOC_HAIKU_COMPAT_WLAN_DOWN	0x6001
#define IEEE80211_IOC_HAIKU_JOIN				0x6002


// Rich payload for IOC_HAIKU_JOIN when invoking from our wpa2_join
// userland tool (rtl8814au-specific extension).  When userland passes
// a request whose i_len matches sizeof(rtl_haiku_join_psk), the driver
// runs PBKDF2 on the supplied passphrase + SSID to derive the PMK and
// drives the full WPA2 4-way handshake in-kernel.  When i_len matches
// the legacy zero-byte form, the existing open-network _DoJoin path
// runs (using fJoinSsid set via IOC_SSID).
//
// Why an rtl-specific name: the FreeBSD/Haiku struct named
// `ieee80211_haiku_join_req` includes a flexible `i_key[]` array
// holding either a 32-byte PMK (when wpa_supplicant runs PBKDF2) or
// a passphrase.  We don't need that variant since we can either get
// the PMK or do PBKDF2 ourselves; the simpler shape below is easier
// to drive from our standalone tool.
struct rtl_haiku_join_psk {
	uint8	jp_bssid[6];				// target BSSID (or zeros = lookup by SSID)
	uint8	jp_ssid_len;				// length of SSID (1..32)
	uint8	jp_pad;
	uint8	jp_ssid[32];				// SSID bytes
	uint8	jp_passphrase_len;			// length of passphrase (8..63)
	uint8	jp_pad2[3];
	uint8	jp_passphrase[64];			// ASCII passphrase
};


// Cipher key install — passed via i_data when i_type == IEEE80211_IOC_WPAKEY.
// Layout matches freebsd_wlan's ieee80211req_key.
#ifndef IEEE80211_KEYBUF_SIZE
#	define IEEE80211_KEYBUF_SIZE	16
#endif
#ifndef IEEE80211_MICBUF_SIZE
#	define IEEE80211_MICBUF_SIZE	8
#endif
struct ieee80211req_key {
	uint8	ik_type;	// cipher type (CCMP, TKIP, ...)
	uint8	ik_pad;
	uint16	ik_keyix;	// key index (slot 0..3 group, IEEE80211_KEYIX_NONE for pairwise)
	uint8	ik_keylen;	// key length in bytes
	uint8	ik_flags;	// IEEE80211_KEY_XMIT / KEY_RECV / KEY_DEFAULT
	uint8	ik_macaddr[IEEE80211_ADDR_LEN];
	uint64	ik_keyrsc;	// key RX sequence counter
	uint64	ik_keytsc;	// key TX sequence counter
	uint8	ik_keydata[IEEE80211_KEYBUF_SIZE + IEEE80211_MICBUF_SIZE];
};

// MLME state-manipulation request — i_data when i_type == IEEE80211_IOC_MLME.
struct ieee80211req_mlme {
	uint8	im_op;		// IEEE80211_MLME_*
#define IEEE80211_MLME_ASSOC		1
#define IEEE80211_MLME_DISASSOC		2
#define IEEE80211_MLME_DEAUTH		3
#define IEEE80211_MLME_AUTHORIZE	4
#define IEEE80211_MLME_UNAUTHORIZE	5
#define IEEE80211_MLME_AUTH		6
	uint8	im_ssid_len;
	uint16	im_reason;	// 802.11 reason code
	uint8	im_macaddr[IEEE80211_ADDR_LEN];
	uint8	im_ssid[IEEE80211_NWID_LEN];
};


// Scan request — passed via i_data when i_type == IEEE80211_IOC_SCAN_REQ.
struct ieee80211_scan_req {
	int		sr_flags;
	uint32	sr_duration;
	uint32	sr_mindwell;
	uint32	sr_maxdwell;
	int		sr_nssid;
#define IEEE80211_IOC_SCAN_MAX_SSID	3
	struct {
		int		len;
		uint8	ssid[IEEE80211_NWID_LEN];
	} sr_ssid[IEEE80211_IOC_SCAN_MAX_SSID];
};

// sr_flags values
#define IEEE80211_IOC_SCAN_NOPICK		0x00001
#define IEEE80211_IOC_SCAN_ACTIVE		0x00002
#define IEEE80211_IOC_SCAN_PICK1ST		0x00004
#define IEEE80211_IOC_SCAN_BGSCAN		0x00008
#define IEEE80211_IOC_SCAN_ONCE			0x00010
#define IEEE80211_IOC_SCAN_NOBCAST		0x00020
#define IEEE80211_IOC_SCAN_NOJOIN		0x00040
#define IEEE80211_IOC_SCAN_FLUSH		0x10000
#define IEEE80211_IOC_SCAN_CHECK		0x20000


// One scan result record — variable length (isr_len bytes total).
// Layout: this fixed header, then variable SSID, then variable IE data.
// All records are rounded to a multiple of 4 bytes.
struct ieee80211req_scan_result {
	uint16	isr_len;					// total record length (4-aligned)
	uint16	isr_ie_off;					// offset to SSID + IE data
	uint16	isr_ie_len;					// IE block length
	uint16	isr_freq;					// channel center freq (MHz)
	uint16	isr_flags;					// channel flags
	int8	isr_noise;
	int8	isr_rssi;
	uint16	isr_intval;					// beacon interval (TUs)
	uint8	isr_capinfo;				// capability info
	uint8	isr_erp;					// ERP element
	uint8	isr_bssid[IEEE80211_ADDR_LEN];
	uint8	isr_nrates;
	uint8	isr_rates[IEEE80211_RATE_MAXSIZE];
	uint8	isr_ssid_len;
	uint8	isr_meshid_len;
	// followed by ssid bytes, mesh id bytes, IE bytes
};


// Channel flag bits used by isr_flags (subset).
#define IEEE80211_CHAN_2GHZ		0x00080
#define IEEE80211_CHAN_5GHZ		0x00100
#define IEEE80211_CHAN_CCK		0x00020
#define IEEE80211_CHAN_OFDM		0x00040
#define IEEE80211_CHAN_DYN		0x00400


// Stub for STA_INFO get — only used to return 'no stations' currently.
struct ieee80211req_sta_info {
	uint16 isi_len;
	uint8 isi_pad[126];
};

#endif	// RTL8814AU_WIFI_IOCTL_H
