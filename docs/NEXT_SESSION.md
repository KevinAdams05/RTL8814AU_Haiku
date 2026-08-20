# Plan for the next session

Written 2026-08-20, after a TX descriptor audit against the reference driver.
Five field-level bugs came out of it, one of which is confirmed by an
over-the-air capture taken before the audit. None of them could be tested to
completion, because the access point stopped cooperating.

## What the audit found

The comparison was `_BuildDescriptor` against `update_txdesc` in
`hal/rtl8814a/usb/rtl8814au_xmit.c`, with bit positions taken from
`include/rtl8814a_xmit.h` — **not** from the `rtl8812a_xmit.h` and
`rtl8188e_xmit.h` headers beside it, which is how several of these got in.

| Field | Was | Should be | Consequence |
|---|---|---|---|
| `SEQ` | dword 3, bits 16-27 | byte 36, bits 12-23 (and unwritten on this path) | overwrote `USE_MAX_LEN`, `MAX_AGG_NUM`, **`NDPA`**, `AMPDU_MAX_TIME` on every frame |
| `DATA_SHORT` | dword 4, bit 4 | byte 20, bit 4 | bit 4 of the 7-bit `TX_RATE` field — rewrote the rate |
| `SW_DEFINE` bit 0 | never set | set whenever `USE_RATE` is | firmware never told the driver fixed the rate |
| mgmt `DISABLE_FB` + `NAV_USE_HDR` | set | `RETRY_LIMIT_ENABLE` + `DATA_RETRY_LIMIT` | those two belong to the beamforming NDPA branch |
| data `MACID` | forced to 1 | 0 | MACID 1 is `RTW_DEFAULT_MGMT_MACID`, the BMC station |

Two of these are worth understanding rather than just noting.

**`DATA_SHORT` explains the capture.** A CCK 1 Mbps request is `DESC_RATE1M`
= `0x00`. OR in bit 4 and you have `0x10`, which is `DESC_RATEMCS4`. The
capture showed frames leaving as **11n MCS 4 when the driver asked for CCK 1
Mbps**, at -61 dBm from a few inches with 29 retransmissions. That was logged
as an unexplained mystery; it was one misplaced bit, and an 11n rate sent
without the HT context to back it accounts for the poor link too.

**`NDPA` is the one to be angry about.** Bits 22-23 of dword 3 mark a frame as
an HT/VHT null-data-packet announcement for channel sounding. Three of every
four sequence numbers set one of those two bits. So most data frames were
being handed to the chip labelled as beamforming sounding packets.

Also cleared up: there is **no `OWN` bit** on this chip. Bit 31 of dword 0 is
`DISQSELSEQ`; the old Realtek `OWN` name refers to the same bit, from the PCIe
ring descriptor. Both names were defined and both were set, so "try clearing
OWN" was a no-op that looked like an experiment.

## What could not be tested, and why

The descriptor fixes are built, deployed and running. The handshake still does
not complete, but the access point is no longer providing a testable
handshake: across three consecutive boots it associates us (assoc status 0,
AID 1), sends Null Data keep-alives, and **never sends M1 at all**. Earlier
the same access point did send M1. Nothing in the driver's receive path
changed between those runs.

So the current state is: five real bugs fixed on reasoning that holds up
against the reference, and no verdict on whether they were *the* bugs.

One genuinely new observation came out of the failed runs, and it retires a
claim that was load-bearing in two documents: **unicast receive from our own
access point works.** The heartbeat now reports `unicast=10 fromOurAp=10`, and
the frames are the access point's Null Data keep-alives — subtype 4, `len=24`,
header only, unprotected. The old claim that we had "never received one
unicast frame addressed to us" is gone, and `deskbar-to-driver.svg` and
`wpa-supplicant-and-deskbar.md` have been corrected.

## Next session, in order

### 1. Get a cooperative access point

This is now the binding constraint, not the driver. Everything below needs an
access point that reliably sends M1, and neither available one does:

- The **phone hotspot** sent M1 earlier in the day and has stopped. It also
  auto-disables on inactivity, re-randomises its BSSID on restart, and
  measured -86 dBm with a bad FCS from eight feet.
- The **home router** associates us and has never sent M1.

A spare router, or a fresh hotspot on a different phone, converts this from
guesswork back into measurement. Power-cycling the hotspot before the run is
worth trying first, since it sent M1 earlier from the same code.

### 2. Then simply retest the handshake

