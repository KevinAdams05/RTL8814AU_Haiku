# Plan for the next session

**The blocker is solved.** As of 2026-08-21 the driver works on both bands,
by either connection route, on **two different adapters**: it associates,
completes the WPA2 four-way handshake, installs CCMP keys, obtains a DHCP
lease and passes bidirectional IP traffic.

Current state at a glance:

| | ASUS USB-AC68 | Edimax AC1750 |
|---|---|---|
| 2.4 GHz | works | works |
| 5 GHz | works (54/15 Mbit/s) | associates; not yet carrying traffic |

**There is one open failure and it affects both bands: an intermittent stall
of the data queue just after association (section 7).** Most runs are clean,
which is why it was briefly mistaken for a 5 GHz-only problem. Everything
below was verified end to end, on runs that did not hit it:

| Test | Result |
|---|---|
| Four-way handshake | M1 -> M2 -> M3 -> M4, `CCMP enabled`, keys in the CAM |
| DHCP | lease obtained, `link auto-configured` |
| ICMP from another host | 8/8, 0% loss |
| Ping size sweep (56 - 1472 byte payloads) | 0% loss at every size, no cliff |
| TCP | full SSH session over the wireless interface |
| Throughput | ~2 Mbit/s for an 8 MB transfer |

Reproducible from a clean boot with `scratchpad/deploy-test.sh <passphrase>`
in about four minutes.

## The two fixes that finished it

Everything else was necessary groundwork; these were the last two.

**REG_CR had two bits that do not exist.** The MAC's central command register
was being written with `(1 << 13)` and `(1 << 14)`, named `kCR_EnsecCAMTx` and
`kCR_EnsecCAMRx`. `REG_CR`'s defined bits stop at bit 10 -- bits 16-17 are the
network type and there is nothing at 11-15. So every boot wrote two reserved
bits into it, and the security engine, which is the single bit `ENSEC` at bit
9, was never enabled at all. The vendor driver reaches `0x06FF` here; we
reached `0x64FF`.

**Uplink frames were mislabelled as 802.11 broadcast.** `Write()` derived
`isBroadcast` from the *Ethernet* destination and passed it straight to the
transmit path, where it set the descriptor's `BMC` bit and forced MACID 1. But
`BMC` describes the *802.11 receiver address*, and a station in infrastructure
mode sends every uplink frame to the access point as unicast -- `Address1` is
the BSSID, unconditionally. DHCP DISCOVER has an Ethernet destination of
`ff:ff:ff:ff:ff:ff`, so every attempt went out marked group-addressed: no
acknowledgement expected, group-key handling, wrong MACID. This is why the
handshake could complete and DHCP still fail.

## What to do next, in rough priority order

### 1. Receive: loss is fixed, throughput is the remaining problem

| | Loss | Transmit | Receive |
|---|---|---|---|
| Start of the investigation | 5-20% | 9-27 Mbit/s | 1.0 Mbit/s |
| Now | **0%** | 19 Mbit/s | 1.2 Mbit/s |

**Packet loss is gone**, verified at 0% across three runs of twenty pings, with
the driver's own drop counters reading `drop=0 (walk=0 icv=0)` and CRC errors
at zero. That was the usability problem: a link shedding one packet in ten
stalls page loads and hiccups SSH sessions regardless of its bandwidth.

Getting there ruled out two suspects and found one real bug:

- **Not RF.** CRC errors run at 1 in 7000 frames. Link margin and rate are
  fine, so the fixed OFDM 24 Mbps data rate is not the problem and a rate
  sweep is unnecessary.
- **Not the ICV check.** Splitting the drop counter by reason showed
  `icv=0` throughout. The concern that the chip might flag ICV errors
  spuriously while we decrypt in software was unfounded.
- **The aggregation walk advanced 4 bytes short, which rounding turned into
  8.** `_ParseDescriptor` strips the 4-byte FCS from `packetLength`, which is
  right for delivering a frame and wrong for finding the next one: the chip
  laid the aggregate out using the full on-air length. Because the per-frame
  offset is then rounded up to 8, losing 4 usually landed a whole 8 bytes
  early. For a real frame with `pkt_len` 345 and a 32-byte PHY block,
  `aligned(24+32+341)` is 400 where `aligned(24+32+345)` is 408 -- and 400 was
  exactly what the logs showed. The next descriptor read landed inside the
  previous frame's payload, produced a nonsense length, failed the bounds
  check, and the walk then abandoned **every remaining frame in the
  transfer** while counting only one drop. That under-counting is why 1.2%
  of bails produced 5-20% of loss.

  The reference computes its `pkt_offset` from the raw `pkt_len` and only
  subtracts the FCS afterwards. Now so do we.

