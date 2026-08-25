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

Both results hold up. The transmissions-per-frame collapse is unambiguous, and
the failure-rate improvement is significant over two independent 30-attempt
runs (30 with one failure, then 30 with none; Fisher exact p = 0.005 against
the baseline).

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

**The two adapters no longer behave the same, and that is the open problem.**
Joins on 5 GHz, measured the same way on the same access point on 2026-08-25:

| adapter | joins failing | notes |
|---|---|---|
| Edimax AC1750 | **1 / 60** | across two independent 30-attempt runs |
| ASUS USB-AC68 | **~19 / 50** | in bursts of consecutive failures (item 1) |

The chip-level fixes are confirmed on *both* -- the ASUS shows 16 distinct
sequence numbers for 16 frames and 1.1 transmissions per frame against 42.5
before -- so those are not in question. What is in question is why the ASUS
still fails, and whether the difference is the adapter or the conditions: the
ASUS runs were later in the day, and the two were never alternated.

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

### 1. The reason-15 burst on the ASUS -- reproducible, cause unknown

**This is the top open defect and it is now reproducible.** On the ASUS
USB-AC68, joins fail in **bursts of consecutive attempts** that recover on
their own: 15 in a row in one 30-attempt run, 4 in a row in the next
20-attempt run. Roughly 19 failures in 50 attempts on that adapter, against
1 in 60 on the Edimax the same day.

Every failure has an identical signature: `assoc=1 M2=4 ok=0 reason=15`. The
air capture says exactly what happens:

- The station transmits **two frames and no more** -- authentication and the
  association request, both fine, both acknowledged.
- The access point sends **M1 four times** (replay counters 1, 2, 3, 4).
- **No M2 ever reaches the air**, though the driver logs "built M2 (121
  bytes)" and "M2 handed to the chip" four times, and the M1-retransmission
  handling is correct each time ("keeping the existing SNonce and PTK").
- The access point gives up: deauthenticate, reason 15.

So the management queue transmits and the data queue does not, for the
duration of the burst. This is the fault previously filed as "an intermittent
stall of the data queue" and as "an H2C control transfer that never
completes"; neither description was ever confirmed, and the air capture
supersedes both.

**Do not read the earlier run as a permanent latch.** The first ASUS run
failed from attempt 16 to attempt 30 and looked like a latch that never
cleared, but that is only where the run ended -- the next run recovered after
four failures. It is a burst, not a threshold.

**Hypothesis tested, and it does not survive -- but read the limits of the
test.** TX packet-buffer page exhaustion fitted well: a queue out of pages
would accept the USB write and never transmit, and the management queue has
pages of its own. `_DoJoin()` now logs all five `FIFOPAGE_INFO` registers per
join, and across 20 joins spanning a failure burst there is exactly **one
distinct reading**: every queue at its configured count, `hi/lo/nml/ext =
0x20/0x20`, `pub = 0x776/0x776`.

Be precise about what that shows. The registers are read at the **start of each
join**, so they rule out pages leaking *cumulatively across* joins -- the
original hypothesis -- but they cannot see pages consumed *during* a failing
attempt. What actually kills the hypothesis is arithmetic: four 121-byte frames
cannot exhaust a 32-page queue. Reading the pages at the moment of failure is
still worth doing (step 2 below), and this diagnostic does not do it.

### The plan, in order

Reproducing takes about 25 minutes unattended and needs no reboots:

```sh
MON=mon0 HOST=user@<shredder> SSID=<5GHz network> \
    scripts/air-noreboot.sh <passphrase> 20
```

Failed attempts keep their capture; successful ones are discarded. Decode with
`scripts/air-eapol.py <pcap>` and `scripts/seq-check.py <pcap> <our-mac>`.

**Step 0. Establish whether this is a regression. Do this first.** The ASUS was
previously reported at roughly 81% first-join success on 5 GHz; it measured
about 62% today (19 failures in 50). Those figures come from different
harnesses and are not directly comparable -- the older one counted neighbours'
deauthentications as our failures -- so this is *not* evidence of a regression.
But three generic transmit changes landed today and only the Edimax was
measured before and after, so a regression cannot be excluded either, and
everything below is wasted effort if the bug is one we introduced.

Build three packages and run 30 attempts each on the ASUS:

| build | contents |
|---|---|
| A | `5bc2e89` -- everything |
| B | A with the sequence-number commit `febad92` reverted |
| C | `7b742c1` -- lifecycle only, neither transmit fix |

