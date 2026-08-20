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

### 1. Throughput

~2 Mbit/s on a 2x2-capable link is functional but poor. The causes are known
and all deliberate:

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

### 2. Strip the diagnostics

The tree carries deliberate instrumentation that earned its place and should
now go: the ANonce and M2 hex dumps, the per-frame "RX from AP" dump, the
`M2 queue-empty` sampling, the `queue_bulk` failure dump, the unicast counter
in the heartbeat, and the deauth reason logging. Then bump the version -- the
repo still says 0.1.1 and only the build server has ever seen `0.1.2~test`.

### 3. REG_HWSEQ_CTRL (0x0423) -- still unwritten, still a real gap

Every non-QoS descriptor sets `HWSEQ_EN`, asking the MAC to fill in the
sequence number, and that requires this register enabled. The vendor writes
`0xFF`; ours reads `0x00`, so every frame requests a service that is switched
off and management frames all go out with sequence 0. The constants exist
(`kRegHwSeqCtrl`, `kHwSeqCtrlAllQueues`) but are deliberately not written,
because both placements tried hang the driver: during hardware init the M2
transmit never returns, and inside `_DoPostAssocSetup` the worker dies before
reaching it. The vendor writes it late in the association phase. Now that the
link works, this can be approached without it being load-bearing.

### 4. 5 GHz association

Receive works and has since 2026-08-19, but association on 5 GHz has never
been attempted. `AdamsFamily02-5G` is on channel 149 and measured *stronger*
than the 2.4 GHz radio (-65 to -69 dBm against -72 to -74).

### 5. The Deskbar route

See `wpa-supplicant-and-deskbar.md`. Needs a supplicant-owned mode: the
in-driver handshake and wpa_supplicant cannot both own the four-way.

### 6. Loose ends

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
