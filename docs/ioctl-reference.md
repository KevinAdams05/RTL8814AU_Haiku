>[!NOTE]
>An LLM was used to aid in development of this code.

# IOCTL reference

Quick reference for the ioctl interface.  All of these go through
`Device::Control()` after a userland process opens
`/dev/net/rtl8814au/0`.

## Top-level ops (Control switch)

| Op code | Name | Notes |
|---|---|---|
| `B_DEVICE_OP_CODES_END + 0` | `ETHER_GETADDR` | Returns our MAC (read from EFUSE) |
| `B_DEVICE_OP_CODES_END + 1` | `ETHER_INIT` | obsolete; we accept and return B_OK so `ethernet_up` proceeds |
| `B_DEVICE_OP_CODES_END + 2` | `ETHER_NONBLOCK` | non-blocking toggle, currently ignored |
| `B_DEVICE_OP_CODES_END + 5` | `ETHER_SETPROMISC` | promiscuous mode toggle |
| `B_DEVICE_OP_CODES_END + 6` | `ETHER_GETFRAMESIZE` | returns 1500 (MTU) |
| `B_DEVICE_OP_CODES_END + 7` | `ETHER_SET_LINK_STATE_SEM` | net_server passes a semaphore for us to release on link change |
| `B_DEVICE_OP_CODES_END + 8` | `ETHER_GET_LINK_STATE` | returns `ether_link_state_t {media, quality, speed}` — `media` is `IFM_IEEE80211 \| IFM_ACTIVE` when associated |
| `SIOCEND + 234` | `SIOCS80211` | dispatched to `_Set80211` |
| `SIOCEND + 235` | `SIOCG80211` | dispatched to `_Get80211` |
| `SIOCSIFMEDIA` (8924) | accept | wpa_supplicant's `bsd_set_mediaopt` runs this; no-op for us |
| (unknown) | logged + return `B_DEV_INVALID_IOCTL` | first 32 unknown ops are dprintf'd to help diagnose new userland callers |

## SIOCS80211 / SIOCG80211 sub-ops (i_type)

The 802.11-specific ioctls follow the FreeBSD `<net80211/ieee80211_ioctl.h>`
convention.  Each carries a `struct ieee80211req` whose `i_type`
selects the operation.

### Implemented

| `i_type` | Name | Set / Get | Behavior |
|---|---|---|---|
| 1 | `IEEE80211_IOC_SSID` | S | stores into `fJoinSsid` for later HAIKU_JOIN |
| 7 | `IEEE80211_IOC_AUTHMODE` | S/G | state-backed (`fAuthMode`) |
| 15 | `IEEE80211_IOC_BSSID` | S | programs chip BSSID register, stores into `fJoinBssid` |
| 15 | `IEEE80211_IOC_BSSID` | G | returns currently-associated BSSID via WiFiManager |
| 16 | `IEEE80211_IOC_ROAMING` | S/G | state-backed (`fRoaming`) |
| 17 | `IEEE80211_IOC_PRIVACY` | S/G | state-backed (`fPrivacy`).  When non-zero, `_SendAssocRequest` sets the Privacy bit in the cap-info field |
| 18 | `IEEE80211_IOC_DROPUNENCRYPTED` | S/G | state-backed (`fDropUnencrypted`); chip drops are implicit once CCMP is on |
| 19 | `IEEE80211_IOC_WPAKEY` | S | stub; logs the key.  Real implementation pending (see [known-issues.md](known-issues.md)). |
| 20 | `IEEE80211_IOC_DELKEY` | S | accept; nothing to delete in current state |
| 21 | `IEEE80211_IOC_MLME` | S | parses `ieee80211req_mlme`, dispatches on `im_op`: ASSOC → calls `_DoJoin`; DEAUTH/DISASSOC → resets state, calls `WiFiManager::Disconnect` |
| 25 | `IEEE80211_IOC_COUNTERMEASURES` | S | accept; we don't do TKIP |
| 26 | `IEEE80211_IOC_WPA` | S/G | state-backed (`fWpaMode`) |
| 76 | `IEEE80211_IOC_SCAN_RESULTS` | G | calls `_GetScanResults` — formats `fBssList` as a series of `ieee80211req_scan_result` records |
| 78 | `IEEE80211_IOC_STA_INFO` | G | currently zero-length response |
| 95 | `IEEE80211_IOC_APPIE` | S | when `i_val == IEEE80211_APPIE_WPA` (0xD0), stores up to 256 bytes of IE into `fWpaIe`.  The assoc-req TX path splices this into the body when present. |
| 98 | `IEEE80211_IOC_DEVCAPS` | G | returns 16-byte `ieee80211_devcaps_req_min` advertising `IEEE80211_C_WPA1 \| IEEE80211_C_WPA2` |
| 103 | `IEEE80211_IOC_SCAN_REQ` | S | calls `_DoScanRequest`; spawns a notifier thread that fires `B_NETWORK_WLAN_SCANNED` after 8 sec |
| 104 | `IEEE80211_IOC_SCAN_CANCEL` | S | accept (no-op) |
| 0x6002 | `IEEE80211_IOC_HAIKU_JOIN` | S | dispatches on `i_len`: zero → legacy open-network path with state from prior `IOC_SSID`/`IOC_BSSID`; `sizeof(rtl_haiku_join_psk)` → rich path with PBKDF2 + in-driver WPA2 (see below) |

