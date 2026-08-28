# The reason-15 failure: investigation record

The open defect, and everything measured against it. Kept out of
`NEXT_SESSION.md` so that stays a task list, and kept at all because the
eliminated hypotheses are the expensive part -- each one cost a build, a deploy
and a measurement run, and re-running them is the easiest way to waste a
session.

**Read the item in [NEXT_SESSION.md](NEXT_SESSION.md) first** for where this
stands and what to do next. This is the detail behind it.

**One rule before any number here is used:** absolute failure rates in this
project are close to meaningless. The same build, same adapter and same access
point has measured 30% and 13% in two tests, and one adapter went 30%, 60%, 10%
across three blocks of a single afternoon. Only interleaved comparisons carry
information. See [testing-notes.md](testing-notes.md).

### The failure itself -- reproducible, cause unknown

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

### Step 0 is done: it is NOT a regression from the sequence-number fix

Measured 2026-08-26/27, **interleaved** in six-attempt blocks over three rounds
so all three builds met the same conditions:

| build | contents | joins failing |
|---|---|---|
| A | everything | 2 / 18 (11%) |
| B | A with the sequence-number mechanism disabled | **4 / 18 (22%)** |
| D | A with `REG_HWSEQ_CTRL` written once at init | 0 / 18 (0%) |

**B is worse than A, not better.** Removing the sequence-number fix did not
remove the failure, so it is not something we introduced. Everything else in
this item is therefore worth pursuing.

No pair is statistically significant (best is B vs D at p = 0.10; A vs D is
p = 0.49), because conditions were unusually good: build A failed 11% here
against 40% for the *same build* the previous evening. That drift is exactly
why the comparison had to be interleaved, and it is also what cost it the power
to say more.

`REG_HWSEQ_CTRL` has been moved to `_InitMAC()` regardless -- it matches the
reference driver and writes a set-once register once -- but **not** on the
strength of 0 versus 2 failures.

**A longer A-versus-D run is the cheapest open question**: with failure rates
around 10%, separating 0% from 11% needs roughly 60 attempts per arm, about
four hours interleaved and unattended.

**Step 0 as originally written, kept for the method.** The ASUS was
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

### What the instrumentation settled on 2026-08-26

**Nothing the driver can observe distinguishes a failing M2 from one that
works.** Measured at the moment of a failing M2, across three failing joins
against fifteen successful ones, then confirmed on a further run:

| checked | failing value | verdict |
|---|---|---|
| `TXPAUSE` | 0x00 | nothing paused — the leading suspect, dead |
| `SECCFG` | 0x00 | chip is not encrypting; stale-key theory does not apply |
| pages, at the failing M2 | `nml 0020/0020`, `pub 0776/0776` | never short, now sampled at the right moment |
| USB completion | `status=No error actual=193/193` | the transfer completes in full |
| descriptor dwords | `84280099 000c0000 00010000 00000100 00000004 … 00008000` | **byte-identical** to a working join |
| frame header | `08 01 00 00 <bssid> <us> <ap> 00 00` | identical |
| RF channel, from RF 0x18 | `0x53195`, ch 149 | correct, and the cached value agrees |

So the chip accepts a well-formed 193-byte frame on the right channel, reports
success, and never transmits it.

**A persuasive hypothesis died here, recorded so it is not re-run.** In the
first two samples the failure correlated with H2C `MEDIA_STATUS_RPT`
*succeeding*, and success with it timing out -- which would have echoed the
earlier finding that removing the power-mode H2C fixed 5 GHz. Across all 18
joins it does not hold: that H2C succeeded in 17 of them, 14 of which joined
fine.

### Step 3 is done: the M2 is genuinely not transmitted (2026-08-27)

Confirmed with a control, on an independently rebuilt monitor setup (`wlo1`
switched to type monitor on iwlwifi -- see below), against the current build:

| observation | value |
|---|---|
| frames our station transmitted, whole attempt | **2** -- auth (seq 1945), assoc-req (seq 1946) |
| data frames from us | **0** |
| frames in the capture | 9129 |
| the access point's M1 retries, to our MAC | **5**, spanning 2.1 s |
| ACKs addressed to us | 2 -- one per frame we sent |

