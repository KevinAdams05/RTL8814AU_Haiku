>[!NOTE]
>An LLM was used to aid in development of this code.

# In-driver WPA2-PSK

The rtl8814au driver runs the entire WPA2-PSK 4-way handshake **inside
the kernel driver** rather than delegating to `wpa_supplicant`, and
performs AES-CCMP encrypt + decrypt for the data path in software on
the host CPU rather than relying on the chip's hardware crypto engine.

Both of those are unusual choices — most Wi-Fi drivers on every
platform delegate the handshake to userspace and let the chip do
crypto in hardware.  The motivation for each is a concrete bug we
hit and couldn't work around any other way.  This page documents
the design and the rationale.

## Why not `wpa_supplicant`?

Haiku ships `wpa_supplicant 2.11` (HaikuPorts package).  It's a
`BApplication` that net_server launches when an interface joins a
WPA-secured network.  It uses the FreeBSD `driver_bsd.c` backend —
the same one any FreeBSD-derived Wi-Fi driver expects — so it talks
to the driver via `SIOCS80211` / `SIOCG80211` ioctls plus an `AF_LINK`
packet socket bound to ethertype 0x888E for EAPOL frames.

The ioctl side works fine.  `wpa_driver_bsd_init`,
`bsd_set_mediaopt`, `IOC_DEVCAPS`, `IOC_WPA`, `IOC_PRIVACY`,
`IOC_AUTHMODE`, `IOC_DROPUNENCRYPTED`, `IOC_APPIE`, `IOC_MLME` (ASSOC
and DEAUTH), `IOC_DELKEY`, `IOC_COUNTERMEASURES` — wpa_supplicant
successfully drives the driver to associate and the AP issues an AID.

What never works is the EAPOL layer.

### What we proved

A small standalone test program that does exactly what
`l2_packet_haiku.c` does:

```c
int s = socket(AF_LINK, SOCK_DGRAM, 0);
ioctl(s, SIOCGIFADDR, &req);          // -> link addr
((sockaddr_dl*)&req.ifr_addr)->sdl_e_type = htons(0x888E);
bind(s, &linkAddr, sizeof(linkAddr)); // succeeds
recvfrom(s, ...);                      // hangs forever
```

The bind succeeds, the kernel registers our handler in
`interface->receive_funcs` for `B_NET_FRAME_TYPE(IFT_ETHER, 0x888E)`,
the chip receives EAPOL frames, the RX path delivers them to the
network stack, and `ifconfig` increments its RX packet counter.
But `recvfrom` never returns and `tcpdump` (which uses the same path)
sees nothing either.

The frames go missing somewhere between
`device_consumer_thread` enqueuing them and the registered handler
running — ethertype 0x888E doesn't reach userland on Haiku via the
`AF_LINK` path.

Fixing the kernel is a real Haiku bug worth addressing, but it's out
of scope for an unofficial standalone .hpkg driver.  The driver works
around it: run the handshake in-kernel, never let an EAPOL frame
touch userland.

## Why not hardware CCMP?

The RTL8814AU has a hardware crypto engine and a 32-entry security
CAM.  The Linux reference driver uses it; rtwn uses it; every other
Realtek STA driver uses it.  The driver still **programs** the CAM
correctly post-handshake — CAM[1] holds the GTK with the group-key
flag (`BIT(6)`) set, CAM[4] holds the pairwise TK keyed to the AP's
MAC — and verifies the entries via the readback path.

But with the SECCFG register set per morrownr's reference (`0x01CC` =
TxEnable + RxDecEnable + TxBcKeyDef + RxBcKeyDef + CHK_KEYID), the
chip's HW decrypt engine **declines to decrypt** incoming Protected
frames.  Every CCMP-encrypted broadcast we receive comes up with
the `SWDEC` bit set in the RX descriptor (bit 27 of dword 0), which
is the chip telling the host "please decrypt this in software, I
won't."  And with `SCR_RxDecEnable` set the chip then silently
**drops** those same frames after signaling SWDEC, leaving the
host with no way to see them at all.

