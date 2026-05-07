>[!NOTE]
>An LLM was used to aid in development of this code.

# In-driver WPA2-PSK (Path B)

The rtl8814au driver runs the entire WPA2-PSK 4-way handshake **inside
the kernel driver** rather than going through `wpa_supplicant`.  This is
unusual — most Wi-Fi drivers on every platform delegate the handshake
to userspace.  The motivation is a Haiku-specific bug we couldn't work
around any other way.

## Why not `wpa_supplicant`?

Haiku ships `wpa_supplicant 2.11` (HaikuPorts package).  It's a
`BApplication` that net_server launches when an interface joins a
WPA-secured network.  It uses the FreeBSD `driver_bsd.c` backend —
the same one any FreeBSD-derived Wi-Fi driver expects — so it talks
to the driver via `SIOCS80211` / `SIOCG80211` ioctls plus an `AF_LINK`
packet socket bound to ethertype 0x888E for EAPOL frames.

We got the ioctl side fully working: `wpa_driver_bsd_init`,
`bsd_set_mediaopt`, `IOC_DEVCAPS`, `IOC_WPA`, `IOC_PRIVACY`,
`IOC_AUTHMODE`, `IOC_DROPUNENCRYPTED`, `IOC_APPIE`, `IOC_MLME` (ASSOC
and DEAUTH), `IOC_DELKEY`, `IOC_COUNTERMEASURES` — wpa_supplicant
successfully drives our driver to associate and the AP issues an AID.

What never works is the EAPOL layer.

### What we proved

We built a small standalone test program (`eapol_sniff.c`) that does
exactly what `l2_packet_haiku.c` does:

```c
int s = socket(AF_LINK, SOCK_DGRAM, 0);
ioctl(s, SIOCGIFADDR, &req);          // -> link addr
((sockaddr_dl*)&req.ifr_addr)->sdl_e_type = htons(0x888E);
bind(s, &linkAddr, sizeof(linkAddr)); // succeeds
recvfrom(s, ...);                      // hangs forever
```

The bind succeeds, the kernel registers our handler in
`interface->receive_funcs` for `B_NET_FRAME_TYPE(IFT_ETHER, 0x888E)`,
the chip receives EAPOL frames, our `_RxFrameReceived` delivers them
to the network stack, and `ifconfig` increments its RX packet counter.
But `recvfrom` never returns and `tcpdump` (which uses the same path)
sees nothing either.

The frames go missing somewhere between
`device_consumer_thread` enqueuing them and the registered handler
running.  We didn't dig deeper because the conclusion is the same
for every `AF_LINK`-bound listener: ethertype 0x888E doesn't reach
userland on Haiku.

Fixing the kernel is a real Haiku bug worth addressing, but it's
out of scope for an unofficial standalone .hpkg driver.  We work
around it instead.

## Path B: in-driver WPA2

The 4-way handshake exchange:

![wpa2 state machine](diagrams/wpa2-state-machine.svg)

EAPOL frames never leave the driver:

![eapol diversion](diagrams/eapol-diversion.svg)

### What the driver does end-to-end

1.  Userland tool `wpa2_join` opens the device fd and issues
    `SIOCS80211 IOC_HAIKU_JOIN` with a custom `rtl_haiku_join_psk`
    payload (`{bssid, ssid, passphrase}`).
2.  Driver runs `pbkdf2_hmac_sha1(passphrase, ssid, 4096, 32)` to derive
    the 32-byte PMK.
3.  Driver runs `_DoJoin` — sets channel, writes BSSID register,
    drives auth + assoc.  Includes the RSN IE in the assoc-req.
4.  AP sends EAPOL-Key M1.  `_RxFrameReceived` notices ethertype
    0x888E and copies the frame into `fEapolInbox` instead of pushing
    it to the data ring (where it would be dropped by the broken
    Haiku stack).
5.  EAPOL worker thread wakes on `fEapolReady`, parses M1, captures
    the AP's ANonce.
6.  Driver generates SNonce from `system_time + real_time_clock_usecs +
    our MAC + counter` mixed through SHA-1.
7.  Driver derives PTK = `prf_384(PMK, "Pairwise key expansion",
    min/max(AA,SPA) || min/max(ANonce,SNonce))`.  Splits into
    KCK[0..15] || KEK[16..31] || TK[32..47].
