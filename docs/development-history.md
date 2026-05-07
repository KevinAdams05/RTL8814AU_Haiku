>[!NOTE]
>An LLM was used to aid in development of this code.

# Development history

The arc of how this driver came to exist.  Useful when you're trying
to understand *why* a piece of code looks the way it does — most of
these decisions came out of a specific bug we hit and worked around.

## 2026-04 — Bring-up

We started from morrownr's Linux fork of the rtl8814au driver and a
couple of full USB packet-capture traces of a working Linux session
(`morrownr-coldstart.pcap`, ~24k packets covering full driver init +
a scan).  The plan was to write a Haiku-native driver from scratch
using Linux only as a reference, never as code to copy verbatim
(per project policy).

Major hurdles in order:

| Date | Milestone | Notes |
|---|---|---|
| 2026-04-15 | First USB enumeration, EFUSE read, MAC address recovered | Cracked the EFUSE map layout from morrownr's `phy_e.h` |
| 2026-04-20 | Firmware load via IDDMA | Discovered the 8-byte XOR trailer per section that the firmware blob has but the header sizes don't include.  See [firmware.md](firmware.md). |
| 2026-04-25 | First TX (probe-request) | TX descriptor format has chip-specific quirks — packet length goes in dword 0, queue in dword 1, and a checksum field the chip computes itself once you set the right enable bit. |
| 2026-04-27 | First on-air TX confirmed | Sniffer on a separate machine saw our probe-req. |
| 2026-04-29 | Scan returns 19 networks | Major architectural fix: Realtek's BB-write-lock isn't `0x1002 BIT0` (the clock-gate from the reference driver).  It's BIT1 — `FEN_BB_GLB_RSTn`.  Without setting BIT1, every BB-region write to 0x800–0x1FFF was silently dropped, even though the driver's apply-table thought it was succeeding.  Found by replaying frame-9642 of the cold-start trace and reading register 0x1002 as `0x03` after that frame.  Adding a Step 5.4 that ORs 0x03 into byte 0x1002 unlocked BB-region writes — verified by a readback-stuck-yes probe. |

The cold-start trace replay (commit `060b18ffca`) was a turning point.
Instead of patching our hand-derived BB/AGC tables one entry at a time,
we replaced them with a flat replay of the 3914 4-byte writes morrownr
performs in cold-start.  Worked first try.  RX began immediately.

## 2026-05-04 — Open-network end-to-end

Consolidated the bring-up into a clean state machine and got DHCP +
ping working over an open Wi-Fi network (ADAMS-Guest, briefly
unsecured for testing).

| Milestone | Note |
|---|---|
| RX 802.11 → ethernet conversion in `_RxFrameReceived` (commit `adbb1d3281`) | Driver registers as `Hardware type: Ethernet` but `Read()` was handing the network stack raw 802.11 frame bytes.  net_server tried to parse the 802.11 header (FC + Dur + 3 MAC addresses + SeqCtrl) as a 14-byte ethernet header, failed, and dropped every DHCP/ARP/IP packet we received.  Added the symmetric counterpart to the eth → 802.11 conversion already present in `Write()`.  After this, DHCP completed cleanly — `192.168.74.176` assigned, pings to gateway and 8.8.8.8 succeeded. |
| BFS panic fixed via dprintf trim (commits `080f516ec9`, `6182dd7d89`) | A `PANIC: acquire_vnode (...): node was not used!` showed up while running with the verbose RX-callback log.  Stack trace was syslog-daemon writing dirty pages while a vnode refcount went to zero — `_x86_64_syscall_entry → common_user_io → dec_vnode_ref_count → free_vnode → VMCache::WriteModified → vfs_write_pages → bfs_io → acquire_vnode`.  Zero RTL8814AU frames in the trace.  The driver's per-callback dprintf at every-64th was emitting 3.5 lines/sec, enough to expose a latent BFS race during log rotation.  Throttling to every-4096 stopped reproducing the panic. |

## 2026-05-05/06 — wpa_supplicant integration attempt

Tried to make wpa_supplicant drive the WPA2-PSK handshake via the
standard FreeBSD ioctl interface.  This was the long road.

