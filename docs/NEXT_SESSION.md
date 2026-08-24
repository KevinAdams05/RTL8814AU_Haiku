# Next session

This document has grown long and is largely chronological. Read this section
first; the rest is detail and history, and several sections describe faults
that were later diagnosed differently.

**Working.** Both bands, both connection routes, on two adapters. First-join
success on the Edimax is roughly 85% on 2.4 GHz and 81% on 5 GHz. Receive is
solid under sustained load: 100 MB transferred with a matching checksum,
86,000 receive callbacks, zero errors and zero drops.

**Fixed this session, each measured rather than asserted:**

| fix | effect |
|---|---|
| Retransmitted M1 no longer discards the SNonce and PTK | the ~1-in-5 first-join failure; 14 retransmissions survived, 0 MIC mismatches |
| The power-mode H2C is no longer sent -- the vendor never sends it either | 5 GHz went from 31% to 81%, p = 0.0057 |
| Firmware download is retried with a power-on between attempts | one boot in five used to lose the adapter entirely |
| Post-assoc worker survives a transient semaphore error | it could previously die silently and permanently |

**The one open defect.** About one join in six still fails on either band,
always the same way: the association completes, `_DoPostAssocSetup()` never
returns, and the handshake dies. The cause is understood -- an H2C control
transfer that never completes, through a path with no timeout. A bounded
version with retries is **built, staged on the test machine, and not yet
measured**; see "Bounded H2C write, second attempt" below, including the
finding that cancellation cannot reclaim a stuck transfer on this chip.

**Do not trust these two things without re-checking them:**

- Any transmit throughput number. Both test interfaces share a subnet and
  Haiku routes out the wired one, so transmit figures gathered over the air
  are almost certainly measuring gigabit ethernet. Check the interface's own
  `Transmit` counter, which routing cannot fake.
- Any failure rate taken from a handful of runs. Two "regressions" this
  session were the access point dropping off the air and a wedged adapter that
  needed a power cycle, and one hypothesis was built on a channel readback that
  only fires on a band change.

---

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
of the data queue just after association (item 6).** Most runs are clean,
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

---

## Open work, highest value first

### 1. Finish the bounded H2C write

**This is the one open defect**: about one join in six on either band, always
the same way -- the association completes, `_DoPostAssocSetup()` never returns,
the handshake dies. An H2C control transfer never completes and the path has no
timeout.

Built on the in-tree `usb_raw` pattern plus the donor's deadline. Three things
were measured on hardware, and the third changed the design:

**1. The deadline works.** The timeout fired and the driver survived it:

```
post-assoc MEDIA_STATUS_RPT: Operation timed out
post-assoc setup: General system error
```

That is the event which used to be a permanent hang. The worker logged it,
stayed alive, and the next boot associated normally. It also confirms the
underlying fault is real: that control transfer genuinely never completes.

**2. Bounding alone is not enough.** The short soak scored 1 of 4, because a
timed-out command is a command the firmware never receives -- the association
then fails exactly as it did when the call hung, just survivably. Hence the
retry, three attempts, fewer than the reference's ten because each costs the
full deadline and this lock now serialises all register access.

**3. `cancel_queued_requests()` does not reclaim a stuck transfer on this
chip.** This is the important one. Measured: on both timeouts, the cancelled
transfer's callback never arrived within a two-second grace. Every timeout
therefore fell into the hard-fail branch and marked the device unusable, and
**the retry never ran once** -- `retrying control write` appears 0 times
against 2 timeouts and 2 device-kills.

So cancellation is not available as a recovery mechanism here, which rules out
the `usb_raw` shape even though it is the in-tree precedent. The transfer has
to be **abandoned** instead:

- No cancellation at all, which also removes the device-wide hazard, since
  `cancel_queued_requests()` would have aborted other threads' transfers.
- No marking the device unusable. That was far too harsh a response to a
  single failed command.
- A **ring of four buffers**, rotating per attempt. An abandoned request stays
  outstanding and keeps pointing at whichever buffer it was given, so nothing
  may reuse it; without the ring a later transfer could overwrite it and an
  abandoned write could eventually deliver the wrong bytes to a register.
- If an abandoned callback ever does arrive it releases the semaphore, and the
  drain at the top of the next attempt discards that stale count.

#### Measured, 2026-08-25: the mechanism works, the retry does not

