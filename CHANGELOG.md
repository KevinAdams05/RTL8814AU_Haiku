# Changelog

## Unreleased

**Every frame was transmitted about 42 times.** The MAC's response-timing
registers -- both SIFS registers, the two response-SIFS registers and the ACK
timeout -- were declared in the source and never written, so the chip ran on
its power-on defaults and did not register the access point's
acknowledgements. Captured over the air, a management frame went out 13 times
and a data frame as many as 49, while the access point acknowledged every
single transmission within microseconds of receiving it. The frames were
arriving and being acknowledged; the chip simply kept retrying them.

It cost more than airtime. The same timing governs the ACK this station sends
back, so association responses went unacknowledged too, and the access point
gave up after retransmitting its response four times -- which is what a join
that authenticates and then never associates looks like.

Measured over the air, same access point, one join each:

| | frames sent | first transmissions | retries | transmissions per frame |
|---|---|---|---|---|
| before | 510 | 12 | 498 | 42.5 |
| after | 8 | 8 | 0 | 1.0 |

Join failures over the same access point fell from 6 in 30 to **1 in 60**
(Fisher exact p = 0.005, two independent 30-attempt runs).


**Every frame this driver sent went out with sequence number 0.** Measured over
the air: 369 consecutive frames from this adapter, all numbered 0, while a
vendor-driven adapter on the same access point numbered its frames 1, 2, 3.

802.11 duplicate detection keys on (transmitter, sequence, fragment), so a
receiver is entitled to treat every frame after the first as a retransmission
of it. The visible symptoms were authentication being ignored outright -- 13
transmissions, no reply from the access point -- and a retransmitted M2 being
discarded once the first M2 had been seen.

The sequence number cannot be written into the frame header on this chip: the
MAC overwrites those two bytes on transmit. A frame submitted with sequence 3
in its header reached the air as sequence 0. Sequencing has to be asked for
through the descriptor, the way the reference driver does it -- `HWSEQ_EN` for
non-QoS frames, the descriptor's own `SEQ` field for QoS frames -- and the MAC
only honours `HWSEQ_EN` if `REG_HWSEQ_CTRL` enables the queue, which this
driver never wrote.

Verified over the air after the fix: sequence numbers increment (2636, 2637,
...) and retransmissions correctly reuse their original's number.


**`ifconfig <device> down` no longer hangs, and the interface comes back.**
Previously `down` never returned, left an unkillable process behind, and the
interface stayed unusable until a reboot.

- **A reader thread was parked in the driver's `Read()` with nothing to wake
  it.** The network stack takes an interface down by clearing `IFF_UP` and then
  waiting for its reader thread to exit; that thread was blocked on a semaphore
  released only when a frame arrives, and the receive path had already stopped,
  so no frame ever came. The close path now wakes each parked reader -- exactly
  once per reader, since a release nobody consumes is indistinguishable from a
  frame arriving and would strand whatever is queued.
- **Reopening the device left it deaf.** `Close()` stops the receive path every
  time, but it was only ever started from the first-open hardware init, so a
  reopened interface had no receive transfers submitted. Scanning still swept
  every channel and still reported completion, while the network list came back
  empty because no beacon had been received. `Open()` now restarts it.
- **`TxPath::CancelAll()` deadlocked against itself**, cancelling bulk OUT
  transfers while holding the lock its own completion callback needs. USB
  transfer cancellation on this stack runs those callbacks inline on the
  calling thread, so the thread waited for a lock it already held.

For anyone testing the driver, this is the difference between one reboot per
join attempt and none.

## 0.3.0 — 2026-08-21

**5 GHz works, and so does connecting from the Deskbar.** 0.2.0 could only
use 2.4 GHz, and only via the bundled `wifi-join` helper.

| | 2.4 GHz | 5 GHz |
|---|---|---|
| Transmit | 11-32 Mbit/s | **53.9 Mbit/s** |
| Receive | 2.4-3.1 Mbit/s | **15.1 Mbit/s** |
| Packet loss | 0-5% | **0%** at every payload size |

5 GHz is much the better band. Those figures are from an ASUS USB-AC68, where
both bands are verified end to end: association, four-way handshake, CCMP
keys, DHCP lease, ICMP at every ping size from 56 to 1472 bytes, and a full
SSH session over the air.

