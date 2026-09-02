# Throughput

The first throughput figures for this driver that are actually trustworthy,
measured 2026-09-02. Everything before this was contaminated by routing, in
one direction or both, and the numbers in the 0.3.0 changelog entry are wrong
because of it -- see "Corrections" at the end.

## Test conditions

| | |
|---|---|
| adapter | Edimax AC1750 USB (`7392:a833`) |
| driver | `8cf5a5e` |
| network | `AdamsFamily02-5G`, WPA2 PSK/CCMP |
| channel | 149, **20 MHz**, 5 GHz |
| peer | Linux box on the same wired LAN, and the public internet |

The passphrase is supplied per run and is deliberately not recorded anywhere in
this repository.

## Results

Local file transfers, six real files, both directions, every transfer verified
with MD5 on both ends:

| file | size | down (to Haiku) | up (from Haiku) |
|---|---|---|---|
| `flac_30s_sample_file_1.1MB.flac` | 1.1 MB | 13.78 Mbit/s | 9.21 Mbit/s |
| `sample-5s.mp4` | 2.7 MB | 15.62 Mbit/s | 9.89 Mbit/s |
| `pcm_60s_sample_file_5.0MB.pcm` | 5.0 MB | 15.80 Mbit/s | 10.56 Mbit/s |
| `sample-mpeg-files-sample_1920x1080.mpeg` | 13.0 MB | 15.87 Mbit/s | 10.92 Mbit/s |
| `IntelDocs.zip` | 15.8 MB | 16.27 Mbit/s | 11.12 Mbit/s |
| `3_Doors_Down-Kryptonite.wav` | 39.3 MB | 16.43 Mbit/s | 11.03 Mbit/s |

**12 of 12 checksums matched.** The rate climbs with file size because the
fixed setup cost -- association is already done, but TCP slow-start and the
SSH handshake are not free -- is amortised over more bytes. Treat the largest
file's figure as the steady-state rate and the smallest as a floor.

A single 300 MB incompressible transfer, to check the rate holds rather than
decaying: **18.53 Mbit/s, MD5 matched, no stalls.** An isolated 20 MB probe run
on a quiet link reached 18.75 Mbit/s, so **the honest receive range is 16-19
Mbit/s** and the ladder above was measured while the link was also carrying the
reverse transfers.

Real internet downloads, checked against the distributor's published SHA256
rather than a checksum of our own making:

| file | size | time | rate | sha256 |
|---|---|---|---|---|
| `alpine-standard-3.21.7-x86_64.iso` | 278,921,216 | 140 s | 15.94 Mbit/s | **MATCH** |
| `alpine-extended-3.21.7-x86_64.iso` | 1,095,401,472 | 532 s | 16.47 Mbit/s | **MATCH** |

The second of those is **a single unbroken 1.02 GB download** that held
16.47 Mbit/s start to finish -- slightly *faster* than the 279 MB one, so the
rate does not decay with duration. Both matched Alpine's published SHA256,
which is a stronger check than our own checksums: it would catch an error that
corrupted our copy and our reference copy identically.

## Reliability under that load

Cumulative over the whole session -- **2.05 GB received and 207 MB
transmitted**, in both directions, local and internet:

| | |
|---|---|
| RX callbacks | **2,387,968**, of which 2,387,923 carried frames |
| driver RX counters | `crc=0 drop=0 (walk=0 icv=0)` |
| interface RX | 11 errors in 1,450,399 packets, **0 dropped** |
| interface TX | **0 errors** in 653,187 packets, 0 dropped |
| TX timeouts | 0 |
| `queue_bulk` failures | 0 |
| H2C timeouts | 0 |
| stalls, wedges, panics | none |

The 11 receive errors correspond to a single logged
`RX callback error: Device check-sum error` -- one bad USB bulk-IN transfer in
2.39 million callbacks. **That count did not move at all** across the second
gigabyte, so it is a one-off rather than a rate. The 99 driver log lines matching "reset" are all
initialisation messages (`DDMA reset`, `BB out of global reset`), one set per
join, not failures; and driver lines reading `status=0` are association
successes, since 0 is the success code in an 802.11 status field. Both are easy
to mistake for faults when grepping.

## Why the numbers are what they are

An AC1750 adapter is sold on 80 MHz channels, three spatial streams and frame
aggregation. This driver currently runs **20 MHz, no A-MPDU aggregation, no
transmit rate adaptation, and CCMP in software**. With aggregation off, every
frame pays a full preamble, SIFS and acknowledgement, so most of the airtime
goes to per-frame overhead no matter how fast the PHY rate is. Figures in the
mid-teens of Mbit/s are consistent with that configuration.

So the gap between this and the number on the box is accounted for by features
that are switched off, not by a defect in the data path. Raising it is a matter
of enabling 40/80 MHz, aggregation and rate adaptation -- each of which is its
own piece of work -- rather than of finding a bug.

The asymmetry is worth noting: **receive is now faster than transmit**, 16-19
against 9-11 Mbit/s. That reverses what the 0.2.0 and 0.3.0 changelog entries
say, and it is what would be expected once the receive-path fixes landed while
the transmit path still sends one unaggregated frame at a time.

## How to measure it, and the trap

Shredder has both a wired and a wireless interface **on the same subnet**, and
the routing table therefore holds two `192.168.74.0/24` routes plus two default
routes. Which interface a transfer actually uses is decided by their order,
which is incidental -- it changed during this session when the wired interface
was brought back up, moving the wireless `/24` route ahead of the wired one
while leaving the wired *default* route in front.

Consequently:

- **Receive** can be forced by addressing the wireless address (`192.168.74.77`)
  directly. That constrains the *sender*, which is the peer, so it works.
- **Transmit cannot be forced that way.** Addressing the wireless interface
  does not change how Haiku routes the reply, so the data leaves over the
  wired link. `ping -S` does not help either; source binding sets the source
  address, not the route.
- **Internet transfers follow the default route**, which is wired-first, so
  they need the same treatment as transmit.

The only reliable answer found so far is to take the wired interface down for
the duration. `scripts/iso-download-test.sh` does this safely: it runs entirely
on the test machine, so losing the controlling SSH session cannot strand it,
and a watchdog child restores wired unconditionally after 30 minutes whatever
the main flow does.

**Verify every figure against the interface's own byte counters.** Routing can
fake a rate; it cannot fake the counter. This is not a hypothetical:

- a 39.3 MB pull measured 94.54 Mbit/s in this very session, and the `Transmit`
  counter had moved by **3,569 bytes** -- it went over ethernet
- the same mistake was made earlier with a 100 MB file at "88.9 Mbit/s"
- the 0.3.0 entry's 53.9 Mbit/s transmit figure is the gigabit wired link

A useful sanity rule: if a wireless number beats the previous measurement of
the *opposite* direction by several times, suspect the route before believing
the result.

## Corrections

| claim | where | correct value |
|---|---|---|
| Transmit 53.9 Mbit/s on 5 GHz | 0.3.0 entry | 9-11 Mbit/s |
| Receive 15.1 Mbit/s on 5 GHz | 0.3.0 entry | 16-19 Mbit/s (close; stands) |
| "Receive is roughly a tenth of transmit" | 0.3.0 limitations | reversed: receive is now the faster direction |
| Transmit "cannot be measured here" | `testing-notes.md` | it can, by dropping wired |
