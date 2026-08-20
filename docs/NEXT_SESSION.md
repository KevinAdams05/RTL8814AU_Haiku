# Plan for the next session

Written 2026-08-20, after a TX descriptor audit against the reference driver.
Five field-level bugs came out of it, one of which is confirmed by an
over-the-air capture taken before the audit. None of them could be tested to
completion, because the access point stopped cooperating.

## What the audit found

The comparison was `_BuildDescriptor` against `update_txdesc` in
`hal/rtl8814a/usb/rtl8814au_xmit.c`, with bit positions taken from
`include/rtl8814a_xmit.h` — **not** from the `rtl8812a_xmit.h` and
`rtl8188e_xmit.h` headers beside it, which is how several of these got in.

| Field | Was | Should be | Consequence |
|---|---|---|---|
| `SEQ` | dword 3, bits 16-27 | byte 36, bits 12-23 (and unwritten on this path) | overwrote `USE_MAX_LEN`, `MAX_AGG_NUM`, **`NDPA`**, `AMPDU_MAX_TIME` on every frame |
| `DATA_SHORT` | dword 4, bit 4 | byte 20, bit 4 | bit 4 of the 7-bit `TX_RATE` field — rewrote the rate |
| `SW_DEFINE` bit 0 | never set | set whenever `USE_RATE` is | firmware never told the driver fixed the rate |
| mgmt `DISABLE_FB` + `NAV_USE_HDR` | set | `RETRY_LIMIT_ENABLE` + `DATA_RETRY_LIMIT` | those two belong to the beamforming NDPA branch |
| data `MACID` | forced to 1 | 0 | MACID 1 is `RTW_DEFAULT_MGMT_MACID`, the BMC station |

Two of these are worth understanding rather than just noting.

**`DATA_SHORT` explains the capture.** A CCK 1 Mbps request is `DESC_RATE1M`
= `0x00`. OR in bit 4 and you have `0x10`, which is `DESC_RATEMCS4`. The
capture showed frames leaving as **11n MCS 4 when the driver asked for CCK 1
Mbps**, at -61 dBm from a few inches with 29 retransmissions. That was logged
as an unexplained mystery; it was one misplaced bit, and an 11n rate sent
without the HT context to back it accounts for the poor link too.

**`NDPA` is the one to be angry about.** Bits 22-23 of dword 3 mark a frame as
an HT/VHT null-data-packet announcement for channel sounding. Three of every
four sequence numbers set one of those two bits. So most data frames were
being handed to the chip labelled as beamforming sounding packets.

Also cleared up: there is **no `OWN` bit** on this chip. Bit 31 of dword 0 is
`DISQSELSEQ`; the old Realtek `OWN` name refers to the same bit, from the PCIe
ring descriptor. Both names were defined and both were set, so "try clearing
OWN" was a no-op that looked like an experiment.

## What could not be tested, and why

The descriptor fixes are built, deployed and running. The handshake still does
not complete, but the access point is no longer providing a testable
handshake: across three consecutive boots it associates us (assoc status 0,
AID 1), sends Null Data keep-alives, and **never sends M1 at all**. Earlier
the same access point did send M1. Nothing in the driver's receive path
changed between those runs.

So the current state is: five real bugs fixed on reasoning that holds up
against the reference, and no verdict on whether they were *the* bugs.

One genuinely new observation came out of the failed runs, and it retires a
claim that was load-bearing in two documents: **unicast receive from our own
access point works.** The heartbeat now reports `unicast=10 fromOurAp=10`, and
the frames are the access point's Null Data keep-alives — subtype 4, `len=24`,
header only, unprotected. The old claim that we had "never received one
unicast frame addressed to us" is gone, and `deskbar-to-driver.svg` and
`wpa-supplicant-and-deskbar.md` have been corrected.

## Next session, in order

### 1. Read the register diff, then stop guessing

The single most useful thing built this session is a systematic comparison
rather than another hypothesis. `_InitHardware` ends with a `regdump` of every
4-byte-aligned register in 0x0200-0x05FF that the vendor Linux driver writes,
and `scratchpad/` holds the tooling to diff it:

