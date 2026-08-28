# Solved defects, and the evidence that settled them

Kept out of `NEXT_SESSION.md` so that document stays a task list, and kept at
all because the reasoning is what stops a solved fault being re-opened on a
hunch. Every entry here was measured, not argued.

## `ifconfig down` hung, and the interface would not come back

**The hang is fixed. Re-registration is only mostly fixed -- see the caveat
below, added 2026-08-28.** `down` returns immediately and joins can be repeated
without rebooting, which is what removed the reboot per test attempt. Three
consecutive down/up cycles: `down` returned in 0 s each time, scanning kept
working (28 networks at baseline, then 26, 23, 26), no stuck processes.

> **Caveat: it still wedges after many cycles.** After roughly 150 down/up
> cycles in one session the interface stops being registered with the stack,
> with exactly the signature this entry was opened for:
>
> ```
> ifconfig <dev>      -> "Interface not found!"
> ifconfig <dev> up   -> "Could not add interface: Name in use"
> ```
>
> The driver is unharmed -- still receiving, beacons counting up, scan sweeps
> completing and notifying -- but the interface cannot be queried, so
> `ifconfig list` returns nothing and every later join attempt fails for an
> unrelated reason. Only a reboot clears it.
>
> It survives a handful of cycles and fails after many, which is why the
> three-cycle verification above passed and why this went unnoticed for days.
> `scripts/air-noreboot.sh` now aborts when it sees this rather than silently
> recording failures. Still open; see `NEXT_SESSION.md`.

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


## Superseded: "the one open defect was an H2C control transfer that never completes"

This was the leading explanation for the intermittent join failure for a long
time, and a bounded-with-retries replacement for the H2C write was built and
staged for it. **It was never confirmed.** The response-timing fault found on
2026-08-25 explains the same symptoms and was measured: frames transmitted 42
times each because the chip never registered the access point's
acknowledgements, and association responses going unacknowledged in the other
direction.

The section is kept in full below because the H2C findings inside it are real
and independently useful -- particularly that a stuck control transfer cannot
be reclaimed by cancellation on this chip, which constrains any retry design.
What is *not* supported is the claim that this was the cause of the join
failures.

### The bounded H2C write, as it was written up

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


## Superseded: the data-queue stall as originally described

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

