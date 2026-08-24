# TX path

How frames flow out of the kernel network stack onto the air.

## Two TX origins

Frames can originate from two places:

1.  **Network stack via `Device::Write`** — the normal data path.
    Stack hands us a 14-byte ethernet header + payload.  We convert
    to 802.11 and queue.
2.  **In-driver management** — auth-req, assoc-req, EAPOL handshake
    frames.  These are built directly as 802.11 frames in driver
    code and submitted via `fTxPath->Transmit`.  See
    `Device::_SendAuthRequest`, `_SendAssocRequest`.

## Pipeline

![TX pipeline](diagrams/tx-pipeline.svg)

The data path: an ethernet frame arrives at `Device::Write`, gets
converted to an 802.11 frame, optionally encrypted via SW CCMP
(once the WPA2 handshake has installed keys), then handed to
`fTxPath->Transmit` which builds a chip-specific TX descriptor and
submits via USB bulk-OUT.  Management frames (auth-req, assoc-req,
EAPOL M2/M4) skip the eth→802.11 conversion since the driver builds
them directly as 802.11 frames already.

## Ethernet → 802.11 conversion

Stack gives us an ethernet frame:

```
[6] dst MAC | [6] src MAC | [2] ethertype | [N] payload
```

We wrap it as 802.11 infrastructure-mode data frame (ToDS=1):

```
FC byte 0 = 0x08              type=Data, subtype=0
FC byte 1 = 0x01 or 0x41      ToDS=1; Protected=1 set by SW CCMP
                              when keys are installed
Duration  = 0x003A
Addr1     = BSSID             where the frame is going (the AP)
Addr2     = our MAC           who's sending
Addr3     = real DA           original dst from the eth header
SeqCtrl   = 0                 chip fills in
LLC/SNAP  = AA AA 03          0x00 0x00 0x00 + 2-byte ethertype
[N]       payload bytes
```

Total wire frame (plaintext) = 24 (802.11 header) + 8 (LLC/SNAP) + N
(payload).  After SW CCMP encrypt it grows by 16 bytes (8-byte
CCMP IV header inserted between the 802.11 header and the LLC, plus
8-byte MIC appended at the end).

The `Addr1 = BSSID, Addr3 = real DA` mapping is crucial.  The chip's
TX scheduler routes frames based on Addr1; in infrastructure mode
that's always the AP's MAC, regardless of the real destination.
The original DA goes in Addr3 so the AP can forward to the right
station.

## SW CCMP encrypt (post-handshake)

Once the in-driver WPA2 handshake finishes and `fCcmpEnabled` is
true, every outbound data frame is encrypted in software using
`wpa2_crypto::ccmp_encrypt`.  The pairwise key (PTK[32..47] = TK)
is used for all STA-side TX — even when the logical destination
(eth `dst`) is broadcast, since the 802.11 frame is unicast at the
link layer (Addr1 = BSSID) and only the AP needs to decrypt.  The
AP re-broadcasts using its own GTK on the way out to other STAs.

EAPOL frames (ethertype 0x888E) are sent in clear regardless of
`fCcmpEnabled` — that's the 802.11i rule, the handshake itself
mustn't be encrypted.

See [wpa2-in-driver.md](wpa2-in-driver.md) for the SW CCMP design.

## TX descriptor

`TxPath::_BuildDescriptor` builds the 40-byte chip-specific descriptor that
prefixes every frame on the bulk-OUT pipe. The authority for the field layout
is `include/rtl8814a_xmit.h` in the reference driver, where each field is a
`SET_BITS_TO_LE_4BYTE(ptxdesc + <byte offset>, <lsb>, <width>)` macro. The
offsets are per-chip and do **not** carry over from the 8812A or 8188E headers
sitting next to them in the same tree.

### The field map we actually write

| dword | byte | Field | Bits | Value |
|---|---|---|---|---|
| 0 | 0 | `PKT_SIZE` | 0-15 | frame length, descriptor excluded |
| 0 | 0 | `OFFSET` | 16-23 | 40 — where the frame starts |
| 0 | 0 | `BMC` | 24 | group-addressed frames |
| 0 | 0 | `LAST_SEG` | 26 | always |
| 0 | 0 | `DISQSELSEQ` | 31 | every non-QoS frame |
| 1 | 4 | `MACID` | 0-6 | 1 |
| 1 | 4 | `QUEUE_SEL` | 8-12 | `QSLT_*` — see below |
| 1 | 4 | `RATE_ID` | 16-20 | 12 (`RATEID_IDX_MIX2`) |
| 1 | 4 | `SEC_TYPE` | 22-23 | 0; crypto is done in software |
| 2 | 8 | `BK` | 16 | non-management frames |
| 3 | 12 | `USE_RATE` | 8 | always |
| 4 | 16 | `TX_RATE` | 0-6 | `DESC_RATE*` index |
| 4 | 16 | `RETRY_LIMIT_ENABLE` | 17 | management frames |
| 4 | 16 | `DATA_RETRY_LIMIT` | 18-23 | 12, management frames |
| 5 | 20 | `DATA_SHORT` | 4 | short preamble, CCK rates only |
| 6 | 24 | `SW_DEFINE` | 0-11 | bit 0 — "the driver fixed the rate" |
| 7 | 28 | `TX_DESC_CHECKSUM` | 0-15 | 16-bit XOR, see below |