If B and C are clean and A is not, the sequence-number change is implicated,
and there is a specific suspect: **`REG_HWSEQ_CTRL = 0xFF` is written from
`_DoJoin()`, on every single join**, where the vendor driver writes it once
during hardware init. Writing it repeatedly at association time could disturb
the transmit sequencer. It was put there because writes in that part of our
init have wedged the MAC scheduler before -- but the response-timing registers
went into `_InitMAC()` today without trouble, so that part of init is evidently
not the hazardous part. Moving it there, or writing it once behind a flag, is
the first thing to try.

If all three builds show bursts, the bug predates today, and the old 81% figure
was harness artefact.

**Step 1. Make the failing M2 observable.** This is needed whichever way step 0
goes. The per-pipe submit and completion traces cap at 8 per pipe per boot, so
they are silent by the time a burst starts -- which is exactly why "the chip
accepted the write" has never been checked for a *failing* M2. Reset those
counters per join, or add a trace tied to EAPOL transmits specifically.

Know what is already established before adding anything: the driver only logs
"M2 handed to the chip" when `_TxEapolDataFrame()` returned `B_OK`, so a free
transfer slot was found and `queue_bulk()` accepted the frame. But `queue_bulk`
is asynchronous, so `B_OK` means *submitted*, not *completed*. **The completion
status of a failing M2 is genuinely unknown today.** That is the gap to close,
and it splits the problem three ways:

- submitted, USB transfer never completes -> a stall on the pipe carrying the
  BE queue; `_RecoverStalledPipe()` is the code to look at
- submitted, completes cleanly, nothing on the air -> the chip swallowed it;
  go to step 2
- not submitted at all -> contradicts the reading of the code above, so
  recheck the reading

**Step 2. If the chip swallows it, read the chip while it is happening.** Dump
state on the *second* M2 of an attempt, once the failure is already under way,
rather than at join start. Read `REG_TXPAUSE`, `REG_SECCFG`, `REG_CR` and the
`FIFOPAGE_INFO` set. In order of promise:

- **`TXPAUSE`.** It gates transmission per queue, which matches the symptom
  exactly -- management transmits, data does not.
- **Stale security state.** `_DoJoin()`'s own comment records that leftover
  `SECCFG` and CAM state makes the chip mangle our M2 so the access point never
  sees a valid one and the handshake stalls in M1 retries. That is precisely
  this symptom. The per-frame descriptor asks for `kSecurityNone`, so this
  requires the global state to override the descriptor -- check it rather than
  assuming either way.
- **Pages at the moment of failure**, which the current diagnostic cannot see.

**Step 3. Rule out the capture blind spot.** The monitor is 20 MHz on channel
149. Before concluding "no M2 reached the air", confirm a failing attempt has
no frame from our address 2 at *any* rate or type, and ideally capture with a
second monitor at a different bandwidth. A frame sent at a rate the monitor
cannot decode would look identical to a frame never sent, and that mistake has
already been made once in this project.

**Step 4. Compare against the vendor driver on the same adapter.** Two
differences are already visible in its source and worth testing:

- It sends EAP, ARP and DHCP frames with `USE_RATE` at a **fixed low rate** --
  the comment is "use the 1M data rate to send the EAP/ARP packet, this will
  maybe make the handshake smooth" -- where we pass `_LowestBasicRate()`.
- It writes `REG_HWSEQ_CTRL` once at init, as noted in step 0.

**Step 5. Separate the adapter from the conditions.** The ASUS runs were later
in the day than the Edimax runs, so time-varying interference is not excluded.
Alternate the two adapters, or re-run the Edimax, before attributing anything
to the hardware. Needs a physical swap, so it needs Kevin.

### What gates a release

The three fixes are a large improvement and the Edimax is at 1 failure in 60.
But **the ASUS is the primary adapter** -- 0.3.0 was developed on it -- so
shipping 0.4.0 with a roughly 38% join failure there is not viable. This defect
gates the release. Nothing else on this list does.

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
task as item 3**, not a precursor to it: the command is the interface to
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

### 5. Throughput, and why it cannot be measured here

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

### 6. Receive throughput

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
taken here.

**Solved faults, and the reasoning that settled them, are in
[solved-defects.md](solved-defects.md).** That is also where explanations this
document used to lead with have gone once they were superseded -- notably the
long-standing "the one open defect is an H2C control transfer that never
completes", which was never confirmed and which the response-timing fault
explains better. Read it before re-opening anything on a hunch.

Completed work as users see it is in the [CHANGELOG](../CHANGELOG.md); the
register-level reasoning is in the per-area docs listed in
[docs/README.md](README.md).