The control is what matters. The monitor was demonstrably live and on-channel
*throughout the M2 window*, because it captured the access point's five M1
retries to our own MAC in that window, and the two ACKs the access point sent
for our auth and association request. A frame sent at a rate the monitor could
not decode would have looked identical to a frame never sent; that is now
excluded.

Sequence numbers increment (1945, 1946) and there are no retransmissions, so
both of the 2026-08-25 fixes are working on this adapter.

**And the chip is not holding the frame either.** `ReadTxQueueEmpty()` existed
in the tree, unused, with a docstring describing exactly this question. Wired
into the per-M2 readback, it reads `qempty=0x0fff` -- every queue drained -- on
all four M2s of a failing attempt, identical to a successful join.

So the chip accepts a well-formed 193-byte frame, reports a successful USB
completion, is on the right channel, is not paused, is not encrypting, has
pages free, **drains its transmit queue**, and the frame never reaches the air.
It is consumed and discarded, not buffered and not stuck.

**That kills the whole "on the air but rejected" branch** -- content, MIC, and
the replay counter, which was separately dead by inspection anyway
(`fM1ReplayCounter` is re-read from every M1 before the repeat branch, so a
resent M2 carries the right counter).

### The monitor setup that works, and the one that must not be repeated

**Do not plug an RTL8814AU into the capture host for monitor mode.** The
out-of-tree `8814au` driver deadlocks in `cfg80211_rtw_add_virtual_intf` on
kernel 7.0.0 -- the call that creates a monitor interface. Hung-task traces,
then `iw`, `ip`, a udev worker, `wpa_supplicant` and a kworker all stuck
unkillable in D state. Only a reboot clears it. This cost a bench reboot.

**What works** is changing the built-in interface's *type* rather than adding a
second interface, because none of iwlwifi's four valid interface combinations
includes `monitor`:

```sh
sudo nmcli radio wifi on && sudo nmcli dev set wlo1 managed no && \
sudo rfkill unblock wifi && sudo ip link set wlo1 down && \
sudo iw dev wlo1 set type monitor && sudo ip link set wlo1 up && \
sudo iw dev wlo1 set channel 149
```

Order matters and cost a round trip: `nmcli radio wifi off` sets an rfkill soft
block, and then `ip link set up` fails with "Operation not possible due to
RF-kill" and the type change silently does not stick. Unblock first. Revert
with `sudo iw dev wlo1 set type managed && sudo nmcli dev set wlo1 managed yes`.

### The chip counts the frame as dropped (2026-08-28)

**There is now a cheap, definitive failure signal.** `0x04EC` on this chip is
`REG_DROP_PKT_NUM` -- a dropped-packet counter -- and it discriminates perfectly:

| outcome | M2s built | drop counter across the attempt |
|---|---|---|
| failure | 4 | 1,2,3,4 -- one per M2 |
| failure | 4 | 2,3,6,8 |
| twelve successes in a row | 1 each | **11, static** |

So the chip is not silently losing the frame: it is *counting* it. Detecting the
fault no longer needs an air capture, which makes hypotheses cheap to test.

**How that register was found is worth repeating.** The driver had been writing
transmit-report enable bits to `0x04EC` and 0x3DF0 to `0x04F0` on every
initialisation, using the *generic* Realtek names. On this chip:

| address | generic header | 8814A header |
|---|---|---|
| `0x04EC` | `REG_TX_RPT_CTRL` | **`REG_DROP_PKT_NUM`** (counter) |
| `0x04F0` | `REG_TX_RPT_TIME` | **`REG_PTCL_TX_RPT`** |

The readback said `0x00 -> 0x00` on every boot while the log line said "TX
report enabled", so per-frame transmit reporting was never on and no C2H report
ever arrived. Both writes are removed; the counter is read instead. **Third time
a generic Realtek register name has meant something else on this chip** --
after 0x4FC/`EN_HWSEQ` and this. Always check `rtl8814a_spec.h`.

**Consequence: the C2H transmit report is not reachable this way.** The vendor
never touches those addresses for the 8814A, so getting the drop *reason*
(its report defines `RETRY_OVER` and `LIFE_TIME_OVER`) needs another route.

### Eliminated by measurement, at the failing M2

Everything here was measured on a failing frame, not argued:

