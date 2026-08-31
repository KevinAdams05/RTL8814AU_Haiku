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
| `ifconfig down` wakes readers parked in `Read()` instead of leaving them blocked | `down` returned in 0 s instead of never; no more unkillable process |
| `Open()` restarts the receive path that `Close()` stopped | a reopened interface received nothing at all, so scans listed no networks |
| Frames carry real sequence numbers (`HWSEQ_EN` + `REG_HWSEQ_CTRL`) | every frame had gone out as sequence 0, breaking duplicate detection |
| The MAC response-timing registers are written at last | frames were transmitted **42 times each**; now 1.0. Join failures 20% -> 1.7%, p = 0.005 |

Those last two together **removed the reboot from the test loop**: joins can
now be repeated back to back, and the first five-attempt run reproduced a
failure that 28 reboot-based attempts had never caught.

**The open defect, restated.** About one join in six used to fail, and the
story in this document -- an H2C control transfer that never completes -- was
never confirmed. Two hardware faults found by capturing the air on 2026-08-25
account for the symptoms much better, and both are now fixed:

- **Every frame went out as sequence 0.** 369 consecutive frames, one distinct
  sequence number, where a vendor-driven adapter on the same access point
  numbered its frames 1, 2, 3. The header cannot carry it on this chip -- the
  MAC overwrites Sequence Control on transmit -- so it has to be asked for
  through the descriptor.
- **The MAC was not registering acknowledgements.** Its response-timing
  registers were declared in the source and never written. Every frame was
  transmitted about **42 times** while the access point acknowledged each
  transmission within microseconds. The same timing governs the ACK we send
  back, so association responses went unacknowledged and the access point gave
  up after four tries -- which is exactly what "authenticates but never
  associates" looks like.

Measured, reboot-free, 30 attempts per configuration against the same access
point:

| configuration | join failures | transmissions per frame |
|---|---|---|
| baseline | 6 / 30 (20%) | 42.5 |
| sequence numbers fixed | 6 / 21 (29%) | 42.5 |
| response timing fixed too | **1 / 60 (1.7%)** | **1.0** |

**One of these two results holds up; the other has to be downgraded.**

The transmissions-per-frame collapse, 42.5 to 1.0, is unambiguous and stands. It
is a direct measurement of a mechanism -- count the frames on the air, divide by
the distinct sequence numbers -- not a success rate, so drift cannot touch it.

**The failure-rate column cannot carry the weight it was given.** Those three
configurations were measured *sequentially*, hours and days apart, and
2026-08-28 established that this rate drifts between roughly 10% and 67% on its
own: the same build measured 30% and 13% in two tests, and one adapter went 30%,
60%, 10% across three blocks of one afternoon. A 20%-to-1.7% improvement is
exactly the shape drift produces. The `p = 0.005` was computed against a
baseline taken on a different day, which the test could not know.

**That comparison has now been run, and the answer is no.** Interleaved,
`febad92` (before the fix) against `f6110e4` (immediately after), six blocks of
ten per arm across two sessions:

| | PRETIME | POSTTIME |
|---|---|---|
| pooled | 19 / 60 (32%) | 22 / 60 (37%) |

Fisher exact `p = 0.701`. **The response-timing fix has no measurable effect on
the join failure rate**, in either direction.

So the fix keeps exactly one of its two claimed results: it stops the chip
retransmitting frames the access point has already acknowledged, 42.5
transmissions per frame down to 1.0, which is a direct count off the air. The
`20% -> 1.7%, p = 0.005` figure reported on 2026-08-26 is **withdrawn** -- it came
from comparing two sequential runs, and it does not reproduce.

**And that is informative about the defect itself:** removing 97% of the
retransmissions changed the join failure rate not at all, so whatever is
breaking these joins is not an airtime or retransmission problem.

**One methodological point, because it nearly produced another wrong answer.**
The first three-round run gave PRETIME 10/30 against POSTTIME 18/30, `p = 0.069`
-- suggesting the fix was *harmful*. The second three-round run gave 9/30 against
4/30, suggesting it helped. Pooled, they cancel. **Three rounds of ten per arm is
not enough here**; six was. A single interleaved comparison can still mislead
when block-to-block noise runs from 0/10 to 9/10.