`FIRST_SEG` (dword 0 bit 27) is deliberately left clear: it is a
ring-descriptor concept and the reference's USB path has it commented out.

**There is no separate `OWN` bit on this chip.** Bit 31 of dword 0 is
`DISQSELSEQ`; older Realtek headers name the same bit `OWN`, from the PCIe
ring descriptor where it hands ownership to the DMA engine. Defining both
names and setting both is harmless only because they are the same bit — but it
makes "clear OWN" look like a change when it is a no-op.

### `REG_HWSEQ_CTRL` (0x0423), and why it is sidestepped

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

Moved here from `NEXT_SESSION.md`: it is a settled design decision about the
transmit path, not a task.

### No RTS/CTS, on any frame

`RTS_ENABLE` (dword 3 bit 12), `RTS_RATE` (dword 4 bits 24-28) and
`RTS_SHORT` (dword 5 bit 12) are all left clear.

Data frames used to set all three, on the stated grounds that the vendor
driver protects data frames with RTS/CTS "which is what the usbmon capture of
a working handshake on this chip shows".  Decoding the descriptors in that
same capture: the vendor sets `RTS_ENABLE` on **none** of its data frames, at
any size from 64 to 1528 bytes.  The claim was simply wrong.

It was not a harmless extra.  With `RTS_ENABLE` set, the MAC must win an
RTS/CTS exchange before it transmits the frame at all, so a missing CTS means
the frame is dropped **inside the chip**: the USB write completes, the
transmit counter increments, and nothing reaches the air.  Whether the
exchange succeeds depends on antenna wiring and transmit power, so this
passed on one adapter and stalled the four-way handshake at M2 on another —
the AP re-sent M1 four times and gave up with a reason-15 timeout.

The general lesson, which cost two separate bugs in this driver: a capture is
only evidence for what you actually decode out of it.  Both this and the
`0x0A04` override were written as "what the capture shows" and neither
survived contact with the bytes.

### The sequence number does not go in the descriptor

The descriptor's `SEQ` field is at byte 36, bits 12-23, and we leave it zero.
`HWSEQ_EN` (dword 8 bit 15) is also left clear.

Both were previously set, asking the MAC to assign sequence numbers — a
service `REG_HWSEQ_CTRL` was never written to enable, so the number stayed at
whatever the header carried, which was zero.  `Transmit()` now writes a real
sequence counter into the frame header itself, which needs neither the bit nor
the register.

The vendor driver takes the other route on its QoS data frames: it writes the
sequence into descriptor `SEQ` *and* the header, leaving `HWSEQ_EN` clear.
Either is fine; what does not work is asking for hardware sequencing without
enabling it.

This is worth stating explicitly because getting it wrong is expensive. A
12-bit software counter was previously written at dword 3 bits 16-27. Those
bits are not spare — they are `USE_MAX_LEN` (16), `MAX_AGG_NUM` (17-21),
`NDPA` (22-23) and `AMPDU_MAX_TIME` (24-27). `NDPA` is the damaging one: a
non-zero value there tells the chip the frame is an HT/VHT null-data-packet
announcement for channel sounding rather than an ordinary frame to transmit,
and three of every four sequence numbers set one of its two bits.

### `DATA_SHORT` is in dword 5, not dword 4

Short-preamble lives at byte 20 bit 4. Putting it in dword 4 instead puts it
on bit 4 of the 7-bit `TX_RATE` field, which rewrites the rate rather than
qualifying it. Concretely, a CCK 1 Mbps request is `DESC_RATE1M` = `0x00`, and
`0x00 | (1 << 4)` is `0x10`, which is `DESC_RATEMCS4`.

That is not hypothetical: an over-the-air capture of this driver's frames
showed them leaving as **11n MCS 4** when the driver had asked for CCK 1 Mbps,
at -61 dBm from a few inches with 29 retransmissions. One misplaced bit
accounts for the rate, and an 11n rate sent without the HT context to back it
accounts for the rest.

### `SW_DEFINE` bit 0 pairs with `USE_RATE`

`USE_RATE` tells the hardware to use `TX_RATE` instead of a rate-adaptation
hint. Bit 0 of `SW_DEFINE` tells the *firmware* the same thing. The reference
sets them together on every frame it fixes the rate for, tracked through its
`DriverFixedRate` local. Setting `USE_RATE` alone leaves the firmware's rate
adaptation believing it still owns the decision.