With five descriptor bugs fixed, the handshake completing is its own signal
and needs no capture. If M3 arrives, the audit was the answer. If M2 still
goes unanswered with the rate and NDPA problems gone, the remaining
descriptor gaps are the aggregation and PHY fields the reference sets only for
non-EAPOL data frames — `DATA_BW`, `DATA_LDPC`, `DATA_STBC`,
`DATA_RATE_FB_LIMIT`, `MAX_AGG_NUM`, `AMPDU_DENSITY` — none of which should
matter for an EAPOL frame the reference deliberately routes around all of
them.

### 3. Dead ends now closed, so nobody re-walks them

- **`REG_TX_RPT_CTRL` at 0x04EC is not that register on this chip.** `0x04EC`
  is `REG_DROP_PKT_NUM_8814A`; the `REG_TX_RPT_CTRL` name is the generic
  definition in `hal_com_reg.h` for older parts. That is why the write never
  stuck. For 8814A, `0x04F0` is `REG_PTCL_TX_RPT`.
- **There is no register that enables TX reports on this chip.**
  `HW_VAR_TX_RPT_MAX_MACID` is 8188E-only, gated on `RATE_ADAPTIVE_SUPPORT`.
  The C2H report is driven purely by `SPE_RPT` in the descriptor.
- **`HW_VAR_MACID_LINK` is a no-op for 8814A** — no HAL in the tree
  implements it, so there is no "mark this MACID linked" bitmap gating data
  transmission. `REG_MACID_NO_LINK_0/1` (0x0484/0x0488) are 8188E names; on
  8814A those addresses are `REG_TXPKTBUF_IV_LOW/HIGH`.
- **Our `RPT_SEL` decode is correct**: byte 8, bit 28, matching
  `GET_RX_STATUS_DESC_RPT_SEL_8814A`. C2H detection is not the reason reports
  never arrive.

### 4. Loose ends worth closing regardless

- **The router never sends M1.** Check whether it wants a WMM/QoS IE in the
  assoc request, and whether stale client state for our MAC is involved.
- **`_TxCallback` reports `submitLength` as 0** in its log line even though
  the byte count is right, and then labels every completion
  `<-- SHORT/FAILED`. Every TX in the log looks like a failure and none of
  them are. This is actively misleading and should be fixed first, before it
  costs another wrong conclusion.
- **A latent teardown bug.** `B_BAD_VALUE` from `queue_bulk` provably means a
  NULL data pointer, and it appeared once in a wedged state — so slots were
  handed out with freed buffers. Cleanup does `delete[]`; check whether it
  NULLs.
- **`SIOCGIFSTATS` (8929)** is unhandled and floods the log with
  `Control unknown op=0x22e1`.
- **The interface-state bug** leaves an unkillable `ifconfig` after some join
  attempts, which then blocks the next scan. It forces a reboot between tests
  and is the single biggest drag on iteration speed.

## Testing notes that cost real time

- **`grep -a` on shredder's syslog.** It contains binary data, so plain `grep`
  misbehaves and silently mixes boots. Mark the position first with
  `MARK=$(wc -l < /var/log/syslog)` and then `awk -v s=$MARK "NR>s"`. Without
  the mark, lines from three boots ago read as current results — that happened
  again this session and briefly looked like a working handshake.
- **Reboot between join attempts**, and check `uptime` — the reboot-wait loop
  can connect to the still-running old system and test stale code.
- **Replacing a `.hpkg` does not swap the running driver.** packagefs serves
  the old one until reboot. Verify with
  `strings /boot/system/add-ons/kernel/drivers/bin/rtl8814au`.
- **A scan read immediately after `ifconfig scan` returns nothing** — the
  sweep purges the BSS list and takes ~5.6 s to refill. Sleep 20 s.
- **Never issue an H2C command from `_HardwareInit`.** It blocks device
  initialisation, so the network stack never finishes and the machine boots
  unreachable — ethernet included. Recovery is to unplug the dongle.
- **SSH goes very slow while DHCP is retrying** on the wireless interface.
  The box is fine; give `ConnectTimeout` 60 s and read the log in small
  greps rather than concluding it has wedged.

## Before any release from this branch

The tree carries deliberate diagnostics: deauth reason codes, the unicast
counter in the heartbeat, the per-frame "RX from AP" dump, the ANonce/M2 hex
dump, the `queue_bulk` failure dump, and the TX completion log. All earn their
place while this is open, but the hex dump in particular should go before
shipping. `SPE_RPT` is currently requested on **every** frame, where the
reference asks for it on almost none; that is a diagnostic too and should come
out with the rest. The repo version is still 0.1.1; the `0.1.2~test` bump
exists only on the build server.