**The sequence-number fix alone changed nothing measurable**, which is worth
recording: it is a genuine bug, verified fixed on the air, and it was not what
was breaking joins.

**Failure rates in this document are overstated.** The harness counted any
`RX DEAUTH` in the syslog as our failure, and the driver logs deauths it merely
overhears -- a neighbouring access point deauthenticating its own client was
recorded as a failed join. Fixed to require `toUs=1`, but every rate measured
before that is inflated by an unknown amount. See
[testing-notes.md](testing-notes.md).

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
| 5 GHz | works (54/15 Mbit/s) | **works** -- 59 of 60 joins, WPA2-CCMP, DHCP lease over the air (2026-08-25) |

**The two adapters behave the same.** Settled 2026-08-28 by an interleaved
comparison on one access point -- three rounds of ten attempts each, with a
physical swap between blocks so both met the same conditions:

| round | ASUS | Edimax |
|---|---|---|
| 1 | 3 / 10 | 4 / 10 |
| 2 | 6 / 10 | 1 / 10 |
| 3 | 1 / 10 | 2 / 10 |
| **pooled** | **10 / 30 (33%)** | **7 / 30 (23%)** |

Fisher exact `p = 0.567`. The within-round winner flips twice. **So the earlier
reading -- Edimax 1/60 against ASUS ~15%, taken hours apart -- was an artefact of
when each was measured, and "the Edimax is the good adapter" is withdrawn.** The
absolute rates in that comparison are inflated for both, because it ran on the
instrumented build (see below), but it compared like with like.

**Absolute rates here are close to meaningless; only interleaved comparisons
carry information.** Same build, same adapter, same access point, measured
twice: 30% and 13%. One adapter across three blocks of a single afternoon: 30%,
60%, 10%. Any claim in this project's history that rests on comparing two
sequential runs is unsafe, including several that were made confidently.

**And the instrumentation was making it worse.** A per-M2 register readback
added on 2026-08-28 put nine synchronous USB control transfers on the EAPOL
critical path and took failures from 30% to 67% (`p = 0.009`). Removed in
`e4d7db0`, and removal was sufficient: against the build that had measured 1 in
60, the cleaned-up build is 4/30 against 2/30, `p = 0.671`. Full account in
[testing-notes.md](testing-notes.md).

The chip-level fixes are confirmed on *both* adapters -- the ASUS shows 16
distinct sequence numbers for 16 frames and 1.1 transmissions per frame against
42.5 before -- so those are not in question.

**Everything below was verified end to end**, on runs that did not hit the
failure:

| Test | Result |
|---|---|
| Four-way handshake | M1 -> M2 -> M3 -> M4, `CCMP enabled`, keys in the CAM |
| DHCP | lease obtained, `link auto-configured` |
| ICMP from another host | 8/8, 0% loss |
| Ping size sweep (56 - 1472 byte payloads) | 0% loss at every size, no cliff |
| TCP | full SSH session over the wireless interface |
| Throughput | ~2 Mbit/s for an 8 MB transfer |

Reproducible without any reboot, about 25 minutes for 20 attempts:

```sh
MON=mon0 HOST=user@<shredder> SSID=<5GHz network> \
    scripts/air-noreboot.sh <passphrase> 20
```

---

## Open work, highest value first

### 1. The reason-15 failure -- pre-existing, NOT a regression

**Read this before concluding anything has got worse.** The rates measured
against this defect look alarming next to nothing, and this document spent days
quoting them without a baseline. The baseline is in the 0.3.0 CHANGELOG, under
Known limitations, written before any of the 2026-08-25 work:

> "About one join in six still fails on either band"

One in six is **17%**. The ASUS measures **~15%** now. The signature was recorded
there too -- "the four-way handshake stalled at M2 with the access point
re-sending M1 four times and giving up with a reason-15 timeout" -- which is
`assoc=1 M2=4 reason=15`, exactly what is still being chased. There is no record
of either adapter ever measuring better than about 85%.

