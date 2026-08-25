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

### 1. `ifconfig down` and back up -- SOLVED, and it removes the reboot per test

**Both halves are fixed and verified.** `down` returns immediately, the
interface comes back, and joins can be repeated without rebooting. Three
consecutive down/up cycles: `down` returned in 0 s each time, scanning kept
working (28 networks at baseline, then 26, 23, 26 -- ordinary variance), no
stuck processes.

**The hang was a reader thread parked in our own `Read()`.** The stack's
`down_device_interface()` ends with:

```c
device->flags &= ~IFF_UP;
device->module->down(device);
...
wait_for_thread(readerThread, &status);   // never returns
```

Its reader thread sits in `Read()`, which blocked on `fRxDataReady` with no
timeout and nothing to wake it -- that semaphore is released only when a frame
arrives, and by then the receive path has stopped, so no frame ever comes.
Every step of our close hook completed; the hang was downstream, in a reader
*we* had parked and never released.

Fixed with a `fClosing` flag, checked both before blocking and after waking,
and released exactly once per parked reader:

```c
device->fClosing = true;
int32 blocked = atomic_get(&device->fBlockedReaders);
if (device->fRxDataReady >= 0 && blocked > 0)
    release_sem_etc(device->fRxDataReady, blocked, B_DO_NOT_RESCHEDULE);
```

The exact count matters. Releasing a fixed surplus "to be safe" is wrong: a
release nobody consumes is indistinguishable from a frame arriving, so it
strands whatever is actually queued and leaves the reader permanently one
frame behind. `Read()` therefore registers itself in `fBlockedReaders` around
the `acquire_sem_etc`, and nothing needs draining on reopen.

Returning an error here is safe, and the stack source is the reason to believe
it rather than a guess -- `device_reader_thread()` loops on
`while ((device->flags & IFF_UP) != 0)`, and on any error other than
`B_DEVICE_NOT_FOUND` it counts the error, snoozes 10 ms and *retries*. So a
spurious error never makes it give up; only `B_DEVICE_NOT_FOUND` is special,
because that triggers `device_removed()`. `B_DEV_NOT_READY` is correct.

**Correcting an earlier note in this document.** It listed "`Read()` is not
returning an error" under *what has been ruled out*. The observation was true
and the inference from it was wrong: blocking there without returning anything
was the entire fault.

**`TxPath::CancelAll()` self-deadlock was a real bug but not this one.** It
cancelled bulk OUT transfers while holding `fLock`, and XHCI's
`CancelQueuedTransfers` runs each cancelled transfer's callback *inline on the
calling thread*, where `_TxCallback` takes `fLock` again. Haiku's mutexes are
not recursive. Cancelling now happens outside the lock. It was reported as the
cause of the hang on the strength of an invalid test -- `down` run against an
interface that was **already down**, which returns instantly without ever
reaching the close hook. `_RecoverStalledPipe` already dropped the lock before
cancelling for this exact reason, so **check every `cancel_queued_transfers`
call against its callback's locking**; those callbacks are not asynchronous.

**The second bug: close stopped receiving and open never started it again.**
With the hang gone, `down` then `up` gave a live interface that received
nothing at all. Scanning still swept all 42 channels and still fired
`B_NETWORK_WLAN_SCANNED`, but `ifconfig list` came back empty because not one
beacon had arrived. `Close()` calls `fRxPath->Stop()` every time, while
`Start()` is reached only from `_InitHardware()`, which is guarded by
`fHardwareInitialized` and so runs only on the *first* open. A reopened device
sat with no bulk IN transfers submitted. `Open()` now restarts the path if it
is not running; `Start()` is idempotent and resubmits every receive buffer.

This asymmetry had been invisible precisely because `down` hung, so every test
began from a reboot and no reopen was ever exercised.

**What this buys.** Five joins were run back to back with no reboot, and the
fifth reproduced the reason-15 failure that 28 reboot-based attempts never
caught once (item 7). A ~70 s loop now does what used to cost a reboot each
time -- and reboot-heavy runs were themselves manufacturing instability.