8.  Driver builds EAPOL-Key M2 with our SNonce + RSN IE in key data,
    computes MIC = `hmac_sha1(KCK, M2)[0..15]`, patches it into the
    MIC field, TXes via `fTxPath` as a data frame wrapped in 802.11
    header + LLC/SNAP + ethertype 0x888E.
9.  AP sends M3 with encrypted GTK in key data.  Driver validates the
    MIC with KCK, decrypts the GTK using `aes_unwrap(KEK, ...)` (RFC
    3394).
10. Driver builds M4 (no key data, secure bit set, MIC over),
    TXes via `fTxPath`.
11. Driver programs PTK + GTK into the chip's security CAM via
    `REG_CAMCMD` / `REG_CAMWRITE` (rtwn-style).
12. Driver enables CCMP encryption in `kRegSECCFG`.  All subsequent
    data frames use hardware AES-CCMP.

After step 12 the connection is fully established and the existing
ethernet ⇄ 802.11 conversion in Read/Write carries plaintext data
between the kernel network stack and the chip's encryption engine.

### Crypto primitives (`WPA2Crypto.cpp`)

All implementations are kernel-side, no allocations, no globals other
than const tables.  Compact textbook implementations rather than vendoring
a third-party library.

| Primitive | Purpose | Source |
|---|---|---|
| `sha1` | Used internally by HMAC-SHA1 | RFC 3174 reference |
| `hmac_sha1` | EAPOL-Key MIC, PTK derivation, PBKDF2 | RFC 2104 |
| `pbkdf2_hmac_sha1` | Derive PMK from passphrase + SSID | RFC 2898 §5.2.  4096 iterations per WPA2-PSK |
| `prf_384` | Derive PTK from PMK + nonces + MAC addresses | IEEE 802.11i §8.5.1.1 |
| `aes128_encrypt` / `aes128_decrypt` | AES-128 ECB block cipher | FIPS 197 textbook impl using S-box + inverse S-box (no T-tables — keeps the static-data footprint small) |
| `aes_unwrap` | Decrypt the GTK delivered in M3's key data | RFC 3394 §2.2.2 |

### Threading

The handshake runs entirely on the EAPOL worker thread
(`_Eapol4WayLoop` in `Device.cpp`).  Spawned at hardware-init time,
blocks on `fEapolReady`, drains `fEapolInbox` under `fLock`, advances
the state machine.  The thread is the **only** writer of
`fEapolState`, `fAnonce`, `fSnonce`, `fPtk[]` — no further locking
needed for those.

Why a dedicated thread instead of processing inline in
`_RxFrameReceived`:

1.  `_RxFrameReceived` runs from the USB bulk-IN callback context.
    It must not block on synchronous USB control transfers, but
    programming the chip's CAM (step 11) requires exactly that.
2.  PBKDF2 takes ~1 ms.  Don't want it on the bulk-IN path.

### State stored on the device

`Device.h`:

```c
EapolState   fEapolState;          // Idle / WaitM1 / WaitM3 / Done
uint8        fAnonce[32];          // captured from M1
uint8        fSnonce[32];          // generated for M2
uint8        fPmk[32];             // derived from PBKDF2
bool         fPmkValid;
uint8        fPtk[48];             // KCK || KEK || TK
bool         fPtkValid;
uint64       fM1ReplayCounter;     // echoed in M2

EapolFrame   fEapolInbox;          // single-slot, mutex-protected
bool         fEapolPending;
sem_id       fEapolReady;
```

The inbox is single-slot because the handshake is strictly
one-message-at-a-time per peer and we have only one BSS to talk to.

### Userland: `wpa2_join`

Source: `tools/wpa2_join.c` (when the source layout lands here).

```
usage: wpa2_join <dev> <ssid> <passphrase> [bssid]
   dev:        e.g. /dev/net/rtl8814au/0
   ssid:       1..32 chars
   passphrase: 8..63 ASCII chars
   bssid:      optional aa:bb:cc:dd:ee:ff (zeros = lookup by SSID)
```

The tool opens the device, issues a single `SIOCS80211
IOC_HAIKU_JOIN` with the rich `rtl_haiku_join_psk` struct, then
sleeps to keep the fd open.  Closing the fd would tear down the
RX loop mid-handshake.

### What's not done yet

See [known-issues.md](known-issues.md) for the open work to bring
this from "M2 builds correctly in memory" to a fully connected DHCP
session.
