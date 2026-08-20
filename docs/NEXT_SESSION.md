# Plan for the next session

**The blocker is solved.** As of 2026-08-20 the driver associates, completes
the WPA2 four-way handshake, installs CCMP keys, obtains a DHCP lease and
passes bidirectional IP traffic. Verified end to end:

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

### 1. Throughput — the bottleneck is RECEIVE, and it is one bug away

Measuring each direction separately is what made this tractable:

| Direction | Throughput |
|---|---|
| shredder -> desktop (**transmit**) | **15-32 Mbit/s** |
| desktop -> shredder (**receive**) | **3.5-4.6 Mbit/s** |

Receive started the session at 1.0 Mbit/s. Two register fixes doubled it
twice: the aggregation threshold (below) and the retry limit. Transmit was
never the problem.

So the transmit path -- the thing that took all of 2026-08-20 to fix -- now
performs acceptably, and **receive is the whole problem.** That inverted the
expectation completely: the assumption had been that missing rate adaptation
and software CCMP encrypt were the limit, and they are not.

**What was found.** `REG_RXDMA_AGG_PG_TH` (0x0280) holds the page threshold in
byte 0 and the timeout in byte 1. It was set to `0x0520` -- threshold 0x20,
timeout 0x05, the two values transposed. A 160 us timer against a 32-page
threshold means the timer always wins, so the chip shipped a nearly-empty
transfer every 160 us and RX aggregation was in effect off. The vendor writes
`0x2005`: five pages, with the longer timeout as a backstop rather than the
trigger. Correcting it doubled receive throughput, 1 -> 2 Mbit/s. A second,
dead writer in `_EnableDMA` setting yet another value was removed --
`_InitRxAggregation` ran after it and always won.

**The retry limit was never programmed.** `REG_RETRY_LIMIT` (0x042A) holds the
short and long retry counts -- how many times the MAC retransmits an
unacknowledged frame before giving up. The constant was declared and never
written, so the chip kept its power-on default and frames that missed their
first acknowledgement were simply dropped. Writing the vendor's 0x3030 (48
each way) took receive from 2.0 to 3.5-4.6 Mbit/s, measured over three runs.

**What is left.** Receive is still roughly a tenth of transmit, and ICMP loss
sits at 5-20% and did not change when retries were enabled. Two contributing
pieces of the aggregation walk are fixed:

- The walk took `drvinfo_sz` as a hardcoded 32 whenever the PHY-status bit was
  set. The reference takes it from the descriptor, and now so do we. A
  single-frame transfer cannot detect the difference, which is why this
  survived for as long as aggregation was off.
- The per-frame offset is `kRxDescSize + drvinfo_sz + shift + packetLength`
  rounded up to 8, matching the reference's `_RND8`.

- `drvinfo_sz` is taken from the descriptor when it reports a size, falling
  back to 32 only when it reports zero. Both extremes were measured: trusting
  the descriptor unconditionally roughly doubled the misalignment rate,
  because on this chip the field frequently reads 0 while a 32-byte PHY status
  block is present, and following the reference literally under-advances by
  exactly that much.
- A packet length of zero now terminates the walk cleanly, as the reference
  does, instead of being parsed as a frame.

What remains is that **the loop is still bounded only by bytes remaining**, so
a transfer padded past its final frame can be walked one descriptor too far.
That accounts for 45-70 bail events per 4 MB transfer -- about 1.5% of frames,
so it is real but too small to explain the 5-20% loss or the receive shortfall
on its own. The loss is the thing to chase next, and its direction has not
been established: it could be our transmitted acknowledgements failing rather
than received frames being dropped.

**Do not fix that by reading the chip's aggregate frame count.** It lives in
the first descriptor at byte 12, bits 16-23, and the reference does read it --
but the reference's own definition is annotated *"Check if it exist anymore"*,
and on this chip it does not: bounding the walk by it dropped receive from
2 Mbit/s to effectively zero, because it under-reports and the walk then
abandons real frames. Tried, measured, reverted.

Better next steps: work out how the chip actually signals the end of the
aggregate — a zero packet length in the trailing descriptor is the obvious
candidate, and the reference does check `pkt_len <= 0` and bail — or take a
usbmon RX capture of the vendor driver and see exactly how it terminates.

**Measure properly before changing anything.** A single 4 MB `scp` has enough
variance to invent results: transmit measured 9, 17, 28, 32 and 15 Mbit/s
across the session on builds that differed in nothing relevant. Take at least
three samples, and beware integer division in the reporting -- 16079 ms for
4 MB is 1.99 Mbit/s, which prints as "1" and looks like a regression from
"2". That cost real confusion this session.

### 2. Throughput, after that

The remaining limits are known and all deliberate:

- **No rate adaptation.** Every data frame is sent at a hardcoded OFDM
  24 Mbps with `USE_RATE` set, which tells the chip to ignore its own rate
  logic. The `ARFR0`/`ARFR1` tables are now programmed, and `RATE_ID` in the
  descriptor selects among them, so the machinery exists -- what is missing is
  letting the firmware drive it and feeding it link quality.
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

### 3. Strip the diagnostics

The tree carries deliberate instrumentation that earned its place and should
now go: the ANonce and M2 hex dumps, the per-frame "RX from AP" dump, the
`M2 queue-empty` sampling, the `queue_bulk` failure dump, the unicast counter
in the heartbeat, and the deauth reason logging. Then bump the version -- the
repo still says 0.1.1 and only the build server has ever seen `0.1.2~test`.

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

### 5. 5 GHz association

Receive works and has since 2026-08-19, but association on 5 GHz has never
been attempted. `AdamsFamily02-5G` is on channel 149 and measured *stronger*
than the 2.4 GHz radio (-65 to -69 dBm against -72 to -74).

### 6. The Deskbar route

See `wpa-supplicant-and-deskbar.md`. Needs a supplicant-owned mode: the
in-driver handshake and wpa_supplicant cannot both own the four-way.

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

## The negative result worth not repeating

A verbatim replay of the vendor driver's MAC initialisation was tried: the
183 writes between the end of its firmware download and the start of its
BB/PHY table, in exact order, then trimmed to 177 to exclude its transition
into the PHY table. **Both versions left data frames untransmitted and
deterministically killed the post-association H2C path** -- the interface
associated, `B_NETWORK_WLAN_JOINED` fired, and nothing after it ran. The MAC
configuration was not the missing piece. Do not spend another afternoon there.

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