On an Edimax AC1750, 2.4 GHz reaches the same standard, and 5 GHz associates.
Both are subject to an **intermittent stall of the data queue just after
association** -- most runs are clean, but when it happens the handshake never
starts. See the known limitations below.

### 5 GHz

Three faults in the band switch, found by capturing the vendor Linux driver
associating to the same 5 GHz network over USB and diffing the register writes.

- **The RFE pinmux path D was only ever written when switching *to* 5 GHz.**
  After any visit to 5 GHz -- including the 5 GHz leg of an ordinary scan --
  path D kept the 5 GHz routing and 2.4 GHz went deaf. This is why every scan
  after the first returned nothing until a reboot.
- **The 5 GHz pinmux values were wrong**: 0x54775477 on all four paths, where
  the hardware wants 0x33173317 on paths A, B and C and 0x77177717 on path D.
  The coex register follows the same pattern.
- **One bit of the TX path register is band state.** It must be set for
  2.4 GHz and clear for 5 GHz. Leaving it set made 5 GHz receive perfectly and
  transmit nothing the access point ever answered.

### Connecting from the Deskbar

The Deskbar network menu, the Network preferences panel and
`ifconfig join <ssid> <passphrase>` all work now. They share one route:
`net_server` hands the join to `wpa_supplicant`, which runs the handshake and
passes the keys to the driver. `wifi-join` still works and still runs the
handshake inside the driver.

- **The SSID read-back had no handler.** `wpa_supplicant` reads the SSID back
  the moment it sees an association event; the failure made it conclude the
  association was not real and tear it down. This single missing ioctl was
  enough to make the whole route impossible.
- **The driver consumed every EAPOL frame**, so the supplicant never saw the
  handshake it was waiting for. It now stands aside when the supplicant is
  driving.
- **Key installation was a stub** that logged the supplicant's keys and
  dropped them.

**A correction.** Earlier releases said the Deskbar could not work because of
a Haiku kernel bug -- that EAPOL frames were not delivered to userland
`AF_LINK` packet sockets. That was wrong. Haiku delivers EAPOL correctly, and
the fault was entirely in this driver. Apologies to the Haiku project for the
misattribution.

### A second adapter, and the bug it found

Everything above was developed against an ASUS USB-AC68. Moving to an Edimax
AC1750 on the same chip broke 2.4 GHz immediately: association succeeded, then
the four-way handshake stalled at M2 with the access point re-sending M1 four
times and giving up with a reason-15 timeout.

- **Data frames asked for RTS/CTS protection, and the vendor driver never
  does.** With `RTS_ENABLE` set the MAC must win an RTS/CTS exchange before it
  will transmit at all, so a missing CTS means the frame is discarded inside
  the chip — the USB write completes, the transmit counter increments, and
  nothing reaches the air. Whether the exchange succeeds depends on antenna
  wiring and transmit power, which is why it passed on one adapter and not the
  other. Decoding the vendor's own descriptors showed it sets `RTS_ENABLE` on
  none of its data frames at any size from 64 to 1528 bytes. Removing it fixed
  the Edimax outright: handshake, CCMP keys, DHCP lease, and no packet loss.

- **Two EFUSE fields were read from the wrong offsets.** The antenna
  configuration and RF front-end class came from `0x00E` and `0x010`, which
  belong to a different chip's map, so both returned unrelated bytes. They are
  at `0x0C9` and `0x0CA` (the latter masked with `0x7F`). Every decision that
  had been made "per this adapter's EFUSE" was reasoning about noise.

- **The RFE pinmux is now chosen by board class** rather than hardcoded. The
  class comes from EFUSE `0x0CA` and is the chip's own mechanism for coping
  with differently wired boards. Both adapters tested report class 1, and the
  values previously hardcoded are exactly that class's — so this is not a
  behaviour change for either, but an adapter of another class no longer
  silently receives routing meant for someone else's board. An unrecognised
  class falls back to the vendor's default and says so in the syslog.