**How it was found**, because the technique generalises: dump the raw
descriptor bytes at the failing offset alongside the first descriptor of the
transfer. Frame 1's `dword0` pattern reappeared exactly 8 bytes into where we
had looked, in three independent samples. That turns "the walk is misaligned
somewhere" into "the advance is 8 bytes short", which is a solvable problem.

**What is left: receive throughput.** Currently 2.4-3.1 Mbit/s against 11-21
for transmit, with loss at 0-5%.

The mechanism is understood and measured. Receive throughput is capped by the
*number of bulk-IN transfers per second*, not by anything on the air:

| | transfers/sec | frames/transfer | receive |
|---|---|---|---|
| aggregation threshold 5 pages | 82 | 1.59 | 1.2 Mbit/s |
| threshold raised to 0x20 pages | ~200 | ~1.0 | 2.4-3.1 Mbit/s |

Raising the threshold grew the transfers (sampled lengths went from ~400 bytes
to ~1600) and roughly tripled the transfer rate. The remaining cap is that
**`_RxCallback` resubmits its buffer only after `_ProcessTransfer` returns**,
so each of the four buffers stops receiving while its contents are walked,
CCMP-decrypted and copied into the ring.

Software CCMP is **not** the suspect: transmit does software AES encrypt at
11-21 Mbit/s, so the cipher is not a 3 Mbit/s bottleneck.

**Widening the pipeline is the right move but not naively.** Raising
`kRxTransferCount` from 4 to 12 was tried and the driver would not come up at
all, twice in a row -- no scan results, no association. That is 384 KB of
kernel allocation. The promising version is to shrink `kUsbRxBufferSize`
first: transfers run about 1.6 KB even with the threshold raised, so 32 KB per
buffer is wildly oversized, and twelve 8 KB buffers would be 96 KB against the
128 KB four 32 KB buffers use now -- more pipelining for less memory. Establish
the chip's maximum aggregate size before shrinking, so a large aggregate is
never truncated.

Two other structural candidates, unmeasured:

1. **The RX ring holds 64 slots and silently drops the oldest when full**
   (`Device.cpp`, the enqueue path) -- those drops are counted nowhere. Add a
   counter; if it is non-zero under load, the ring is a second ceiling.
2. **`Read()` returns one frame per call**, and both it and the RX enqueue take
   the device-wide `fLock`, which transmit also takes. Contention on one mutex
   across the whole driver is plausible at these rates.

### 2. Throughput, after that

The remaining limits are known and all deliberate:

- **No rate adaptation.** Every data frame is sent at a hardcoded OFDM
  24 Mbps with `USE_RATE` set, which tells the chip to ignore its own rate
  logic. The `ARFR0`/`ARFR1` tables are now programmed, and `RATE_ID` in the
  descriptor selects among them, so the machinery exists -- what is missing is
  letting the firmware drive it and feeding it link quality.

  Two things to know before starting. `RATE_ID` and the RA_INFO H2C's
  `rate_id` share one 5-bit rate-group namespace, and **both were 8**, which
  is `RATEID_IDX_B`, the CCK-only group -- not the "OFDM" both comments
  claimed. Both are now 12 (`RATEID_IDX_MIX2`), matching the vendor. And the
  RA_INFO rate mask is still deliberately narrower than the vendor's; with
  `USE_RATE` on every frame it is close to inert, but it is the first thing to
  widen when the firmware is allowed to choose.
- **`ARFR4`/`ARFR5` are still unwritten** (0x049C and 0x04A4; note these are
  64-bit and non-contiguous on this chip -- 0x0444, 0x044C, 0x048C, 0x0494,
  0x049C, 0x04A4 per `rtl8814a_spec.h`, *not* the 4-byte stride in
  `hal_com_reg.h`).
- **CCMP runs in software.** The hardware engine is available and the keys are
  already in the CAM; a note in `Write()` records that hardware encrypt was
  tried and produced garbled frames, but that was before a dozen TX-path
  defects were fixed and is worth retrying.
