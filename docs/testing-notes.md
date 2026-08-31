# Testing notes

Method, tooling and traps for working on this driver. None of it is a task --
`NEXT_SESSION.md` holds those. This is the accumulated cost of measuring things
badly, written down so it is only paid once.

Most of these were learned by getting an answer, believing it, and being wrong.

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

## Check whether the donor does the thing at all

Before rebuilding the bounded transfer, the donor driver was checked, and it
answered the question outright: **the vendor never sends this command.**
Decoding every H2C in two usbmon captures, across all four mailboxes:

| H2C | name | vendor sends | we sent |
|---|---|---|---|
| `0x01` | `MEDIA_STATUS_RPT` | 2x | yes |
| `0x40` | `MACID_CFG` (RA_INFO) | 4x | yes |
| `0x42` | `RSSI_SETTING` | 13x | **no** |
| `0x46` | `RA_MASK_3SS` (8814A-specific) | 4x | **no** |
| `0x05` | `SET_PWR_MODE` | **never** | **every association** |

And there is a reason it never needs to: the reference defaults
`rtw_power_mgnt` to `PS_MODE_ACTIVE`, so the chip is already in active mode and
there is no mode to change. The concern the call was guarding against does not
materialise either -- in the boots where it hung, M1 still arrived, so unicast
was not being buffered.

So the call is simply removed. That deletes the driver's largest single failure
without adding any kernel plumbing, which is the better outcome by a wide
margin: the bounded-transfer attempt below ended in a KDL.

**The lesson worth keeping: check whether the donor does the thing at all
before engineering a safe way to do it.** A day went into making a hanging
command safe to issue, when the command was never needed.