### Checksum

A 16-bit XOR over the **first 32 bytes** of the descriptor, stored at bytes
28-29, computed with that field zeroed. The reference's comment is explicit
that the span is always 32 bytes and does not scale with descriptor length —
so dword 8, which carries `HWSEQ_EN`, is outside the checksum by design.
Without a correct checksum the chip silently drops the frame at submission.

### EAPOL, ARP and DHCP are a special case

The reference branches on ether type: `0x888E` (EAPOL), `0x0806` (ARP),
`0x88B4`, and anything it has flagged as DHCP skip the aggregation and PHY
configuration block entirely and are sent with `USE_RATE`, `BK`, and the
management rate. Its comment — "Use the 1M data rate to send the EAP/ARP
packet. This will maybe make the handshake smooth." — says why: these are the
frames a handshake or an address lease depends on, so they are sent as
conservatively as the link allows rather than at whatever rate adaptation
currently favours.

## TX queues

`TxQueueSelect` names a hardware queue, not a USB pipe. That distinction was
learned the hard way: the enum used to hold pipe indices, which collapsed
`kTxQueueMGT`, `kTxQueueCMD` and `kTxQueueBCN` onto the same value so the
descriptor builder could not tell them apart and had to infer a QSEL from the
pipe — getting two of them wrong in the process.

- **kTxQueueMGT** — auth, assoc-req, deauth, disassoc.
- **kTxQueueBE** — best-effort data frames. Most user traffic.
- **kTxQueueVO/VI/BK/BCN/HIGH/CMD** — the remaining queues; VI, BK and the
  access-category split are currently unused (we don't honour 802.11e
  prioritisation).

Queue selection is `QUEUE_SEL` in dword 1, bits 8-12, and it takes the chip's
own `QSLT_*` namespace rather than our pipe indices: `QSLT_BE` = 0,
`QSLT_BK` = 2, `QSLT_VI` = 5, `QSLT_VO` = 7, `QSLT_BEACON` = 0x10,
`QSLT_HIGH` = 0x11, `QSLT_MGNT` = 0x12.

## USB submission

`fTxPath` maintains 16 transfer buffers per bulk-OUT pipe, 48 in total. Each
`Transmit` call claims a free buffer, builds the descriptor, copies the frame
after it, and submits the pair via `usb_module->queue_bulk`.

### Which endpoint a queue goes to

The three bulk OUT endpoints are 0x02, 0x03 and 0x04 in enumeration order, and
the chip services specific queues on specific endpoints. From
`_ThreeOutPipeMapping` in the reference driver, and confirmed against a usbmon
capture of the vendor Linux driver completing a handshake on this same chip:

| Queue | Pipe | Endpoint |
|---|---|---|
| VO, BCN, MGT, HIGH, CMD | 0 | **0x02** |
| VI | 1 | 0x03 |
| BE, BK | 2 | **0x04** |

This is not a preference about latency; it is what the hardware drains. This
documentation previously stated the opposite mapping — 0x02 for BE, 0x03 for
management — and so did the code. Management went to 0x04 and data to 0x03, an
endpoint the working driver never uses at all, which is why **management frames
reached the air and no data frame ever did**, across three over-the-air
captures. Everything downstream followed from it: EAPOL M2 unanswered until the
access point gave up with a four-way handshake timeout, DHCP never completing,
TCP unusable.

Firmware download is separate and uses a hardcoded pipe 0, which was always
correct.

### Bulk transfer sizing

A bulk transfer whose length is an exact multiple of the endpoint's max packet
size ends on a full packet, so the device cannot tell the transfer is over. The
descriptor's `PKT_OFFSET` field (dword 1, bits 24-28, in units of 8 bytes)
exists to avoid that: when `40 + frameLength` would land on a 512-byte
boundary, an 8-byte gap is inserted between descriptor and frame and declared
there, making the submission `48 + frameLength` instead.

## TX completion

USB completion fires `_TxCallback`, which frees the buffer back to the pool.
There is no per-frame ack-status bookkeeping.

`SPE_RPT` in the descriptor would ask the firmware for a per-frame transmit
report, delivered as a C2H event. It is deliberately not set: the vendor driver
asks for one on almost nothing, and no C2H report ever arrived during the
period this driver asked on every frame. Note also that the completion callback
compares against `submitLength`, which for a long time was only set on the
firmware-download path — so every ordinary transmit compared its byte count
against zero and logged itself as short. Every TX in the syslog looked like a
failure and none were.

## Write() entry point

`Device::Write` is the network-stack-facing entry.  Validates the
buffer length (≥ 14 bytes for ethernet header), checks
`fJoinState == kJoinConnected` (drop frames if we're not associated),
then calls into the conversion above.  Standard pattern.