- **A-MPDU aggregation is disabled.** `MAX_AGG_NUM` and `AMPDU_DENSITY` are
  never set and Block-ACK state is not wired up.

### 3. Diagnostics -- done, and deliberately kept

The 0.2.0-era instrumentation was stripped and the version is now 0.3.0.

What remains is bounded and kept on purpose, because it is what a bug report
from someone else's adapter needs to be useful:

- `TX submit` and `TX done`, **budgeted per bulk endpoint** (8 each) rather
  than globally. See the trap in the last section: a single global budget is
  spent entirely on the firmware download.
- One `TXDESC` dump per endpoint -- the ten descriptor dwords and the 802.11
  header. This is what identified the RTS bug, and it is the fastest way to
  compare a failing adapter against a known-good capture.
- The per-band readback (`[band ...] RFE ... RF0x18 ... txpwr ...`).

Anything added beyond this should be removed again before a release.

### 4. REG_HWSEQ_CTRL (0x0423) -- sidestepped

Sidestepped rather than open now. Every non-QoS descriptor used to set
`HWSEQ_EN`, asking the MAC to supply the sequence number — a service this
register never enabled, so **every frame went out as sequence 0**. Rather than
fight the register (both placements tried hung the transmit path),
`TxPath::Transmit` writes a real sequence number into the frame header and
`HWSEQ_EN` is no longer set.

Worth knowing: fixing this did **not** improve throughput, which was the
hypothesis that prompted it. Sequence 0 on every frame is a real protocol
violation and the fix stands on its own, but an access point's duplicate
filtering was evidently not what was costing bandwidth. The constants remain
for anyone who wants the hardware path instead.

### 5. 5 GHz — WORKING on the ASUS as of 2026-08-21 (see section 7)

| Direction | 2.4 GHz | **5 GHz** |
|---|---|---|
| Transmit | 11-32 Mbit/s | **53.9 Mbit/s** |
| Receive | 2.4-3.1 Mbit/s | **15.1 Mbit/s** |
| Loss | 0-5% | **0%** at every payload size |

**5 GHz is much the better band**, and its receive figure being five times
2.4 GHz's reframes the receive investigation above: a good part of that
ceiling was the 2.4 GHz link rate rather than the driver.

Three faults, all in the band switch:

1. **RFE pinmux path D was written only when switching *to* 5 GHz.** After any
   5 GHz excursion -- including the 5 GHz leg of a routine scan sweep -- path D
   kept the 5 GHz routing and 2.4 GHz went deaf, which is why every scan after
   the first returned zero networks until a reboot.
2. **The 5 GHz pinmux values were wrong.** `0x54775477` on all four paths,
   where the vendor writes `0x33173317` on A/B/C and `0x77177717` on D -- so
   the assumption that 5 GHz wants one value everywhere was wrong too. Coex
   `0x1ABC` bits [27:20] are 0x33 for 5 GHz, not 0x54. The register addresses
   were already correct, which is why this looked plausible for so long.
3. **Bit 5 of TX_PATH (0x080C) is band state, not wiring.** The vendor sets it
   for 2.4 GHz and clears it for 5 GHz on one and the same adapter. Leaving it
   set is what made 5 GHz receive perfectly and transmit nothing the access
   point answered. Only that bit is touched; chain selection stays at its
   EFUSE-derived value.

**The lesson worth keeping:** reading `_SwitchBand` did not find any of this,
and I judged the function reasonable -- it was. The post-band-switch readback
did, by showing everything *correct* on channel 149 and so proving the fault
lay in a register the switch never touched. Add the readback earlier next time.

### 6. The Deskbar route — WORKING as of 2026-08-21

Connecting from the Deskbar network menu or the Network preflet works.
`net_server` always delegates a wireless join to `wpa_supplicant`, so
`ifconfig join <ssid> <passphrase>` takes the identical path -- which is how to
test it without touching the GUI.

Three things were missing:

1. **`IEEE80211_IOC_SSID` had no GET handler**, and that alone was fatal. The
   supplicant reads the SSID back immediately after associating; our failure
   made it conclude the association was not real and deauthenticate. Note this
   dispatcher writes the request header back **per-case**, so a GET must
   `user_memcpy(userArgs, &request, ...)` or the caller never sees `i_len`.
2. **The EAPOL diversion was unconditional**, so the supplicant waited forever
   for frames the driver was consuming. Now gated on `fPmkValid`: the
   in-driver path takes its PMK from `IOC_HAIKU_JOIN`, so no PMK means the
   supplicant is driving and the driver stands down.
