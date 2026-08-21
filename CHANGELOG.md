# Changelog

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