12 joins on 5 GHz: **10 ok, 2 deauth, 0 timeout verdicts**. Counts bounded to
this run's boots, because the cumulative syslog includes earlier builds:

| signal | count | reading |
|---|---|---|
| `did not complete within` | 3 | timeouts still happen |
| `retrying control write` | **2** | **the retry path executes** -- the previous build reached it 0 times |
| `succeeded on attempt` | **0** | **a retry never succeeds** |
| `marking the device unusable` | **0** | the hard-fail branch is correctly gone |
| `post-assoc setup: No error` | 10 | |

**What is fixed:** abandoning instead of cancelling does what it was meant to.
The retry is now reachable, the device is never killed over one failed command,
and a timeout no longer necessarily fails the join -- three timeouts occurred
and only one produced a setup error.

**What is not:** the retry is useless. `succeeded on attempt` is zero, which
**confirms the open question rather than answering it**: control requests to
one endpoint queue in order, so a retry issued while the previous transfer is
still stuck sits behind it and gets nowhere. Retrying into a wedged endpoint
cannot work, and no number of attempts will change that.

**The rate did not change.** 2 failures in 12 against 3 in 16 before, Fisher
one-sided p = 0.64 -- indistinguishable. Do not read the 83% as an improvement.

**And the two remaining failures are not this bug:**

- one had `post-assoc setup: No error`, then `M1=4, M2=4, M3=0` and reason 15.
  The access point retransmitted M1 four times, we answered each time, and it
  accepted none of them. Setup succeeded, so the firmware knew about the
  association; something else stops M2 being accepted.
- one had no EAPOL at all and an early reason 2.

So the H2C hang is now survivable but the failures have moved elsewhere. **The
next question is whether the control endpoint ever recovers on its own**, since
if it does not then the command has to be issued another way -- before the
first transfer wedges, or after a pipe reset. `clear_feature(ENDPOINT_HALT)` on
the control pipe is the cheap thing to try next, by analogy with the bulk
pipes.

#### Retry delay: added, and NOT yet exercised

Re-reading the run above changed the diagnosis. "A retry never succeeds" looked
like proof that a stuck endpoint blocks everything queued behind it. But **3
timeouts produced only 1 failed setup**, so in the other cases a later control
write on the same endpoint went through perfectly well. The endpoint is not
wedged; the device just does not answer *instantly*, and the retry loop had no
delay at all -- it asked again at the one moment guaranteed to fail. A 100 ms
pause between attempts was added on that reasoning.

**It has not been tested.** The next 14 boots scored 14 of 14 with **zero
timeouts**, so the retry path never ran. A clean run that never exercises the
fault is not evidence the fix works, and the score should not be quoted as
though it were.

Nor can the change explain the quiet run: **a delay after a timeout cannot
prevent a timeout happening.** Going from roughly 3-in-12 to 0-in-14 is luck or
a change in conditions -- P(0 in 14) is 0.02 at a 25% per-boot rate but 0.23 at
10%, and the rate has never been pinned down.

To settle it, keep running until a timeout appears, then read one line:

- `succeeded on attempt` **> 0** -- the device needed a moment, the retry earns
  its place, and the H2C defect is closed.
- still **0** -- the "needs a moment" reading is wrong too, queue ordering
  stands, and the command has to be issued **before** anything wedges rather
  than retried.

#### A dead end, checked so it is not tried again

Clearing the halt on the control endpoint cannot work. Both
`Device::ClearFeature` and `Pipe::ClearFeature` issue their request through
`fDefaultPipe->SendRequest(...)` -- through the control pipe itself -- so a
wedged control endpoint cannot be cleared by a control transfer on that same
endpoint. `set_configuration()` has the same problem, and Haiku's USB API
exposes no device reset. **There is no driver-level recovery for a genuinely
wedged control endpoint**, which is precisely why the question above matters:
if retrying cannot work, the only remaining option is to avoid wedging it.

**Status: built, style-clean, deployed. The bounded write and the abandon
behaviour are measured; the retry delay is not.**
The abandon-and-retry version has never run. What is known is that the
version before it converts the hang into a survivable error but does not
deliver the command. The open question is whether a retry succeeds while a
previous transfer is still stuck on the control endpoint -- control requests to
one endpoint queue in order, so a wedged transfer may well block the retry
behind it. If it does, the next thing to establish is whether the endpoint ever
recovers on its own, because if it does not then nothing short of a device
reset will help and the command has to be issued some other way.

### 2. Why the first firmware-download attempt fails