3. **`IOC_WPAKEY` was a logging stub.** It now installs the keys, buffering
   pairwise and group until both arrive, **and arms the software cipher** --
   programming the CAM alone is not enough when encrypt and decrypt are in
   software, and skipping it gives an association that looks perfect and
   carries nothing.

**A documentation lesson from this one.** The DEAUTH that killed the Deskbar
route was recorded in `wpa-supplicant-and-deskbar.md` for days as net_server
tearing down its own join, complete with a note that it happened even with the
supplicant killed. It was ours all along, and the ioctl log said so as soon as
the unsupported call was visible. A confidently written note in our own docs is
still a hypothesis.

Still genuinely Haiku-side: **open (unencrypted) networks** must go through
net_server, which tears the association down immediately. Not the driver.

### 7. Intermittent: associates, then the data queue stalls (OPEN)

**This is the one open failure, and it is the most important thing in this
document.** After a successful association the best-effort data queue
sometimes stops draining:

```
rtl8814au: TX queue 2 full, waiting
rtl8814au: TX wait timed out on pipe 2
```

repeating indefinitely, with no EAPOL exchange at all -- the stall lands
between `post-assoc RA_INFO: No error` and M1, so the handshake never starts:

```
rtl8814au: ASSOCIATED to 'AdamsFamily02' AID=12
rtl8814au: post-assoc RA_INFO: No error
rtl8814au: TX queue 2 full, waiting
rtl8814au: TX wait timed out on pipe 2
```

**It is intermittent and it is not band-specific.** It was first seen on a
5 GHz join and initially written up here as a 5 GHz problem; it then appeared
on a 2.4 GHz join on a build whose only functional difference was the RA_INFO
`rate_id`, after several consecutive clean 2.4 GHz runs on either side of it.
Treat "2.4 GHz works, 5 GHz stalls" as a wrong characterisation that came from
too few runs -- **the same failure hits both bands, and most runs succeed.**

What is known:

- **The transfers are genuinely outstanding, not leaked.** All four pipe-2
  slots have `inUse` set with no completions arriving. Both paths that can
  fail after claiming a slot clear the flag, and no `queue_bulk failed for TX`
  ever appears, so this is not a slot-accounting bug: the USB bulk OUT
  submissions on endpoint 0x04 are accepted and never complete.
- It is **downstream of association and of the post-assoc H2C**, both of which
  report success.
- Whether it predates the 2026-08-21 changes is **unknown**. It has only ever
  been observed on the Edimax, but that is also the only adapter that has been
  in the machine since the per-pipe logging that makes it legible was added.
- **`ifconfig` on the device hangs unkillably once this happens.** Read
  `/var/log/syslog` instead, and expect an `ssh` that runs `ifconfig` to hang
  until it is killed.

### Measured, 2026-08-22

Two runs, and the honest conclusion is that **the stall is much rarer than
first claimed and neither change has been shown to affect it.**

| Build | Joins | ok | stalled | recovered | other |
|---|---|---|---|---|---|
| no recovery (partial, v1 harness) | 6 | 4 | **1** | n/a | 1 |
| recovery only | 16 | 8 | **0** | 0 | 8 |
| recovery + `TX_HANG_CTRL`, one join per boot | 10 | 8 | **0** | 0 | 2 |

**Zero stalls in 26 joins.** It is tempting to read that as a fix. It is not,
for a reason worth stating plainly: **recovery cannot prevent a stall** -- it
only reclaims the slots after one happens -- and `recovered` is 0, so no stall
occurred for it to act on. Neither change has a mechanism that would produce
0-in-26.

What the arithmetic says instead: if the true rate were the 1-in-6 the first
run suggested, seeing 0 in 26 has probability 0.009. At 1-in-20 it has
probability 0.26. So the first estimate was almost certainly too high --
it came from a handful of `deploy-test.sh` runs, not from a measurement --
and the real rate is low enough that **26 joins cannot distinguish a fix from
chance.** Anyone resuming this should budget for a much larger sample, or find
a way to provoke the stall deliberately, before believing any fix.

`TX_HANG_CTRL` stands on being a confirmed missing initialisation write that
the vendor performs on both bands, not on a measured improvement. Recovery
stands as cheap insurance against a failure that is currently permanent when
it happens. Neither is evidence about the other.