- `vendor-init.sh` — captures the vendor driver's **complete** register
  transcript over usbmon, from probe through a finished WPA2 handshake. Note
  the driver programs almost nothing at probe: it only reads the EFUSE there,
  and RQPN, EDCA, the queue map and the PHY are all set on **first open**. A
  capture that misses `ip link set up` is worthless.
- `usbmon-regs.py` — decodes Realtek register access out of that capture. The
  chip has no memory-mapped I/O, so every access is a USB control transfer and
  the capture is a complete ordered transcript.
- `analyse-usbmon.py` — pulls out TX descriptors and which endpoint each frame
  went to.

**Registers that still differ**, vendor value against ours, restricted to ones
where all four bytes of the vendor's value were actually observed:

| Register | Vendor | Ours | Name |
|---|---|---|---|
| 0x0420 | 0xFF310F80 | 0x00710F81 | `REG_FWHW_TXQ_CTRL` |
| 0x0428 | 0x30300E0A | 0x20201616 | `REG_SPEC_SIFS` |
| 0x0440 | 0x0080015F | 0x00000FFF | `REG_RRSR` (deliberate: ours is permissive) |
| 0x049C | 0x0600F010 | 0x00000000 | `REG_ARFR4` low |
| 0x04A0 | 0x400003E0 | 0x00000000 | `REG_ARFR4` high |
| 0x04A4 | 0x0600F015 | 0x00000000 | `REG_ARFR5` low |
| 0x04A8 | 0x000000E0 | 0x00000000 | `REG_ARFR5` high |
| 0x04C8 | 0x363608FF | 0x0C1401FF | `REG_PROT_MODE_CTRL` upper bytes |
| 0x0514 | 0x0E0A0E0A | 0x10101010 | `REG_SIFS_CTX` |
| 0x0550 | 0x01001019 | 0x00001414 | `REG_BCN_CTRL` |
| 0x0560 | 0x5D4FEC00 | 0x00000001 | `REG_TSFTR` (free-running, expected) |
| 0x0564 | 0x00000012 | 0x00000000 | — |

`ARFR4`/`ARFR5` being entirely unwritten is the most substantive item left:
the TX descriptor's `RATE_ID` selects among the ARFR tables, and on this chip
they are 64-bit and **not** contiguous — 0x0444, 0x044C, 0x048C, 0x0494,
0x049C, 0x04A4 per `rtl8814a_spec.h`. `hal_com_reg.h`'s 4-byte stride is the
older parts' layout and does not apply.

Ignore these when reading the dump, they are not faults: 0x022C (commit bit
self-clears), 0x0230-0x0240 (the upper half is read-only available-page
count), 0x0204 high byte (firmware-download trigger), and anything marked as
partially observed.

### 2. The blocker, restated precisely

The access point associates us, sends M1 up to five times, never accepts M2,
and deauthenticates with **reason 15** — `4WAY_HANDSHAKE_TIMEOUT`. It is
telling us plainly that it ran the handshake and never got a usable reply.
This reproduces on demand from a clean boot via
`scratchpad/deploy-test.sh <passphrase>`, taking about four minutes.

**The vendor Linux driver completes the same handshake on the same silicon,
the same access point and the same channel** — `PTK=CCMP GTK=CCMP`. So the
chip and the access point are both exonerated and the fault is ours.

### 3. What was fixed this session, and why none of it was enough

Seven distinct defects, each verified against the working driver rather than
guessed, and **the handshake still fails identically after all of them**:

1. Bulk OUT endpoints were backwards — management belonged on 0x02 and data on
   0x04; we sent data to 0x03, which the working driver never touches.
2. EDCA parameters were declared and never written, so the best-effort queue
   could not contend for the medium.
3. TX packet-buffer page counts read a hex `0x20` as decimal `20`, and PUB and
   the boundary were derived from it — our numbers did not even self-add.
4. `REG_AUTO_LLT` and `REG_TXDMA_OFFSET_CHK` were never written.
5. The rate-fallback tables (`ARFR0`/`ARFR1`) and `RRSR` were never programmed,
   with `ARFR1` at the wrong address.
6. `REG_PKT_LIFE_TIME` sat at a finite default, so queued frames could expire.
7. `REG_MACID_SLEEP` came up with bit 1 set — MACID 1, the management and
   broadcast MACID, marked asleep. This very likely explains the old result
   where moving data frames to MACID 1 stopped DHCP working.