### 2. Finish the bounded H2C write

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

#### Settled: retrying a control write does not work, at any delay

Two batches, 30 joins on 5 GHz: **27 ok, 3 failures.** The fault was heavily
exercised the second time -- **12 control-write timeouts, 8 retries, 0
successes.** With a 100 ms pause between attempts, and 0 of 2 without one.

So the queue-ordering explanation is the right one after all: requests to a
control endpoint complete in order, and one issued behind a transfer that is
still stuck simply waits behind it. **No delay or attempt count fixes this**,
and `kControlWriteAttempts` is now 1 -- retrying bought nothing while costing
up to 1.2 s per failed command with a lock held that serialises every register
access in the driver.

The loop is left in the code rather than unpicked, because if a way is ever
found to clear a stuck control endpoint then raising that constant is the
entire change.

Worth noting the timeout is usually survivable anyway: 12 timeouts produced
only 5 failed setups, and 13 of 16 joins succeeded regardless.

#### The dominant failure is now on-air, not in the driver

The remaining failures are all one shape, and it is **not** the H2C bug --
post-association setup succeeds in every case:

```
post-assoc setup: No error
EAPOL M1 ... built M2 ... TX submit pipe=2 len=193 rate=0x04 ... TX done 193/193
   (four times over)
RX DEAUTH reason=15
```

The access point re-sends M1 four times, we answer each, and it accepts none.
Compared against the boots that succeeded, the M2 submissions are **identical**
-- same pipe, same queue, same 193 bytes, same rate 0x04 -- and two successful
boots had a retransmission themselves, so retransmission alone is not fatal.

That means M2 is built and submitted identically whether the join works or not,
and **further driver-side inspection of the M2 path is unlikely to help.** The
frame either is not reaching the access point or is not being accepted for a
reason invisible from this side.

#### Air capture, 28 attempts: the reason-15 mode did not recur

Two runs of monitor-mode capture on channel 149, keeping the capture for any
attempt that was not a clean success. **28 attempts, 25 clean.** The
`M2built=4` reason-15 failure this was built to explain **did not happen once**,
so the question it was meant to answer is still open.

What the captures did settle is a *different* failure, and settle it properly:

```
frames transmitted by our station: 13   -- all mgmt/auth
frames from the AP addressed to us: 5
EAPOL from our station: 0
driver: RX DEAUTH toUs=1 reason=2
```

**Thirteen authentication frames and not one association request.** The
exchange stops at the auth-to-assoc transition, nowhere near the handshake, and
the access point eventually gives up with reason 2. That is a distinct mode
from the reason-15 one and worth chasing on its own -- the driver has clearly
decided to keep re-authenticating rather than proceeding.

Note the driver's own log showed only the deauth for that attempt. **The air
capture is what revealed thirteen transmissions**, which is the case for having
done this at all.

Tooling now in `scratchpad/`: `air-capture.sh` (reboot, capture, join, keep
non-clean attempts) and `air-analyse.py` (parses radiotap and 802.11 directly
and says whether our frames reached the air). Both had to be corrected before
they could be trusted -- see [testing-notes.md](testing-notes.md).

**The reason-15 mode remains the open question**, and catching it needs a longer
unattended loop rather than supervised batches: at roughly 1 in 8 the chance of
missing it across 28 attempts was about 2%, so either it is rarer than measured
-- plausible, since the deauth misfilter inflated earlier rates -- or
conditions have shifted.

**When it is caught, the capture answers it in one line**, not
more code reading -- is M2 on the air at all, and does it look right? Note from
[testing-notes.md](testing-notes.md) that air captures need the Edimax
unplugged from the capturing laptop; a 4x4 radio inches from its antenna
desensitises it enough to produce a capture full of corrupt frames with the
station under test missing entirely.

#### Retry delay: the reasoning that led there, now superseded

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

### 5b. Throughput, and why it still cannot be measured here

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

### 7. The data-queue stall -- open, but never reproduced under measurement

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
