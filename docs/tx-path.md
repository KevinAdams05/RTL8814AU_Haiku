>[!NOTE]
>An LLM was used to aid in development of this code.

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
| 0 | 0 | `DISQSELSEQ` | 31 | always, paired with `HWSEQ_EN` |
| 1 | 4 | `MACID` | 0-6 | 1 |
| 1 | 4 | `QUEUE_SEL` | 8-12 | `QSLT_*` — see below |
| 1 | 4 | `RATE_ID` | 16-20 | 8 |
| 1 | 4 | `SEC_TYPE` | 22-23 | 0; crypto is done in software |
| 2 | 8 | `BK` | 16 | non-management frames |
| 2 | 8 | `SPE_RPT` | 19 | ask for a per-frame TX report |
| 3 | 12 | `USE_RATE` | 8 | always |
| 4 | 16 | `TX_RATE` | 0-6 | `DESC_RATE*` index |
| 4 | 16 | `RETRY_LIMIT_ENABLE` | 17 | management frames |
| 4 | 16 | `DATA_RETRY_LIMIT` | 18-23 | 12, management frames |
| 5 | 20 | `DATA_SHORT` | 4 | short preamble, CCK rates only |
| 6 | 24 | `SW_DEFINE` | 0-11 | bit 0 — "the driver fixed the rate" |
| 7 | 28 | `TX_DESC_CHECKSUM` | 0-15 | 16-bit XOR, see below |
| 8 | 32 | `HWSEQ_EN` | 15 | always |

`FIRST_SEG` (dword 0 bit 27) is deliberately left clear: it is a
ring-descriptor concept and the reference's USB path has it commented out.

**There is no separate `OWN` bit on this chip.** Bit 31 of dword 0 is
`DISQSELSEQ`; older Realtek headers name the same bit `OWN`, from the PCIe
ring descriptor where it hands ownership to the DMA engine. Defining both
names and setting both is harmless only because they are the same bit — but it
makes "clear OWN" look like a change when it is a no-op.

### The sequence number does not go in the descriptor

The descriptor's `SEQ` field is at byte 36, bits 12-23. We do not write it,
and neither does the reference on this path: `HWSEQ_EN` asks the hardware to
assign sequence numbers, and the reference only writes `SEQ` for QoS frames,
which supply their own and leave `HWSEQ_EN` clear.

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

The chip has multiple TX queues.  We use:

- **kTxQueueMGT** — auth, assoc-req, deauth, disassoc.  High
  priority, low latency.
- **kTxQueueBE** — best-effort data frames.  Most user traffic.
- **kTxQueueVO/VI/BK** — the other access categories, currently
  unused (we don't honor 802.11e prioritization).

Queue selection is `QUEUE_SEL` in dword 1, bits 8-12, and it takes the chip's
own `QSLT_*` namespace rather than our pipe indices: `QSLT_BE` = 0,
`QSLT_BK` = 2, `QSLT_VI` = 5, `QSLT_VO` = 7, `QSLT_BEACON` = 0x10,
`QSLT_HIGH` = 0x11, `QSLT_MGNT` = 0x12.

## USB submission

`fTxPath` maintains a pool of 12 USB bulk-OUT buffers.  Each
`Transmit` call takes a free buffer, builds the descriptor, copies
the frame, and submits via `usb_module->queue_bulk` to one of the
three bulk-OUT endpoints.

The three endpoints map to traffic-class buckets — endpoint 0x02 for
queue 0 (BE), 0x03 for queue 1 (mgmt), 0x04 for queue 2 (HQ).
Picking the right endpoint matters: if we submit a mgmt frame on
the BE endpoint it gets queued behind data and may not go out fast
enough to satisfy auth/assoc timeout requirements.

## TX completion

USB completion fires `_TxCallback`.  Currently we just free the
buffer back to the pool — no per-frame ack-status bookkeeping.
The chip's H2C/C2H protocol provides aggregate TX-report C2H events
(`kC2H_TxReport`) which we currently log but don't act on.

## Write() entry point

`Device::Write` is the network-stack-facing entry.  Validates the
buffer length (≥ 14 bytes for ethernet header), checks
`fJoinState == kJoinConnected` (drop frames if we're not associated),
then calls into the conversion above.  Standard pattern.