That every one of these was real and none was sufficient is itself the
finding: there is no single remaining gate, or the gate is somewhere the
register comparison does not reach — the PHY/RF path, or the frame content
rather than its transmission.

### 4. Decide between "not transmitted" and "refused" before anything else

This is still unresolved and everything else is guesswork without it. The
11:47 capture showed **zero data frames** from us against 3656 from other
stations, but that predates fixes 1-7.

**The air capture is currently unusable.** Later captures came back with
`SA:00:00:00:00:00:00`, empty SSIDs and nonsense addresses — tcpdump
mis-parsing corrupt frames — and shredder absent entirely even when the timing
provably overlapped. The change that broke it is the **Edimax being plugged
into the laptop**, a 4x4 radio inches from its internal antenna. Unplug it
before capturing, or better, use the Edimax itself as the monitor: it is a far
more sensitive receiver and sits beside the machine under test.

Timing, so a window cannot miss again: association lands **T+32 s** after the
join is fired from a clean boot, and the handshake is over by ~T+50 s.
`wifi-capture.sh` now runs 180 s.

### 4. Loose ends worth closing regardless

- **The router never sends M1.** Check whether it wants a WMM/QoS IE in the
  assoc request, and whether stale client state for our MAC is involved.
- **A latent teardown bug.** `B_BAD_VALUE` from `queue_bulk` provably means a
  NULL data pointer, and it appeared once in a wedged state — so slots were
  handed out with freed buffers. Cleanup does `delete[]`; check whether it
  NULLs.
- **`SIOCGIFSTATS` (8929)** is unhandled and floods the log with
  `Control unknown op=0x22e1`.
- **The interface-state bug** leaves an unkillable `ifconfig` after some join
  attempts, which then blocks the next scan. It forces a reboot between tests
  and is the single biggest drag on iteration speed.

## Testing notes that cost real time

- **`grep -a` on shredder's syslog.** It contains binary data, so plain `grep`
  misbehaves and silently mixes boots. Mark the position first with
  `MARK=$(wc -l < /var/log/syslog)` and then `awk -v s=$MARK "NR>s"`. Without
  the mark, lines from three boots ago read as current results — that happened
  again this session and briefly looked like a working handshake.
- **Reboot between join attempts, and wait for the box to actually go down.**
  Checking `uptime` is not enough, and the obvious pattern is actively
  dangerous: `grep -q "up   0:0"` also matches `up 0:01` through `up 0:09`.
  With reboots a few minutes apart it therefore matches the *old* system that
  has not gone down yet, the join fires against a box that is about to
  reboot, and the result reads as a spontaneous crash. This cost three test
  cycles and a bisect hunt for a crash that did not exist. Wait for ping to
  fail first, then for it to come back — `scratchpad/deploy-test.sh` does
  this correctly.
- **Replacing a `.hpkg` does not swap the running driver.** packagefs serves
  the old one until reboot. Verify with
  `strings /boot/system/add-ons/kernel/drivers/bin/rtl8814au`.
- **A scan read immediately after `ifconfig scan` returns nothing** — the
  sweep purges the BSS list and takes ~5.6 s to refill. Sleep 20 s.
- **Never issue an H2C command from `_HardwareInit`.** It blocks device
  initialisation, so the network stack never finishes and the machine boots
  unreachable — ethernet included. Recovery is to unplug the dongle.
- **SSH goes very slow while DHCP is retrying** on the wireless interface.
  The box is fine; give `ConnectTimeout` 60 s and read the log in small
  greps rather than concluding it has wedged.

## Before any release from this branch

The tree carries deliberate diagnostics: deauth reason codes, the unicast
counter in the heartbeat, the per-frame "RX from AP" dump, the ANonce/M2 hex
dump, the `queue_bulk` failure dump, and the TX completion log. All earn their
place while this is open, but the hex dump in particular should go before
shipping. `SPE_RPT` is currently requested on **every** frame, where the
reference asks for it on almost none; that is a diagnostic too and should come
out with the rest. The repo version is still 0.1.1; the `0.1.2~test` bump
exists only on the build server.