### Not implemented

| `i_type` | Name | Status |
|---|---|---|
| any other | falls through to default | logged with hex dump of first 16 bytes of i_data, returns `B_DEV_INVALID_IOCTL` |

## `IOC_HAIKU_JOIN` rich payload

Driver-specific extension carried in `i_data` when `i_len ==
sizeof(struct rtl_haiku_join_psk)`.  Defined in `WiFiIoctl.h`:

```c
struct rtl_haiku_join_psk {
    uint8  jp_bssid[6];                 // target BSSID, or 6 zero bytes
                                        // to look up by SSID
    uint8  jp_ssid_len;                 // 1..32
    uint8  jp_pad;
    uint8  jp_ssid[32];                 // SSID bytes
    uint8  jp_passphrase_len;           // 8..63
    uint8  jp_pad2[3];
    uint8  jp_passphrase[64];           // ASCII passphrase
};
```

Driver flow on receipt:

1.  Validate lengths.
2.  Run `pbkdf2_hmac_sha1(passphrase, ssid, 4096, 32)` to derive PMK
    into `fPmk[32]`.  Set `fPmkValid = true`.
3.  Set `fEapolState = kEapolWaitM1`, clear `fPtkValid`.
4.  Copy SSID/BSSID into `fJoinSsid`/`fJoinBssid`.
5.  Call `_DoJoin` to drive auth + assoc.

When the AP later sends EAPOL-Key M1, the EAPOL worker thread picks
up from `kEapolWaitM1` and runs the rest of the 4-way handshake —
see [wpa2-in-driver.md](wpa2-in-driver.md).

## Userland tools that drive these IOCs

- **`ifconfig`** — open networks via `ifconfig <dev> join <ssid>`.
  Goes through `IOC_SSID` + `IOC_HAIKU_JOIN` (zero-len) for open
  networks.
- **`wpa_supplicant`** (BApplication, launched by net_server) —
  drives the full freebsd_wlan ioctl set: ROAMING, PRIVACY, WPA,
  DEVCAPS, AUTHMODE, DROPUNENCRYPTED, APPIE, MLME ASSOC, etc.  We
  support its init through the IOC layer but the EAPOL handshake
  never completes via wpa_supplicant due to a Haiku stack bug.
- **`wpa2_join`** (ours) — issues a single
  `SIOCS80211 IOC_HAIKU_JOIN` with the rich struct, used to drive
  the in-driver WPA2 path.