**Two H2C commands are missing** and are now the obvious follow-up, since both
are sent repeatedly by the vendor and neither has ever been sent by us:
`0x42 RSSI_SETTING` (13 times -- feeds the firmware's rate adaptation) and
`0x46 RA_MASK_3SS`, which the reference header marks explicitly "for 8814A".
Worth investigating alongside the rate-adaptation work, which is stalled on
exactly the kind of information those commands carry.

## The negative result worth not repeating

A verbatim replay of the vendor driver's MAC initialisation was tried: the
183 writes between the end of its firmware download and the start of its
BB/PHY table, in exact order, then trimmed to 177 to exclude its transition
into the PHY table. **Both versions left data frames untransmitted and
deterministically killed the post-association H2C path** -- the interface
associated, `B_NETWORK_WLAN_JOINED` fired, and nothing after it ran. The MAC
configuration was not the missing piece. Do not spend another afternoon there.

## The test network is a measurement hazard

A 16-boot run produced 4 successes and 11 never-associated, against 13 of 14 on
the run before it. That looks exactly like a regression, and it was not one.
The failures were consecutive from boot 7 onwards -- variation does not arrive
in a block -- and a scan showed why: **`AdamsFamily02` was not in the BSS list
at all.** The 5 GHz SSID from the same access point was present the whole time
at -45 dBm. The 2.4 GHz network simply stopped being visible partway through
the run, then came back later at a weakish -61 dBm.

Two checks settled it in a couple of minutes, and both are worth repeating
before believing any regression:

- **Is the target actually in the scan list?** Not "did the join fail" but
  "was there anything to join". Print the BSS list.
- **Does the other band still work?** A successful join on 5 GHz while 2.4 GHz
  cannot even see the access point separates the driver from the environment
  immediately.

The corroborating detail was that the code path I had just added logged
`acquire_sem returned` zero times, so the change under test had never executed
its new branch and could not have caused anything.

Every failure-rate number in this document is only as good as the access point
was on the day. When a run disagrees sharply with the one before it, suspect
the network first. This is the same lesson the RadeonHD work learned twice from
a sleepy monitor and a bad cable.

## Load testing, and why transmit figures here are not trustworthy

The current build was put under sustained load rather than only repeated joins.
Receiving is clean:

| | |
|---|---|
| transferred | 100 MB, incompressible, over 5 GHz |
| rate | 46 s, about 17 Mbit/s |
| integrity | MD5 matched exactly |
| receive callbacks | 32768 -> 118784, i.e. 86,016 of them |
| driver counters | `crc=0 drop=0 (walk=0 icv=0)` |
| interface | 79,691 packets, **0 errors, 0 dropped**, 110.8 MB |
| transmit timeouts, `queue_bulk` failures, callback errors, panics | none |

**Transmit under load could not be measured, and the reason is a trap worth
knowing.** Both of shredder's interfaces sit on the same subnet, and Haiku's
routing sends everything out the wired one:

- Pulling the 100 MB file back "achieved 88.9 Mbit/s" -- impossible on a link
  that had just measured 17 Mbit/s inbound. The interface's `Transmit` counter
  was **unchanged at 22 packets** afterwards, so none of it went over the air.
- `ping -S 192.168.74.117` does not help. 2000 pings reported 0% loss, and the
  wireless `Transmit` counter moved from 22 to **23**. Source binding sets the
  source address; it does not override the route.

So any transmit figure gathered this way is really a measurement of the
gigabit wired link. **Check the interface's own `Transmit` counter before
believing a transmit number** -- it is the only thing here that cannot be
fooled by routing.

Testing it properly needs the wired interface down, which severs the only
control channel, so it needs either the cable physically out or a working
serial console. IPMI is currently unavailable for the latter.

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
- **A data-queue stall wedges `ifconfig` unkillably.** Read the
  syslog; an `ssh` that runs `ifconfig` will hang until killed.
- **The syslog spans reboots, so `tail` alone mixes boots.** Checking
  "did the handshake succeed" with `grep ... | tail -2` happily returned the
  *previous* boot's result twice. Bound the window (`tail -25`) or mark it.

## Tooling in `scratchpad/`

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


New with the 2026-08-25 air captures:

- `air-noreboot.sh` -- the join-attempt loop, one capture per attempt, kept
  only when the attempt fails. No reboots: an attempt is scan, join, check,
  `down`, `up`, about 70 seconds.
- `air-eapol.py` -- decodes EAPOL-Key frames out of an air capture with the
  802.11 header fields alongside, for comparing our M2 against a vendor M2 on
  the same access point.
- `seq-check.py` -- every frame a station transmitted, with sequence number and
  retry bit. First-transmissions versus retries is the airtime-waste metric.
- `air_eapol_util.py` -- the shared pcap/radiotap reader the two above use.

## The H2C mailbox decoder is not trustworthy at byte level

`scratchpad/h2c-all.py` decodes host-to-firmware commands out of a capture by
watching the mailbox registers. Its **command IDs are reliable** -- they sit at
a known offset and cross-check against the reference's own enum, and the
`MACID_CFG` payload it produces decodes to a sensible rate ID.

Its **payload bytes are not**. It takes each mailbox's four extension bytes as
whatever was last written to them, which can belong to an earlier command in
the same mailbox. Caught by comparing against the reference: for `RSSI_SETTING`
the decoder reports `h2c[1] = 0x8C` where `phydm_rssi_monitor.c` sets that byte
to zero unconditionally.

So the decoder is fine for answering "does the vendor send this command, and
how often" -- which is what retired the power-mode command and found the two
missing ones. It is not fine for reconstructing a payload to copy. Take layouts
from the reference and use the capture only to confirm the command is sent.

Fixing it properly means tracking which command each extension write belongs
to, rather than snapshotting the registers when the command byte lands.


## A neighbour's deauth is not your failure

The join harness decided a run had failed by matching `RX DEAUTH` in the
syslog. Neighbouring access points deauthenticate their own clients constantly,
and the driver logs every deauth it hears, so **a stranger's frame counted as
our failure.**

Caught by an air capture rather than by reading the log. A run marked FAILED had
exactly one driver event:

```
RX DEAUTH toUs=0 reason=7 from=02:c5:7d:2a:4d:0d
```

`toUs=0`, and from an access point belonging to a different network entirely.
The capture confirmed our station transmitted **zero frames** in that window
while the target access point sent 862 -- so the join never started, and the
deauth was pure coincidence.

**Every failure rate measured before this fix is overstated by some unknown
amount.** The same `CenturyLink3673` deauth appears in earlier "failing" boots,
which were probably not failures of ours at all. Match `RX DEAUTH toUs=1`.

The wider point: this driver logs everything it hears, not just what is
addressed to it, which is exactly what you want for diagnosis and exactly what
will mislead an automated verdict. Anything the harness treats as evidence of
*our* behaviour has to be filtered to frames addressed to us -- and the same
caution applies to the beacon, probe and auth counters.


## The air-capture tooling needed three corrections before it could be trusted

Every one of these would have produced a confident, wrong answer.

- **A silently empty filter looks like "the frame was never sent".**
  `air-analyse.py` parses radiotap and 802.11 itself rather than relying on
  tcpdump's 802.11 primitives, which are not always compiled in. It was then
  validated by pointing it at a station known to be present -- the access
  point -- rather than by trusting a zero.
- **Short control frames have no Address 2.** An ACK is frame control,
  duration, Address 1 and FCS, so reading bytes 10..16 attributes checksum
  bytes to whichever station they resemble. That inflated one validation from
  5 real frames to 19 and would have reported "our station transmitted" when
  it had transmitted nothing. Control frames are now excluded from address
  attribution.
- **"No EAPOL from us" is only suspicious if the driver tried to send one.**
  The first verdict logic announced the RTS-bug signature for an attempt that
  failed during authentication and never built an M2 -- there was nothing to
  transmit, so the absence proved nothing. The verdict now says so and points
  at the driver log.

And one in the capture harness itself: it saved only EAPOL and pipe-2 lines
from the driver log, which left a failure showing thirteen authentication
frames on the air with no driver-side record of sending any of them. Keep
enough of the log to interpret the capture.


## Two logging traps that hid evidence in plain sight

**A capped trace is silent exactly when you need it.** The per-pipe transmit
traces allowed 8 lines per pipe per boot. The firmware download spends that
allowance in the first seconds, so every join afterwards logged nothing, and
"no submit line for this pipe" read as "nothing was submitted" when it meant
"we stopped looking". `TxPath::ResetTraces()` now restarts them at each
association. Worth noting what saved this from being worse: the *completion*
trace always logs a failed or short completion regardless of the cap, so the
absence of `SHORT/FAILED` was real evidence even while the cap was hiding the
successful completions.

**`grep -a rtl8814au /var/log/syslog` drops multi-line output.** A `dprintf`
with embedded newlines gets the driver-name prefix on its *first* line only, so
filtering by the driver name silently discards every continuation line. The
transmit descriptor dump is three lines: the header line survived the grep and
the two `dw 0=... dw 5=...` lines did not, which made the descriptor look
undumped for an entire session. Use `grep -a -A2` around the anchor line, or
grep for the payload rather than the prefix.

## Three rounds is not enough; six was

Interleaving is necessary but not sufficient. Testing whether the
response-timing fix improved the join rate, two independent three-round
comparisons of the same two builds pointed in **opposite directions**:

| | PRETIME | POSTTIME | reading |
|---|---|---|---|
| run 1 | 10/30 | 18/30 | fix looks harmful, `p = 0.069` |
| run 2 | 9/30 | 4/30 | fix looks helpful |
| pooled | 19/60 (32%) | 22/60 (37%) | **no effect, `p = 0.701`** |

Run 1 alone would have been written up as "the fix may be making things worse",
on a p-value close enough to conventional significance to be persuasive. It was
one anomalous block: 8/10 failures in a POSTTIME block sitting between two ties.
Run 2 contained the mirror image, 7/10 failures in a PRETIME block.

**With block-to-block noise running from 0/10 to 9/10, three blocks per arm can
be dominated by one of them.** Budget six blocks per arm for anything that
matters, and if two independent comparisons disagree, pool them rather than
believing the more interesting one.

## The instrumentation made the fault worse -- measure with it removed

On 2026-08-28 a per-M2 register readback was added to characterise the reason-15
failure: TXPAUSE, SECCFG, CR, two page registers, the RF channel, the transmit
queue-empty flags, the dropped-packet counter and the lifetime enable. **Nine
synchronous USB control transfers, one of them an indirect RF read worth several
more, all on the EAPOL critical path** -- executed between handing M2 to the chip
and being able to process M3.

Measured, interleaved, 30 attempts each:

| build | join failures |
|---|---|
| with the readback | 20 / 30 (67%) |
| with it removed | 9 / 30 (30%) |

`p = 0.009`. **The diagnostics more than doubled the failure rate they were added
to investigate.** Every rate quoted while they were in is inflated.

Removing them was also sufficient: against the earlier build that had measured
1 failure in 60, the cleaned-up current build is 4/30 against 2/30, `p = 0.67`.
There was no second regression -- the instrumentation was the whole of it.

**The rule this gives:** on a path with a deadline -- a handshake, an interrupt
handler, anything an access point is timing -- a synchronous register read is not
free and is not passive. Before trusting any rate measured with new
instrumentation in place, measure again with it removed, interleaved. And prefer
instrumentation that is off the critical path: a one-shot dump, a counter read at
join time, a value latched now and printed later.

**How it was nearly missed.** Kevin said the driver seemed to have regressed. I
compared the ASUS against its shipped 17% baseline, found ~15%, and concluded
nothing had regressed. That was the wrong comparison: the regression was on the
*other* adapter against *its own* best measurement, 1.7% to 23%, `p = 0.0017`.
Reaching for the baseline that makes a concern disappear is how a real regression
survives a check.

## The air is the only place some faults are visible

Two hardware faults survived months of reading our own source, comparing USB
submissions against the vendor's, and decoding descriptors byte by byte. Both
fell out of a single over-the-air capture in an afternoon, because both are
faults in what the chip does *after* the driver hands the frame over:

- **Sequence numbers.** The driver wrote Sequence Control into the frame
  header and its own descriptor dump confirmed the bytes were there --
  sequence 3, plainly visible. The air showed sequence 0. This chip's MAC
  overwrites those two bytes on transmit. No amount of reading the driver, or
  even reading back what the driver submitted, could have shown that.
- **Acknowledgement timing.** Every frame was transmitted about 42 times while
  the access point acknowledged each transmission. From the driver's side a
  transmit looks entirely successful, because it is: the frame goes out, and
  goes out, and goes out.

The method that worked, and is worth repeating:

1. **Get a reference on the same access point.** Run the vendor Linux driver on
   the same adapter model, join the same network, and capture it. Ours numbered
   its frames 0, 0, 0; the vendor's 1, 2, 3. One line of output settled a
   question that had been open for weeks.
2. **Compare the air, not the submission.** `air-eapol.py` decodes EAPOL-Key
   frames out of a capture with the 802.11 header fields beside them, so our M2
   and the vendor's M2 can be diffed at the level the access point sees.
3. **Count first transmissions separately from retries.** `seq-check.py` prints
   every frame a station sends with its sequence and retry bit. The ratio
   between them is a direct measure of how much airtime is being wasted, and it
   is the metric that showed the response-timing fix working: 42.5
   transmissions per frame before, 1.0 after.
4. **Read the ACKs.** An ACK carries only Address 1, so it needs handling
   separately from every other frame -- and it is the frame that tells you
   whether the other end heard you. Believing "we retransmitted, so it was
   lost" without checking for the ACK gets the direction of the fault backwards.

The corollary is a rule for this driver: **when a register or descriptor field
governs something that happens on the air, do not conclude anything from the
value the driver wrote.** Capture it.

## Test attempts no longer need a reboot (fixed 2026-08-25)

For most of this driver's life every join attempt cost a reboot, because there
was no working way to re-join without one:

- Join once: succeeds.
- Join again with no teardown: authentication is transmitted, nothing
  associates.
- Join again after `ifconfig down` then `up`: nothing transmitted at all, and
  `down` itself never returned.

**Both blockers are now fixed** (see item 1 of `NEXT_SESSION.md`): the close
path wakes readers parked in `Read()`, and `Open()` restarts the receive path
that `Close()` stops. A join attempt is now scan -> join -> check -> `down` ->
`up`, about 70 seconds, with no reboot.

This matters for measurement, not just convenience. The **first** five-attempt
reboot-free run reproduced a reason-15 handshake failure that 28 reboot-based
attempts had never produced once. Reboot-cycling was not sampling the fault --
it was changing the conditions.

**The old warning still stands for runs that genuinely need fresh boots.** The
firmware-download failures clustered at the end of a long session of rapid
reboots and needed a **power cycle** to clear, because a Haiku restart does not
power-cycle USB. **A long reboot-heavy run can manufacture the very instability
it is trying to measure.**

Practical consequences:

- Prefer the reboot-free loop. Reserve reboots for questions that genuinely
  need a fresh boot -- firmware download being the obvious one. Most do not.
- Watch for firmware-load failures during a long run. Their appearance means
  the results after that point are suspect and the adapter needs a power cycle,
  not more attempts.
- When an intermittent fault refuses to appear across a few dozen attempts,
  consider that reboot-cycling may be changing the conditions rather than
  sampling them.
