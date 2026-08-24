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
