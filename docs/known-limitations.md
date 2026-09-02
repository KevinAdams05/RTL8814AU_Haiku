# Known Limitations

These are measured, not estimated. Full detail and method in [CHANGELOG.md](../CHANGELOG.md)
and the rest of these docs.

| | |
|---|---|
| **A join can fail** | 2 of 40 from a fresh boot (**5%**). Retry works. |
| **Don't cycle the interface repeatedly** | With `ifconfig down`/`up` between joins, failures rise to 18 of 40 (**45%**) and degrade from ~30% to ~85% over 120 joins. After ~150 cycles the interface stops being registered with the stack. **A reboot clears both.** |
| **Receive throughput** | 16–19 Mbit/s on 5 GHz |
| **Transmit throughput** | 9–11 Mbit/s on 5 GHz |
| **Channel width** | 20 MHz only — no 40/80 MHz, so no 802.11ac rates yet |
| **Aggregation** | No A-MPDU |
| **Rate control** | None; every data frame goes out at a fixed rate |
| **Crypto** | CCMP in software, not on the chip's engine |
| **2.4 GHz receive** | Slower than 5 GHz — prefer 5 GHz where you have it |

The join failure is an incomplete four-way handshake: the chip accepts a
well-formed 193-byte EAPOL M2, reports the USB transfer complete, counts the
frame dropped in `REG_DROP_PKT_NUM`, and never puts it on the air. Around
twenty candidate causes have been eliminated by measurement; the mechanism is
still unknown. The elimination record is in
[reason-15-investigation.md](reason-15-investigation.md) — read it
before investigating, it will save you repeating work.

The throughput figures are a **configuration limit, not a defect**. With 20 MHz
channels, no aggregation and no rate adaptation, per-frame overhead dominates
the airtime whatever the PHY rate is, so the gap to the "AC1750" number on the
box is features that are switched off. Method and counter-validated
measurements in [throughput.md](throughput.md).

Two limitations are **Haiku-side, not this driver**: open unencrypted networks
must go through `net_server`, which tears the association down again
immediately; and there is no auto-connect at boot, which affects every WiFi
driver on the platform (see the `UserBootscript` workaround under
[Reconnect after reboot](command-line-usage.md#reconnect-after-reboot)).

## Hardware coverage

Only the **ASUS USB-AC68** and **Edimax EW-7833UAC** have been in a machine.
The other seven USB IDs the driver claims are there because they appear in the
reference driver's RTL8814AU table, and are **untested** — the chipset is the
same, but nothing about a particular adapter's RF front end or EFUSE contents
has been verified. If you have one, a report either way is useful; see the
compatibility table in the [README](../README.md).