- **A post-init override of the CCK path register is gone.** It stamped
  `0x46ff800c` over `0x0A04`, a value taken from the middle of the vendor's
  cold-start sequence — which writes that register four times and settles on
  `0x45ff800c`. The override was undoing the last two writes of the trace it
  claimed to follow.

- **`RATE_ID` was 8, documented as "OFDM 6-54 Mbps".** Value 8 is
  `RATEID_IDX_B`, the CCK-only set — very nearly the opposite. It is now 12
  (`RATEID_IDX_MIX2`), matching the vendor, so OFDM data frames no longer ask
  the MAC for an OFDM rate out of a CCK-only table.

- **`REG_TX_HANG_CTRL` (`0x045E`) is now written**, as the vendor's own MAC
  initialisation table does on both bands. In fairness to the reader: a
  readback shows the register **already holds `0x04` before the write**, so
  this is a no-op on the hardware tested and fixes nothing. The write was
  missing; the value was not. It is kept because the vendor programs it
  explicitly and another board may default differently, and it is recorded
  here rather than quietly dropped because it was previously described as a
  promising lead for the transmit stall. It was not.

- **The same wrong value, in the rate-adaptation handshake.** The RA_INFO H2C
  command sent `rate_id` 8, commented "OFDM-only rate group", so the firmware
  was told this peer used CCK while the accompanying rate mask offered it
  nothing but OFDM. `rate_id` shares the descriptor's rate-group namespace;
  decoding the vendor's own RA_INFO out of the HMEBOX register writes gives
  12, agreeing with its descriptors. The rate mask stays deliberately
  narrower than the vendor's: every frame sets `USE_RATE`, which overrides the
  rate-adaptation engine, so it is close to inert until there is real rate
  adaptation to feed.

- **Association requests advertised CCK rates on 5 GHz.** The Supported Rates
  element claimed 1, 2, 5.5 and 11 Mbps as *basic* rates on both bands, and
  CCK does not exist above 2.4 GHz — so a 5 GHz request claimed four basic
  rates the band does not define. This access point answered with a DEAUTH
  carrying reason 2, "previous authentication no longer valid", which is a
  thoroughly misleading way to say "your association request is
  unacceptable". The element is now band-dependent: OFDM only above channel
  14, with 6, 12 and 24 Mbps basic, and the extended-rates element is emitted
  on 2.4 GHz only. This bug was latent on the ASUS, where the access point
  tolerated it.

Both of the first two were written as "what the usbmon capture shows", and
neither survived being checked against the actual bytes. A capture is only
evidence for what you decode out of it.

### 5 GHz stopped failing two joins in three

The post-association power-mode command never returned. It is issued through
the USB stack's synchronous request path, which has no timeout, and on this
chip that transfer sometimes never completes -- so the worker thread issuing it
blocked permanently. Everything after it was therefore skipped: the firmware
was never told the association existed, and the four-way handshake could not
finish.

**The command is simply not sent any more.** The vendor driver never sends it
either -- decoding every host-to-firmware command in two USB captures, across
all four mailboxes, it does not appear once -- and there is nothing for it to
do, since the chip is already in active mode. The concern it was guarding
against does not occur: in the runs where it hung, the first handshake message
still arrived.

Measured over 16 joins on 5 GHz: **13 succeeded, against 5 before**. The
supporting detail is that the post-association setup now runs 11 times and the
handshake completes 11 times, exactly matching, where previously the setup ran
4 times out of 17.

5 GHz and 2.4 GHz now fail at comparable rates, so the large gap between the
bands is closed.

### The four-way handshake could lose its own keys

**A retransmitted M1 restarted the key derivation.** The access point re-sends
M1 whenever M2 has not reached it yet, which happens routinely -- the two
simply cross in flight. Every M1 generated a fresh SNonce and re-derived the
PTK, so a retransmission discarded the keys belonging to the M2 the access
point was about to accept. It then sent M3 computed against the first SNonce
while the driver verified that MIC against a PTK derived from the second. The
MIC cannot match, so M3 was dropped repeatedly and the access point eventually
gave up with a four-way-handshake timeout.

```
EAPOL M1 / PTK derived / built M2 / WaitM3
EAPOL M1 / PTK derived / built M2 / WaitM3     <- keys replaced here
EAPOL M3 / M3 MIC mismatch ... RX DEAUTH reason=15
```

