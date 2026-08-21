# Changelog

## 0.3.0 — 2026-08-21

**5 GHz works, and so does connecting from the Deskbar.** 0.2.0 could only
use 2.4 GHz, and only via the bundled `wifi-join` helper.

| | 2.4 GHz | 5 GHz |
|---|---|---|
| Transmit | 11-32 Mbit/s | **53.9 Mbit/s** |
| Receive | 2.4-3.1 Mbit/s | **15.1 Mbit/s** |
| Packet loss | 0-5% | **0%** at every payload size |

5 GHz is much the better band. Both are verified end to end: association,
four-way handshake, CCMP keys, DHCP lease, ICMP at every ping size from 56 to
1472 bytes, and a full SSH session over the air.

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

### Also

- A failed join no longer leaves the radio unable to scan.
- Buffer allocation failure is reported instead of leaving a silently dead
  receive path.

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
- Tested on one access point and two adapter models.

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