So nothing regressed. What changed is that it became **visible**: every attempt
used to cost a reboot, so nobody ran thirty back to back, and a 1-in-6 defect
rarely bit inside a short supervised batch. A 35-minute reboot-free run now
surfaces four or five failures, which reads as a broken adapter.

Two things did move, both forward:

- **The Edimax improved from 20% to 1.7%** (p = 0.005) with the response-timing
  fix -- better than the historical 15-19% either adapter ever had.
- **The regression question was tested, not assumed.** Interleaved A/B/D:
  build B with the sequence-number change *disabled* failed 4/18, against 2/18
  with it enabled. Removing the change made it worse.

**Always quote a measured rate against the 17% baseline**, and say which build
and which hour it came from. Doing otherwise is what made this look like a
regression -- and then hid a real one.

**The rates in this document taken between 2026-08-28's diagnostics and their
removal are inflated** and should not be compared with anything. See
testing-notes.md: nine synchronous control transfers on the EAPOL critical path
took failures from 30% to 67%, `p = 0.009`.

**Absolute rates are close to meaningless here; only interleaved comparisons
are.** Same build, same adapter, same access point, measured twice: 30% and 13%.
One adapter across three blocks of one afternoon: 30%, 60%, 10%.

### Where it stands

The chip accepts a well-formed 193-byte M2, reports a successful USB
completion, is on the right channel, is not paused, is not encrypting, has
pages free, drains its transmit queue -- and never puts the frame on the air.
It then **counts it as dropped**. Management frames on the same queue and
endpoint transmit fine, so the discriminator is frame type, not the queue.

Roughly fifteen candidate causes are dead by measurement, and the full record --
with what each cost and why it failed -- is in
[reason-15-investigation.md](reason-15-investigation.md). Read it before
proposing anything; the most likely way to lose a session here is to re-run an
experiment that has already been done.

**The cheap instrument:** `0x04EC` is `REG_DROP_PKT_NUM` on this chip and
discriminates perfectly -- it rises once per failing M2 and does not move across
a dozen successes. Detecting the fault needs no air capture. **But read it off
the critical path**: nine synchronous control transfers next to the handshake
took failures from 30% to 67%.

### What to do next

1. **Rerun the access-point comparison.** Both previous attempts are void --
   one on a wedged interface, one because a connect used to blind later scans
   (both now fixed). It is the only variable left that is plausible and cheap:
   every measurement of this defect has been against one network on one access
   point. `scripts/air-noreboot.sh` alternates SSIDs with no reboot.
2. **Test whether the response-timing fix actually helped the join rate**, by
   interleaving against the pre-fix build. Its airtime effect is proven; its
   rate effect rests on a sequential comparison and is unproven.
3. **Fix the interface wedge (item 2)** before any long run, since it silently
   ends the usefulness of everything after it.

**Do not** chase C2H, the drop reason, the two missing H2C commands, or air
contention as causes: all four are settled in the investigation record.

### What gates a release

The three fixes are a large improvement and the Edimax is at 1 failure in 60.
But **the ASUS is the primary adapter** -- 0.3.0 was developed on it -- so
shipping 0.4.0 with a roughly 38% join failure there is not viable. This defect
gates the release. Nothing else on this list does.

### 2. The interface wedges after many down/up cycles

Reopened 2026-08-28. Most of the lifecycle fix holds -- `down` returns, the
interface comes back, testing needs no reboot per attempt -- but after roughly
150 cycles in one session the interface stops being registered with the stack:

```
ifconfig <dev>      -> "Interface not found!"
ifconfig <dev> up   -> "Could not add interface: Name in use"
```

The driver is unharmed: still receiving, beacons counting up, scan sweeps
completing and firing their notification. It is the stack-side registration that
is lost, so `ifconfig list` returns nothing and every later join attempt fails
for an unrelated reason. Only a reboot clears it.

**Why it matters beyond tidiness:** it silently inflates any failure rate
measured across a long run, and it has already produced one false finding -- an
access-point comparison that returned "0 of 4" for one network, which was really
that SSID having dropped out of the scan list on a wedged interface.
`scripts/air-noreboot.sh` now checks before each attempt and aborts.