Two further notes on method:

- **The 6 no-assoc results in the middle run are a harness artefact.** They
  cluster after the first attempt in a boot, because re-joining without
  tearing down the existing association fails. Use **one join per boot** for
  anything you intend to compare -- every historical sample was a
  first-join-after-boot. The third run does this.
- **About one first-join in five failed**, across both builds. Chasing that
  found a real bug with a clear mechanism -- a retransmitted EAPOL M1
  restarted the key derivation, so the access point's M3 was verified against
  the wrong PTK and dropped until the handshake timed out. Fixed; see the
  CHANGELOG. Whether it accounts for *all* of the one-in-five is still being
  measured, so do not assume the residue is zero.

A third presentation turned up while measuring, distinct from the stall and
worth its own investigation: an association that comes up with **broadcast
receive working and unicast receive at zero**, so DHCP sends DISCOVER
forever and no reply ever arrives.

```
data RX heartbeat: total=256 protected=256 bcast=256 unicast=0 fromOurAp=0
SW CCMP encrypt OK #4 len=76 ethertype=0806 pn=4
```

Transmit is fine there and nothing stalls; healthy runs show `unicast=4246`.
Whether this shares a root cause with the transmit stall is unknown.

**Before anything else, get a failure rate.** Run `deploy-test.sh` in a loop
and count; every conclusion in this section rests on single runs, which is
exactly how it got mischaracterised the first time. Then, cheapest first:
whether the chip's TX report or queue-status registers show the BE queue
backed up (accepted but not drained implies no free TX pages or a halted
scheduler); whether cancelling and resubmitting the stuck transfers recovers
it, which would separate a chip stall from a USB-stack one; and a vendor
capture across the same window, diffed the way the RTS bug was found.

### 8. Loose ends

- **`SetActivePowerMode()` hangs intermittently.** It is the post-assoc
  worker's first action and issues an H2C command; when it hangs, association
  succeeds and nothing after it runs. It cost a bisect this session because it
  mimics a regression. Needs a timeout and an error path.
- **A latent teardown bug.** `B_BAD_VALUE` from `queue_bulk` provably means a
  NULL data pointer and appeared once in a wedged state, so slots were handed
  out with freed buffers. Cleanup does `delete[]`; check whether it NULLs.
- **`SIOCGIFSTATS` (8929)** is unhandled and floods the log.
- **The interface-state bug** leaves an unkillable `ifconfig` after some join
  attempts, which then blocks the next scan.

## The negative result worth not repeating

A verbatim replay of the vendor driver's MAC initialisation was tried: the
183 writes between the end of its firmware download and the start of its
BB/PHY table, in exact order, then trimmed to 177 to exclude its transition
into the PHY table. **Both versions left data frames untransmitted and
deterministically killed the post-association H2C path** -- the interface
associated, `B_NETWORK_WLAN_JOINED` fired, and nothing after it ran. The MAC
configuration was not the missing piece. Do not spend another afternoon there.

## Reading a capture: the mistake that cost two bugs

Two register/descriptor decisions in this driver were justified in comments as
"what the usbmon capture shows", and **neither survived decoding the bytes**:

- Data frames set `RTS_ENABLE` because "the vendor protects data frames with
  RTS/CTS". It sets it on **0 of 8** data frames, 64 to 1528 bytes. This was
  the Edimax bug: with the bit set the MAC will not transmit until it wins an
  RTS/CTS exchange, so a missing CTS discards the frame inside the chip -- the
  USB write completes, the counter increments, nothing reaches the air.
- `0x0A04` was overwritten with `0x46ff800c`, cited to a specific frame of the
  cold-start trace. The vendor writes that register **four times** and settles
  on `0x45ff800c`, so the override was undoing the last two writes of the
  trace it claimed to follow.

Both read like measured evidence and were really recollections, and the
comments then protected the bugs from review. The same shape of error put two
EFUSE fields at another chip's offsets (`0x00E`/`0x010` instead of
`0x0C9`/`0x0CA`), so every "per this adapter's EFUSE" decision was reasoning
about unrelated bytes.

**Write the throwaway script that prints the claim before making it**, and put
the count or the write sequence in the comment rather than the conclusion, so
a later reader can tell measurement from inference.

## Tooling built this session — worth keeping