We tried every relevant variant: BIT(6) group flag, RCR bit-position
fixes (CBSSID_DATA + CBSSID_BCN at the correct 8814au bit positions
6 and 7, not the r92c-era 12 and 13 that some headers list),
REG_MAR opening, atomic BSSID writes, CHK_KEYID, NoSKMC.  The chip
still refused.  Without the Realtek datasheet to consult, we don't
know what register or H2C command makes the engine cooperate.

The fix is to clear `SCR_RxDecEnable` (chip stops silently dropping
encrypted frames and passes them through untouched), then decrypt
in software.  TX side is symmetric — we never trusted HW encrypt
once HW decrypt was clearly broken.  Final SECCFG = `0`; the chip
does no crypto in either direction.

The cost is some CPU time per data frame (AES-128 in software,
~150 ns/block on a modern x86 core).  At 802.11ac line rates that's
noticeable but not catastrophic; the driver is not going to saturate
gigabit wireless on this hardware in any case.  Once we figure out
what the chip wants and HW crypto becomes available, the SW path
can stay as a fallback — the keys are already in CAM, just bring
SECCFG back to `0x01CC`.

## The end-to-end handshake

![wpa2 state machine](diagrams/wpa2-state-machine.svg)

EAPOL frames never leave the driver:

![eapol diversion](diagrams/eapol-diversion.svg)

### Walkthrough

1.  An associated STA brings up WPA2-PSK on the device (via a userland
    helper that opens the device fd and issues
    `SIOCS80211 IOC_HAIKU_JOIN` with a custom `rtl_haiku_join_psk`
    payload of `{bssid, ssid, passphrase}`).
2.  Driver runs `pbkdf2_hmac_sha1(passphrase, ssid, 4096, 32)` to
    derive the 32-byte PMK.
3.  Driver runs `_DoJoin` — sets channel, writes BSSID register,
    drives auth + assoc.  The assoc-req carries an RSN IE synthesized
    in-driver (for the standard AKM 00-0F-AC:2 PSK with CCMP cipher).
4.  AP sends EAPOL-Key M1.  `_RxFrameReceived` notices ethertype
    0x888E and copies the payload into `fEapolInbox` instead of
    pushing it to the data ring (where Haiku's stack would drop
    it).
5.  EAPOL worker thread wakes on `fEapolReady`, parses M1, captures
    the AP's ANonce.
6.  Driver generates SNonce by mixing `system_time +
    real_time_clock_usecs + our MAC + counter` through SHA-1.
7.  Driver derives PTK = `prf_384(PMK, "Pairwise key expansion",
    min/max(AA,SPA) || min/max(ANonce,SNonce))` and splits it into
    `KCK[0..15] || KEK[16..31] || TK[32..47]`.
8.  Driver builds EAPOL-Key M2 with our SNonce + RSN IE in key
    data, computes `MIC = hmac_sha1(KCK, M2_with_MIC_zeroed)[0..15]`,
    patches it into the MIC field, TXes via `fTxPath` as a data
    frame (802.11 header + LLC/SNAP + ethertype 0x888E + EAPOL
    bytes).
9.  AP sends M3 with encrypted GTK in key data.  Driver validates
    the MIC against KCK (hashing exactly `4 + body_length` bytes
    of the EAPOL frame, not the chip-reported `frameLen` — the
    chip's RX length includes the trailing FCS that the AP didn't
    cover in its MIC).  Then unwraps the GTK using
    `aes_unwrap(KEK, ...)` per RFC 3394.
10. Driver builds M4 (no key data, secure bit set in Key Info, MIC
    over the frame with the MIC field zeroed), TXes via `fTxPath`.
11. Driver programs both keys into the chip's security CAM:
    CAM[gtk_keyid] = group key with `BIT(6)` (group flag),
    `BIT(15)` (valid), `ALGO=AES`, MAC=00:00:00:00:00:00 (per the
    rtwn / morrownr convention for default-key entries 0..3); and
    CAM[4] = pairwise TK, `BIT(15)`, `ALGO=AES`, MAC = AP BSSID.
12. Driver leaves `kRegSECCFG = 0` and switches the data path to
    SW CCMP (see below).  `fCcmpEnabled` is set true; `Write()`
    and `_RxFrameReceived` from here on encrypt/decrypt every
    non-EAPOL data frame in software.