| hypothesis | result |
|---|---|
| lifetime expiry | dead -- `LIFETIME_EN` (0x0426) reads 0x30, low nibble 0, expiry disabled for every AC; value 0xffffffff |
| bad retry-limit register | dead -- 0x042A reads back 0x3030 as written |
| wrong USB endpoint / queue | dead -- EAPOL moved to the VO queue (endpoint 0x02, the one management uses): 4/24 failed against 3/24 on BE |
| descriptor retry limit | dead -- setting `RetryLimitEn`+12 on data frames: 2/14 still failed, drops still counted |
| MACID | previously tested; moving data to MACID 1 **broke DHCP**, and MACID 0 is correct per the reference's allocation rules |
| TXPAUSE, SECCFG, pages, channel, USB completion, descriptor bytes | all dead, see above |

The VO result matters more than it looks: **management frames transmit on the
very queue and endpoint where our data frame is dropped**, so the discriminator
is not the queue. What still differs is frame type -- management versus data --
and MACID, which is already known-correct.

### A constraint that should have been applied sooner

**A permanently-missing register write or H2C command cannot explain a failure
that only happens 15% of the time.** It is a constant; the outcome varies.

That retires a whole class of candidate causes that has absorbed a lot of
effort here, including the two H2C commands the vendor sends and we never do
(item 4). They may well be worth adding on their own merits -- `H2C_RA_MASK_3SS`
(0x46) is marked *for 8814A* in the reference driver, so it is chip-specific and
plausibly required -- but **neither can be the cause of this defect**, and
neither should be attempted as a fix for it.

Applied to what varies, the same reasoning also killed the post-association
race. The sequence of events is identical in every single join --
`RA_INFO -> MEDIA_STATUS_RPT -> first M2 -> post-assoc setup completes` -- across
13 successes and 2 failures. The first M2 is always sent before post-assoc setup
finishes, in the working case as much as the failing one, so the overlap is not
the discriminator either.

### What actually varies, and the clue nobody has chased

Only three things vary between attempts: the air, the access point's state, and
the chip's internal state. Of those, one has a measured correlation already:

**Contention is dead too, and the "time of day" effect was over-read.** Air
utilisation during each attempt -- capture bytes per fixed window, now reported
for every attempt -- does not predict the outcome at all: median 1.89 MB across
9 successes against 1.85 MB across 5 failures, with fully overlapping ranges
(1.66-2.24 against 1.70-2.32).

And the time-of-day claim that motivated it does not hold up either. Tested
rather than asserted: 4/20 one afternoon against 12/30 that evening is
p = 0.216, and 2/18 against 12/30 is p = 0.049. One marginal result and one
null. **The rate is unstable, which is reason enough to interleave every build
comparison, but "it tracks time of day" is not established** and should not be
built on.

**The experiment that follows is cheap and uses data already being collected.**
The harness captures the air for each attempt; compute channel utilisation from
each capture and correlate it against the outcome. Successful attempts' captures
are currently discarded, so keep a short one for those too. If busy air predicts
failure, this is contention and the fix is in transmit timing or retry policy,
not in a missing register.

**One caveat on the instrument, stated because it changes the reading if wrong.**
`REG_DROP_PKT_NUM` is assumed here to count *transmit* drops. It sits in the
protocol page, which is transmit territory, and it increments exactly once per
M2 on a failing attempt and not at all across twelve successes -- a correlation
tight enough that it is hard to read as anything else. But the reference driver
never uses the register and no datasheet in the corpus documents it, so the
direction is inferred, not established. Worth confirming before building much on
it.

### C2H: the path is correct, the silence is expected -- do not chase it

**Correcting the previous entry, which recommended debugging this.** It is not a
bug and it is not the next thread.

C2H on this chip arrives **inline in the RX bulk stream**, flagged by `RPT_SEL`
in RX descriptor dword 2 bit 28 -- confirmed against the reference driver's
`GET_RX_STATUS_DESC_RPT_SEL_8814A`, which is the same offset and bit. **This
driver already implements that correctly**, in the RX callback, with a dispatch
into `HandleC2HEvent()` and a log line for the first twelve events. The
interrupt-IN listener in `WiFiManagement.cpp` is vestigial: its own comment
records that the endpoint was the wrong place to look. Debugging that listener,
which the previous entry suggested, would be chasing dead code.

The reason nothing arrives is that **nothing requests a report**:

- Transmit reporting has no reachable enable on this chip. 0x04EC is the
  dropped-packet counter here, not `TX_RPT_CTRL`.