This was the most common failure on the Edimax, roughly one first join in
five, and it is intermittent precisely because it depends on the timing of an
M1 retransmission. The SNonce and PTK are now held fixed for as long as the
access point offers the same ANonce; a repeated M1 still rebuilds M2, because
M2 echoes the replay counter of the M1 it answers, but rebuilds it from the
same keys.

Worth recording how it was found, because the reasoning is reusable: M3
*arrived*, so reception and the BSSID filter were fine; and an access point
only sends M3 after accepting M2's MIC, so the key was right at that moment.
M3 is also the only frame whose MIC this driver verifies -- M1 carries none,
and M2's is generated locally. Generation proven correct while verification
failed means the key changed in between.

Measured after the fix, over 12 joins: the M1 retransmission that used to be
fatal occurred **4 times and was survived every time**. Ten handshakes reached
M3 and all ten verified; no MIC mismatch and no four-way timeout. The
retransmission rate of roughly one join in three also matches the failure rate
seen before the fix, which is the corroboration that matters -- the mechanism
predicts how often it should have been breaking, and it does.

### Also

- A failed join no longer leaves the radio unable to scan.
- Buffer allocation failure is reported instead of leaving a silently dead
  receive path.
- Transmit logging is budgeted per bulk endpoint instead of globally. A single
  counter was spent entirely on the firmware download, which all goes to one
  endpoint, so completions on the data endpoints were invisible — and "no log
  line for that pipe" reads as "that pipe failed" when it means "we never
  looked".

### Known limitations

- **Receive throughput on 2.4 GHz** is roughly a tenth of transmit. Each
  bulk-IN buffer stops receiving while its contents are processed and there
  are only four of them. 5 GHz is much less affected.
- No transmit rate adaptation; every data frame goes out at a fixed rate.
- CCMP runs in software rather than on the chip's engine.
- A-MPDU aggregation is disabled.
- **Open (unencrypted) networks do not work.** They must go through
  `net_server`, which tears the association down immediately. This one is not
  the driver's doing.
- No WEP, WPA3 or enterprise (802.1X) authentication.
- **About one join in six still fails on either band**, for reasons not yet
  identified: the interface associates and then the handshake does not
  complete. Retrying works.
- **About one first-join in five used to fail on 2.4 GHz**, and that cause is
  known and fixed: a retransmitted EAPOL M1 restarted the key derivation. See
  below. **Roughly one first join in eight still fails there for a different
  and unidentified reason** -- the interface associates and then stops hearing
  the access point altogether, so the handshake never starts and no address is
  obtained. Unicast management frames arrive normally right up until
  association completes, which is what makes it puzzling. Retrying works.
- **An intermittent stall of the data queue just after association**, on
  either band. Rarer than the above: **not once in 26 measured joins**, though
  it was seen several times before it was measured properly. There is now
  recovery for it -- the stuck transfers are cancelled and the endpoint's halt
  condition cleared -- but since the stall never recurred during measurement,
  that recovery is untested against the real failure and is best regarded as
  insurance rather than a fix. Bulk transfers on the best-effort endpoint stop completing and
  fill every slot, and the syslog fills with `TX wait timed out on pipe 2`.
  When it happens the four-way handshake never starts, so the symptom is an
  association that never gets an address. Most runs are clean. It has only
  been observed on the Edimax AC1750 so far, and was briefly and wrongly
  written up as a 5 GHz-specific problem on the strength of single runs. Not
  yet root-caused. Side effect worth knowing: `ifconfig` on the device hangs
  once this happens, so read the syslog instead.
- Tested on one access point and two adapter models: ASUS USB-AC68 and Edimax
  AC1750. Both report RF front-end class 1; an adapter of another class is
  untested and will log a warning.

## 0.2.0 — 2026-08-21

**The driver carries traffic.** Previous releases could scan and associate but
never passed a data frame. This one completes the WPA2 four-way handshake,
installs CCMP keys, obtains a DHCP lease, and carries ICMP and TCP — a full SSH
session over the air, at every ping payload size from 56 to 1472 bytes.

On 2.4 GHz, measured against a home router:

