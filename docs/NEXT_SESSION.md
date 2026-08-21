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

### 1. Receive: loss is fixed, throughput is the remaining problem

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
