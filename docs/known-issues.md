>[!NOTE]
>An LLM was used to aid in development of this code.

# Known issues and roadmap

A snapshot of where the driver stands and what's between here and a
1.0 release.

## What works

- Bring-up: USB enumeration, EFUSE read, MAC initialization, firmware
  load via IDDMA, BB / RF / AGC table replay.
- Scanning: `ifconfig … scan` returns full BSS list with SSIDs,
  signal strengths, and security types.
- Open-network association + DHCP + ping.  Verified end-to-end on
  ADAMS-Guest (when temporarily set to open) — full Internet
  connectivity over the air.
- WPA2-PSK on-air association.  AP accepts our assoc-req with the
  right RSN IE and issues an AID.
- In-driver WPA2 4-way handshake skeleton: PBKDF2 → PMK → ANonce
  capture → SNonce generation → PTK derivation → M2 frame build with
  valid HMAC-SHA1 MIC.

## What's not done yet

In rough order of how to tackle them.  Each item is a few hundred
lines or less of focused work.

### 1. Synthesize the RSN IE in-driver

When the rich `IOC_HAIKU_JOIN` runs (driven by `wpa2_join`,
not wpa_supplicant), `fWpaIe` is empty — `wpa_supplicant`'s
`IOC_APPIE` is what normally fills it.  As a result the assoc-req
goes out with no RSN IE and the AP accepts us as an open client
even though it advertises WPA2.

Fix: when `fPmkValid` and `fWpaIeLength == 0`, build the standard
22-byte RSN IE for AKM 00-0F-AC:2 with CCMP cipher:

```
30 14 01 00          element-id=48 len=20 version=1
00 0F AC 04          group cipher = CCMP
01 00 00 0F AC 04    pairwise count=1, CCMP
01 00 00 0F AC 02    AKM count=1, PSK
00 00                RSN capabilities
```

Splice into `fWpaIe` from the `IOC_HAIKU_JOIN` rich-struct path.

### 2. Suppress external `IOC_MLME DEAUTH` during in-driver WPA2

After our successful ASSOC, something — probably net_server's
auth-mode reconciliation — sends `IOC_MLME` op=DEAUTH and tears
down the connection before the handshake can complete.

Fix: in the `IOC_MLME` DEAUTH/DISASSOC branch, ignore the call when
`fPmkValid && fEapolState != kEapolDone` (handshake in progress;
external actors should not interfere).  Once `fEapolState == kEapolDone`
the connection is fully ours and DEAUTH means "user wants to leave."

### 3. Actually TX M2 via `fTxPath`

The M2 frame is built but not yet sent.  Wrap it in an 802.11 data
frame:

```
FC byte 0 = 0x08    (type=Data, subtype=0)
FC byte 1 = 0x01    (ToDS=1, FromDS=0, Protected=0 -- EAPOL pre-keys)
Duration = 0x003A
Addr1 = BSSID       (where the AP's MAC actually receives)
Addr2 = our MAC
Addr3 = AP MAC
SeqCtrl = 0
LLC/SNAP = AA AA 03 00 00 00 88 8E
EAPOL bytes
```

`fTxPath->Transmit(frame, len, kTxQueueBE, 0, 0, kSecurityNone, false)`
should be the right invocation.  Check ASSOC code path for the
parameter pattern.

### 4. Process M3, build + TX M4

Once M2 is on the air, the AP should respond with M3.  The state
machine's `kEapolWaitM3` branch is the receive-side stub.  Steps:

- Validate M3's MIC: `hmac_sha1(KCK, M3-with-mic-zeroed)[0..15]`
  must match the MIC field.
- The key data field is encrypted.  Decrypt with
  `aes_unwrap(KEK, m3.key_data, m3.key_data_len, plaintext)`.
- Walk the unwrapped key data for the GTK KDE: type 0x00 0x0F 0xAC 0x01
  followed by the GTK length and bytes.
- Build M4: similar to M2 but with no key data, secure bit set in
  Key Info, MIC computed over the whole frame.
- TX M4 via `fTxPath`.

### 5. Program PTK + GTK into chip security CAM

The Realtek 8814AU has a 32-entry security CAM.  Each entry holds
a 16-byte key plus metadata (key type, MAC address, key index).
Programming sequence (rtwn-style, mirrored from the morrownr trace):

```
For each (slot, key_data, key_macaddr, key_type) {
    REG_CAMWRITE = key_data[0..3];
    REG_CAMCMD = SLOT | OFFSET(0) | WRITE | POLL;
    poll until !POLL;
    ... repeat for the rest of the CAM entry layout ...
}
```

Slot 0 = pairwise key (PTK's TK[32..47]) for MACID 0 (the AP).
Slot 1 = group key (GTK).

### 6. Enable CCMP in `kRegSECCFG`

After CAM keys are loaded, flip the `kRegSECCFG` bits to enable
hardware AES-CCMP encrypt + decrypt.  The exact bits are in
the Realtek MAC reference; rtwn has the recipe.

### 7. End-to-end verification

Run `wpa2_join` against ADAMS-Guest with WPA2-PSK enabled.  Should
get:

- `EAPOL state -> Done`
- DHCP completes (now over encrypted air)
- Pings succeed

When that works, we have a 1.0 candidate.

## Hardware testing

Currently verified only on:

- ASUS USB-AC68 (`0b05:1817`)

The Edimax EW-7833UAC (`7392:a833`) uses the same RTL8814AU chip
and should work identically — needs verification.

Other potential RTL8814AU dongles that should work but haven't been
tested:

- Alfa AWUS1900 (`0bda:8813` per some refs — chip ID variant?)
- Various OEM dongles

If you have one, please test and file a bug report (with `listdev`
output) — see the README's "Logging Bugs / How to Help" section.

## Distribution

The release plan is a standalone `.hpkg` published on this repo.
Open work to get there:

1.  **Move the source into this repo.**  Currently lives in our
    Haiku source tree.  Once the WPA2 path is complete and stable,
    relocate to `C:\Code\Haiku\rtl8814au\src\` (mirror in this repo)
    and have it build standalone.
2.  **Standalone build script.**  A small Jamfile or shell script
    that builds against an installed Haiku SDK without needing the
    full Haiku source.
3.  **Firmware redistribution.**  `rtl8814aufw.bin` is Realtek's
    firmware blob, redistribution license to be confirmed.  Worst
    case the .hpkg downloads it from a known mirror at install time.
4.  **HaikuPorts submission (later).**  Once stable, submitting a
    HaikuPorts recipe makes the driver `pkgman install`-able.
5.  **Upstream submission to Haiku itself (much later, optional).**
    The unofficial fork stays as the development tip; merging to
    Haiku proper is a stretch goal that depends on community
    interest and review.

## Diagnostic noise to clean up before 1.0

Per `feedback_documentation`-style cleanup:

- The diagnostic `dprintf` lines for `IOC_MLME stub`, the per-frame
  EAPOL hex-dump, etc. should be gated behind a debug-only define
  in production builds.  They're invaluable during bring-up but
  noisy in normal operation.
- Some unused tables in `PhyRegTables.h` (the hand-derived BB/AGC
  values from before the morrownr replay landed) are still in the
  source for reference.  Remove once the replay path is bulletproof.
- The `_DumpRxState` helper is currently unused.  Either wire it
  into a /sysfs-equivalent or delete.