| Milestone | Note |
|---|---|
| `IOC_ROAMING` / `_PRIVACY` / `_WPA` / `_DEVCAPS` (commit `29c290e81d`) | Without these GETs returning success, wpa_supplicant's `wpa_driver_bsd_init` bails before anything else fires.  Implementing them as state-backed get/set unblocked the wpa_supplicant init sequence. |
| `SIOCSIFMEDIA` accept (commit `fdeae19381`) | wpa_supplicant's `bsd_set_mediaopt` issues `SIOCGIFMEDIA` + `SIOCSIFMEDIA` before every scan.  SIOCGIFMEDIA was synthesized by Haiku from `ETHER_GET_LINK_STATE`, but SIOCSIFMEDIA fell through to our default ENOTTY branch, making `wpa_driver_bsd_scan` early-bail before `IOC_SCAN_REQ`.  Adding a no-op accept for SIOCSIFMEDIA finally let wpa_supplicant trigger our scan. |
| Proper device path in `B_NETWORK_WLAN_SCANNED` (commit `e674ad1750`) | We were sending `interface=fDeviceName` ("ASUS USB-AC68") in the scan-complete notification.  wpa_supplicant's `_NotifyNetworkEvent` does `interfaceName.Prepend("/dev/")` and looks up its driver_bsd_data by ifname (`/dev/net/rtl8814au/0`).  "/dev/ASUS USB-AC68" matches nothing → event dropped → wpa_supplicant never fetches scan results.  Fixed by sending `net/rtl8814au/<slot>` built from `RTL8814AU_DEVICE_PATH_BASE + fSlotIndex`. |
| WPA2 ASSOC works on-air (commit `230123c4e7`) | `IOC_MLME` op=ASSOC handler now calls `_DoJoin` with the bssid + ssid from the request, and `_SendAssocRequest` splices the RSN IE wpa_supplicant deposited via `IOC_APPIE` into the assoc-req body and sets the Privacy bit in the capability info.  AP accepted us with `cap=0x1011 status=0 aid=50` — first verified WPA2 association on-air. |
| `B_NETWORK_WLAN_JOINED` notification + EAPOL diagnostic (commit `48b1b95fa6`) | Without firing `B_NETWORK_WLAN_JOINED`, wpa_supplicant doesn't know we're associated and times out.  Added the notification.  Also added EAPOL-payload logging to confirm the AP was sending us EAPOL-Key M1 — and it was, four times in retry, payload bytes `02 03 00 5f 02 00 8a` decoding cleanly to `M1, descVer=2, pair, ack`. |

At that point everything looked right.  AP sent M1.  wpa_supplicant
should have replied with M2.  It never did.  We chased that.

## 2026-05-07 — Diagnosis: Haiku's stack drops EAPOL

Built a tiny userland test program (`eapol_sniff.c`) that replicated
exactly what `l2_packet_haiku.c` does — `socket(AF_LINK, SOCK_DGRAM, 0)`,
`SIOCGIFADDR`, set `sdl_e_type = htons(0x888E)`, `bind`, `recvfrom`.
The bind succeeded, the kernel registered our handler, our chip
RX'd 4 EAPOL-Key M1 frames, `ifconfig` counted them as RX (4 packets,
412 bytes = exactly 4 × 103-byte M1).  But our `recvfrom` blocked
forever.  `tcpdump` saw nothing either.

Conclusion: Haiku's network stack does not deliver ethertype 0x888E
frames to AF_LINK packet sockets, even when a handler is correctly
registered.  Probably a kernel network-stack issue (no
0x888E/ETHERTYPE_PAE handling outside `freebsd_wlan`'s filtering
logic).  Confirmed by surveying every Wi-Fi driver in
`src/add-ons/kernel/drivers/network/wlan/` — they all use the same
`if_input → ether_input → ifp->receive_queue → compat_receive →
Haiku stack` path.  None of them work for WPA2 either.

This is a real Haiku platform bug.  Filing a kernel patch upstream
would help all Wi-Fi drivers, but is out of scope for an unofficial
standalone .hpkg.  We pivoted to **Path B**: run the entire 4-way
handshake in-driver, never let EAPOL touch userland.

## 2026-05-07 — Path B: in-driver WPA2

Three commits that built the foundation:

| Commit | What |
|---|---|
| `7dd902bb89` (step 1) | Divert ethertype 0x888E frames in `_RxFrameReceived` to a single-slot `fEapolInbox` instead of pushing to the data ring.  Confirmed by `ifconfig` RX going from 4/412 to 0/0 — no EAPOL leaks past the driver. |
| `fe928b5ac9` (steps 2+3) | `WPA2Crypto.{h,cpp}` — kernel-side SHA-1, HMAC-SHA1, AES-128 enc/dec, RFC 3394 unwrap, PRF-384.  Plus the EAPOL 4-way worker thread that drains the inbox, parses Key Info, classifies M1/M3 by bit pattern, captures ANonce, transitions state. |
| `c9b8858a99` (step 6 + step 2 cont) | PBKDF2 added to `WPA2Crypto`.  `IOC_HAIKU_JOIN` extended to accept the rich `rtl_haiku_join_psk` struct (SSID + passphrase + BSSID).  Driver runs PBKDF2 in-kernel, captures PMK, drives `_DoJoin`, handles M1, derives PTK via PRF-384, builds full M2 with HMAC-SHA1 MIC.  New userland tool `wpa2_join` issues the rich ioctl. |

Verified end-to-end: `wpa2_join /dev/net/rtl8814au/0 ADAMS-Guest <pass>`
produces

```
IOC_HAIKU_JOIN (WPA2-PSK): ssid='ADAMS-Guest' bssid=00:... PMK[0..3]=97ed6fc3
_DoJoin 'ADAMS-Guest' -> ca:98:b5:d9:a2:86 ch=1
TX auth request seq=1
RX auth response alg=0 seq=2 status=0
TX assoc request (open) len=57
RX assoc response cap=0x1011 status=0 aid=38
ASSOCIATED to 'ADAMS-Guest' AID=38
```

PBKDF2 ran cleanly, auth + assoc succeeded, AP issued AID.  The
in-driver state machine and crypto plumbing are in place.  Open
items to actually complete the handshake are tracked in
[known-issues.md](known-issues.md).