The vendor driver on identical silicon is the oracle. A second RTL8814AU (an
Edimax AC1750, `7392:a833`) runs morrownr's `8814au` on the Linux desktop and
completes the same handshake against the same access point, so it can settle
any question about what the hardware wants. The chip has no memory-mapped
I/O -- every register access is a USB control transfer -- so a `usbmon`
capture is a complete ordered transcript of what a working driver does.
Provenance is a black-box observation of hardware, not GPL source.

In `scratchpad/`:

- `deploy-test.sh` — build, deploy, clean reboot, join, report. ~4 minutes.
- `vendor-init.sh` — usbmon capture across a module reload and association.
- `usbmon-regs.py` — decode register access out of a capture.
- `analyse-usbmon.py` — extract TX descriptors and endpoint use.
- `wifi-capture.sh` — over-the-air capture (180 s window).
- `eapol-desc.py` — decode the vendor's **EAPOL TX descriptors** field by
  field (QSEL, MACID, RATE, USE_RATE, SEC_TYPE, PKT_OFFSET, plus the 802.11
  header). This is what found the RTS bug; it is the highest-value tool here
  when a frame is built but never reaches the air.
- `rts-usage.py` — count how many vendor data frames set a given descriptor
  bit, bucketed by frame size. The shape to copy when checking any
  "the vendor always/never does X" claim.
- `preeapol.py` — the register writes in the window *before* the first EAPOL
  transmit, i.e. the post-association setup. Confirmed our EDCA values match
  the vendor's exactly.
- `h2c-decode.py` — decode H2C commands out of the HMEBOX register writes
  (`0x01D0`+4n, ext at `0x01F0`+4n). Gave the vendor's RA_INFO `rate_id`.
  Caveat: its mailbox state-tracking is only reliable for the first command
  it reports; later entries are artifacts, so do not trust them.

**Two traps when capturing the vendor driver:** almost nothing is programmed
at probe (only the EFUSE readout) -- RQPN, EDCA, the queue map and the PHY are
all set on *first open*, so a capture that misses `ip link set up` is useless.
And `rfkill unblock` must come **after** `modprobe`, because reloading
re-creates the device's rfkill switch blocked.

## Testing traps that cost real time

- **Wait for the box to actually go down before testing a new build.**
  `grep -q "up   0:0"` also matches `up 0:01` through `up 0:09`, so with
  reboots minutes apart it matches the *old* system, the join fires against a
  box about to reboot, and the result reads as a spontaneous crash. This cost
  three cycles and a hunt for a crash that did not exist.
- **`grep -a` on shredder's syslog** — it contains binary data, so plain grep
  silently mixes boots. Mark with `MARK=$(wc -l < /var/log/syslog)` then
  `awk -v s=$MARK "NR>s"`. Note the driver's init logging happens *before* any
  mark set after boot.
- **Replacing a `.hpkg` does not swap the running driver** — packagefs serves
  the old one until reboot.
- **A scan read immediately after `ifconfig scan` returns nothing** — the
  sweep purges the BSS list and needs ~5.6 s.
- **Never issue an H2C command from `_HardwareInit`**, and more generally
  beware adding USB control transfers there: 55 extra register reads for a
  diagnostic were enough to break the post-assoc H2C path.
- **Air captures need the Edimax unplugged from the laptop** — a 4x4 radio
  inches from its internal antenna desensitised it enough that captures came
  back full of corrupt frames with the station under test absent entirely.
- **Budget diagnostic logging per endpoint, not globally.** A single counter
  gated on `sLogged < 12` was spent entirely by the firmware download, which
  all goes to pipe 0, so every later completion on the data pipes was
  invisible. "No log line for pipe 2" then reads as "pipe 2 failed" when it
  means "we never looked" -- and that misreading sent a whole round of
  debugging at the USB layer for a bug that was in the descriptor.
- **`len=` in the TX traces is the *total* including the 40-byte
  descriptor.** A 121-byte line is an 81-byte frame, not a 121-byte one.
  Misreading this made the assoc request look like the EAPOL M2 and briefly
  made the queue mapping look wrong when it was correct.
- **A 5 GHz data stall wedges `ifconfig` unkillably** (section 7). Read the
  syslog; an `ssh` that runs `ifconfig` will hang until killed.
- **The syslog spans reboots, so `tail` alone mixes boots.** Checking
  "did the handshake succeed" with `grep ... | tail -2` happily returned the
  *previous* boot's result twice. Bound the window (`tail -25`) or mark it.