**Where to start:** the close path releases readers and stops the receive path,
and `Open()` restarts it -- all verified. What is not verified is what the *stack*
does across many cycles: `up_device_interface()` only respawns its reader thread
when `up_count` is zero, and `interface_protocol_down()` decrements it, so a
count that drifts by one would produce exactly this. Instrument `fOpenCount`
against the stack's own up/down calls over many cycles and look for the drift.

### 3. Why the first firmware-download attempt fails

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

### 4. Two H2C commands the vendor sends and we never do

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

### 5. Rate adaptation, hardware CCMP, aggregation

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

### 6. Throughput, and why it cannot be measured here

The response-timing fix cut transmissions per frame from 42.5 to 1.0, so
throughput should have improved substantially. It has not been measured,
because there is still nowhere to measure it to.

`Adams-Guest` looked ideal: it is a **separate subnet, 192.168.20.0/24**, so
routing cannot divert its traffic out of the wired interface the way it does on
the shared subnet. DHCP over the air works and shredder gets 192.168.20.16.
But the network blocks client traffic -- neither shredder nor a second adapter
on the same network can ping the gateway or each other.

**That was checked against the vendor driver, and it fails there too**, so it
is the network's policy and not ours. Worth remembering as the general
technique: when a link-level test fails, run the same test with the vendor
driver on the same network before spending any time on the driver.

What would actually work, in order of preference:

1. **The main 5 GHz network** (`AdamsFamily02-5G`), which is a normal subnet
   with reachable hosts. Needs its passphrase; it has never been recorded, only
   supplied for individual runs.
2. **Drop the wired interface** over IPMI serial-over-LAN, so routing has no
   alternative. The BMC and SOL setup are already documented.

Either way, read the interface's own `Transmit` counter as well as any
timing figure -- routing cannot fake the counter, and every transmit number in
this document's history was suspect for exactly that reason.

### 7. Receive throughput

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

### 8. The KDL -- recurring, unattributed, and capturable next time

The machine dropped into KDL on 2026-08-31 and again at least once earlier in
this driver's history. **Both times the cause went unrecorded**, and that is the
part worth fixing rather than the crash itself.

What is known about the 2026-08-31 one:

- It happened **mid-scan**. The last driver lines before the boot banner are
  `BSS +` entries being added on 2.4 GHz channels 1 and 6, `data RX heartbeat`,
  and `RX cb #4096` -- so the receive path and the scan were both active.
- **It was not a boot failure and not deterministic.** The same build had booted
  and run 30 attempts an hour earlier, and booted fine again afterwards. Its
  driver source is byte-identical to a build with dozens of clean boots.
- The receive walk and the BSS-list update were both reviewed for bounds faults
  and both are sound: the walk bails on `payloadOffset + payloadLength > length`
  and breaks on zero-length padding, and the PHY-status read is provably inside
  that bound; SSID and IE copies are clamped.

**Why the cause was not captured, and how to fix that permanently.** KDL output
goes to the console, not the syslog. There is no `previous_syslog` on this
machine. But `serial_debug_output true` and `serial_debug_port 0x2f8` are
**already set** -- COM2 is the SOL UART on this board -- so a serial-over-LAN
capture would record the panic text automatically. The only obstacle is that the
BMC's IPMI-over-LAN stops answering periodically and needs a hard reset; it was
dead when this crash happened.

**So: before any long unattended run, check that SOL is alive.** A crash with a
serial capture is a diagnosable bug; a crash without one costs a session and
teaches nothing, which is what happened here twice.

### 9. Loose ends

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
taken here.

**The open defect's full investigation record is in
[reason-15-investigation.md](reason-15-investigation.md)** -- every hypothesis
measured and killed, and what each cost. Read it before proposing an experiment.

**Solved faults, and the reasoning that settled them, are in
[solved-defects.md](solved-defects.md).** That is also where explanations this
document used to lead with have gone once they were superseded -- notably the
long-standing "the one open defect is an H2C control transfer that never
completes", which was never confirmed and which the response-timing fault
explains better. Read it before re-opening anything on a hunch.

Completed work as users see it is in the [CHANGELOG](../CHANGELOG.md); the
register-level reasoning is in the per-area docs listed in
[docs/README.md](README.md).