After step 12 the connection is fully usable.  DHCP runs over
SW-encrypted frames, gets an OFFER + ACK back over SW-decrypted
frames, and the link is functionally indistinguishable from a
HW-CCMP connection except for CPU cost.

## SW CCMP (`wpa2_crypto::ccmp_encrypt` / `ccmp_decrypt`)

The two functions in `WPA2Crypto.cpp` implement the IEEE 802.11i
CCMP scheme directly (AES-CCM with `M=8`, `L=2`, 13-byte nonce,
22- or 24-byte AAD for non-QoS / QoS data frames).  Both are
straightforward textbook implementations on top of the existing
`aes128_encrypt` primitive — no third-party library.

Details worth knowing:

- **STA always uses the pairwise key for TX**, even when the eth
  destination is broadcast.  At the 802.11 layer the frame is
  unicast to BSSID (Addr1 = BSSID); only the AP needs to decrypt,
  and it does so with the pairwise key.  The AP re-broadcasts to
  other STAs using its own GTK.  Using GTK for our broadcast TX
  was an early bug — AP couldn't decrypt and silently dropped
  every DHCP_DISCOVER.
- **TX PN counter** is per-key and must be strictly monotonic
  for the AP to accept frames; the driver maintains
  `fTxPnPairwise` (used for all outbound) and `fTxPnGroup` (kept
  but unused in STA mode), both reset to `1` each time keys are
  reinstalled.
- **Protected bit must be set in FC[1] BEFORE building AAD.**  Both
  sender and receiver fold the masked FC byte into AES-CBC-MAC, and
  both use Protected=1 in their AAD.  Setting Protected only after
  the MIC is computed produces a MIC the AP can't validate.
- **EAPOL frames stay unencrypted** even after `fCcmpEnabled` is
  set; that's the 802.11i rule — the handshake itself is in clear.

### Crypto primitives (`WPA2Crypto.cpp`)

All implementations are kernel-side, no allocations, no globals
other than const tables.  Compact textbook implementations rather
than vendoring a third-party library.

| Primitive | Purpose | Source |
|---|---|---|
| `sha1` | Used internally by HMAC-SHA1 | RFC 3174 reference |
| `hmac_sha1` | EAPOL-Key MIC, PTK derivation, PBKDF2 | RFC 2104 |
| `pbkdf2_hmac_sha1` | Derive PMK from passphrase + SSID | RFC 2898 §5.2.  4096 iterations per WPA2-PSK |
| `prf_384` | Derive PTK from PMK + nonces + MAC addresses | IEEE 802.11i §8.5.1.1 |
| `aes128_encrypt` / `aes128_decrypt` | AES-128 ECB block cipher | FIPS 197 textbook impl using S-box + inverse S-box (no T-tables — keeps the static-data footprint small) |
| `aes_unwrap` | Decrypt the GTK delivered in M3's key data | RFC 3394 §2.2.2 |
| `ccmp_decrypt` | AES-CCM decrypt for incoming Protected frames | IEEE 802.11-2012 §11.4.4 |
| `ccmp_encrypt` | AES-CCM encrypt for outgoing Protected frames | IEEE 802.11-2012 §11.4.4 |

## Threading

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
2.  PBKDF2 takes ~1 ms.  We don't want it on the bulk-IN path.

SW CCMP encrypt + decrypt do **not** use this thread — they run
inline in `Device::Write` and `Device::_RxFrameReceived` respectively,
since both are bounded work (~150 ns/AES-block) and the cost of a
context-switch would dwarf the win.

## State stored on the device

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

uint8        fGtk[16];             // group key stashed for SW decrypt
uint8        fGtkKeyId;            // group key id from M3 KDE
bool         fGtkValid;

bool         fCcmpEnabled;         // true after step 12
uint64       fTxPnPairwise;        // outbound CCMP PN counter
uint64       fTxPnGroup;           // (unused in STA mode)
```

The inbox is single-slot because the handshake is strictly
one-message-at-a-time per peer and we have only one BSS to talk to.