- The per-frame request, `SPE_RPT` in transmit descriptor dword 2 bit 19 (our
  bit position is right), was tried on data frames and produced nothing. The
  reference driver sets it **only in its management-frame branch** -- "CCX-TXRPT
  ack for xmit mgmt frames" -- so on this chip the instrument only works on the
  frames that already succeed.

**Consequence: the drop reason is not obtainable by any cheap route.** Stop
trying to get it. `RETRY_OVER` versus `LIFE_TIME_OVER` would have been decisive,
lifetime is already excluded by register readback, and there is no third route
short of firmware work.

### The access-point comparison is still unanswered, and here is why

It was attempted twice and both runs are void. The second network failed 8 of 8
while the first ran 6 of 8 -- which looked like a large, clean effect and was
entirely an artefact: **each block of the second network followed a successful
join to the first, which left the chip's BSSID filter pinned to the wrong access
point, so the second SSID was not in the scan list to be joined.** That bug is
now fixed (see the CHANGELOG), and the comparison can be run again -- the
harness alternates SSIDs without a reboot, so it is about 40 minutes for 16
attempts each.

Two lessons worth carrying rather than repeating:

- **"No BSS matching X in scan list - run a scan first" was a misleading
  message.** A scan had just listed the network three times over. The message
  now prints the requested SSID with its length and the scanned entries with
  theirs, because the lookup requires an exact length match and a length
  disagreement is indistinguishable from a missing network.
- **An experiment that alternates networks was the wrong shape** while a
  connect-then-scan bug existed. Interleaving is still right; it just needs the
  thing being alternated to be independent between blocks, and it was not.

### The axis never tested: a different access point

Every measurement of this defect, on both adapters, has been against **one
network on one access point** -- `Adams-Guest`, 5 GHz channel 149. The
possibility that the access point is a participant has never been tested, and it
is the only variable left that is both plausible and cheap to change.

It is not far-fetched. The failure is the access point declining to proceed
after our M2 goes missing, the rate is unstable in a way no driver-side variable
explains, and the comparison that once suggested this was adapter-specific --
Edimax 1/60 against ASUS ~15% -- was made across different hours, and an
interleaved comparison has since shown the two adapters to be
indistinguishable (p = 0.567).

**Two experiments, in order of value:**

1. **Join a different access point.** Needs a passphrase for one; only
   `Adams-Guest` credentials are recorded. Any second network settles whether
   the access point is involved, and it is a handful of attempts, not a
   30-attempt run, if the effect is large.
2. **Interleave the two adapters against the same access point**, the way builds
   are interleaved, to settle whether the Edimax really is better or was simply
   measured at a better hour. Needs a physical swap per block, so it needs Kevin
   and is worth doing only after (1).

### What is left

**So the critical path is now step 3, not step 1.** Every driver-side avenue is
exhausted, which makes "is the frame actually absent from the air?" the
question that decides where this goes next:

- **If the frame is genuinely not transmitted**, the remaining lead is the
  chip's own TX report -- `REG_TX_RPT_CTRL` (0x04EC), bits 1 and 5, which the
  vendor toggles to enable per-MACID transmit reporting. That arrives as a C2H
  message, so it needs C2H handling this driver does not have yet. It is the
  only way found so far to ask the chip "did you put it on the air" without a
  monitor.
- **If the frame is on the air and the access point is rejecting it**, the
  investigation moves to frame content -- the MIC, the SNonce, and especially
  the replay counter. In the one captured failure the access point's four M1s
  carried replay counters 1, 2, 3 and 4; if our M2 answers a later M1 while
  echoing the first counter, an access point is entitled to discard it. That is
  a much more tractable problem than a silent chip.

Those two branches share almost no work, which is why confirming the air
observation comes first.

**Blocked on one root command.** Monitor mode on the capture host does not
survive a reboot of that machine, and `ip link set mon0 up` needs privileges
`iw` and `tcpdump` have but `ip` does not:

```sh
sudo ip link set mon0 up && sudo iw dev mon0 set channel 149
```

`iw phy phy0 interface add mon0 type monitor` already works unprivileged, so
mon0 usually exists but is down. Until then `MON=none` counts outcomes without
capturing.

**Step 1. Make the failing M2 observable.** *(done -- see above; kept for the
reasoning)* This is needed whichever way step 0
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