The retry works around it, so this is not urgent, but the fault itself is
undiagnosed. **Three plausible explanations have been eliminated by comparing
against the reference**, so nobody needs to spend time on them again:

- **Not the wrong bit.** `CPU_DL_READY` really is `BIT(15)` of
  `REG_8051FW_CTRL_8814A` (0x0080), a flag distinct from `WINTINI_RDY`
  (`BIT(6)`) and added specifically for download-ready. We poll the same bit
  the vendor does.
- **Not too few polls.** The reference's `_FWFreeToGo8814A` allows 100.
  `kFirmwareReadyAttempts` is 100.
- **Not too short a wait.** The reference delays `rtw_mdelay_os(50)` per poll,
  so five seconds in total. `kFirmwareReadyDelay` is 50 ms, so five seconds.
  **Lengthening the timeout is the obvious wrong fix** -- ours already matches.

Decoding the register with the reference's own bit names:

| | `REG_MCUFWDL` | bits 3-6 `MACINI`/`BBINI`/`RFINI`/`WINTINI_RDY` | bit 15 `CPU_DL_READY` |
|---|---|---|---|
| healthy | `0x0060e078` | all four set | set |
| failing | `0x00602000` | **none set** | clear |

So the MCU never signalled a single initialisation stage. It was not starting
slowly; it was not running at all, and the failure is therefore **upstream of
the poll**, somewhere in the download itself.

That is as far as the capture and the source can take it. **The next step is a
diagnostic, not a fix:** read `REG_8051FW_CTRL_8814A` back at each stage of the
download -- after the DMEM section, after IRAM, and after the MCU is released --
so the next failure says which stage did not complete. Cheap, and it turns an
unattributable failure into a located one.

A Haiku restart does not power-cycle USB, so a chip wedged this way stays
wedged across reboots. A run of sudden failures should prompt a power cycle
before a bisect.

### 3. Two H2C commands the vendor sends and we never do

Both fell out of decoding the mailbox writes, and both are pure additions:

| H2C | name | vendor sends |
|---|---|---|
| `0x42` | `RSSI_SETTING` | 13x |
| `0x46` | `RA_MASK_3SS` (marked "for 8814A") | 4x |

**These are not the standalone additions they first appeared to be.** Both are
issued by **phydm**, Realtek's dynamic-management subsystem, which is the part
this driver deliberately does not port -- so there is no small patch that
"adds" them correctly.

The authoritative payload for `0x42`, from `phydm_rssi_monitor.c`:

```c
h2c[0] = mac_id;
h2c[1] = 0;
h2c[2] = rssi;
h2c[3] = is_rx | (stbc_en << 1) | (noisy << 2) | (bf_en << 6);
h2c[4] = (ra_th_ofst & 0x7f) | (ra_ofst_direc << 7);
h2c[5] = 0;
h2c[6] = 0;
```

So sending it means having a **current link RSSI** and somewhere to send it
from periodically. The receive path already computes RSSI per frame
(`phyStatus[1] - 110` in `RxPath.cpp`), but it is only recorded on scan
entries; nothing tracks it for the associated peer. **That makes this the same
task as item 4**, not a precursor to it: the command is the interface to
firmware rate adaptation, and sending it without real RSSI behind it would just
feed the firmware noise.

Caveat on the capture evidence: the command **IDs** are reliable, but the
payload bytes decoded from the mailbox writes are **not**. The decoder reads
each mailbox's extension bytes as whatever was last written there, and its
output disagrees with the reference for `0x42` -- it shows `h2c[1] = 0x8C`
where the reference sets that byte to zero unconditionally. Trust the
reference's layout over `scratchpad/h2c-all.py` until that decoder is fixed.

### 4. Rate adaptation, hardware CCMP, aggregation

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

### 5. Receive throughput

Note the transmit figures below predate the discovery that transmit cannot be
measured over the air on this bench -- see
[testing-notes.md](testing-notes.md). Treat them as unverified.

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

### 6. The data-queue stall -- open, but never reproduced under measurement

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

### 7. Loose ends

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

---

## Standing constraints

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

### Where the rest of the detail went

Method, tooling and the traps that produced wrong answers are in
[testing-notes.md](testing-notes.md) -- read it before trusting any measurement
taken here. Completed work and its evidence is in the
[CHANGELOG](../CHANGELOG.md); the register-level reasoning is in the per-area
docs listed in [docs/README.md](README.md).