| | |
|---|---|
| Four-way handshake | completes; CCMP keys installed in the hardware CAM |
| DHCP | lease obtained |
| Packet loss | 0-5%, from 5-20% |
| Transmit | 11-32 Mbit/s |
| Receive | 2.4-3.1 Mbit/s |

5 GHz networks are visible to a scan and receive works there, but **associating
over 5 GHz is still untested.**

### What was wrong

Nineteen distinct defects, found by comparing against a second RTL8814AU
running the vendor Linux driver on the same access point, captured over USB
with `usbmon`. The chip has no memory-mapped I/O, so every register access is a
USB control transfer and such a capture is a complete transcript of what a
working driver does.

**Transmit — why no data frame ever reached the air:**

- Bulk OUT endpoints were mapped backwards. Management belonged on endpoint
  0x02 and data on 0x04; data was going to 0x03, which the working driver never
  touches. `TxQueueSelect` held USB pipe indices rather than queue identities,
  which also collapsed three distinct queues onto one value.
- EDCA channel-access parameters were declared and never written, so the
  best-effort queue could not contend for the medium.
- `REG_CR` was written with two bits that do not exist on this chip, while the
  security engine (`ENSEC`, bit 9) was never enabled at all.
- Uplink frames were marked as 802.11 broadcast whenever the *Ethernet*
  destination was — but a station sends every uplink frame to the access point
  as unicast, so DHCP DISCOVER went out group-addressed with the wrong MACID.
- TX packet-buffer page counts read a hex `0x20` as decimal `20`; the derived
  boundary did not even self-add.
- `REG_PKT_LIFE_TIME` sat at a default short enough for queued frames to
  expire, and `REG_MACID_SLEEP` came up with the management MACID marked
  asleep.
- `REG_RETRY_LIMIT` was never written, so any frame unacknowledged on the first
  attempt was dropped instead of retried.
- `REG_AUTO_LLT`, `REG_TXDMA_OFFSET_CHK`, the rate-fallback tables and the
  response rate set were all unwritten.
- Descriptor field errors: the sequence number was written over `NDPA`,
  marking most data frames as beamforming sounding packets; `DATA_SHORT` sat
  inside the 7-bit rate field, silently turning a CCK 1 Mbps request into
  MCS 4; `SW_DEFINE` was never set; management frames carried the wrong retry
  fields.
- Every frame went out as 802.11 sequence 0: the descriptor asked the MAC to
  assign one via `HWSEQ_EN`, but the register enabling that was never written.
- Bulk transfers landing on a 512-byte boundary stalled, for want of the
  descriptor's `PKT_OFFSET` padding.

**Receive:**

- The RX DMA aggregation register had its two fields transposed — a 32-page
  threshold against a 160 µs timer, so the timer always won and aggregation
  was effectively off.
- The walk over aggregated frames computed the next frame's offset from the
  FCS-stripped length instead of the on-air length. Rounding turned a 4-byte
  deficit into 8, so the next descriptor read landed inside the previous
  frame's payload and the walk abandoned every remaining frame in the
  transfer. This was the whole of the driver's packet loss.

### Also in this release

- Association requests now emit information elements in ascending element-ID
  order, which a strict access point requires and a lenient one silently
  punishes.
- Management and EAPOL frames are validated as addressed to us; without that
  the driver accepted other stations' auth and association responses.
- 5 GHz receive works: RF register writes go via the 3-wire LSSI registers,
  reads via the direct-mapped window. Writing the read window is silently
  discarded, which is why channel changes never took effect.
- Host-driven channel sweep for scanning, across both bands.
- A coding style guide and a style checker (`scripts/style-check.py`).

### Known limitations

- Receive throughput is roughly a tenth of transmit. The cause is understood
  and documented: each bulk-IN buffer stops receiving while its contents are
  processed, and there are only four of them.
- No rate adaptation; every data frame is sent at a fixed rate.
- CCMP runs in software rather than on the chip's engine.
- A-MPDU aggregation is disabled.
- Connecting from the Deskbar does not work: the driver runs its own handshake
  and wpa_supplicant wants to run one too.
- 5 GHz association untested.

## 0.1.1

Earlier development release. Scanning and association worked; data frames did
not reach the air.
