# Plan for the next session

Written 2026-08-19 at the end of a long day. One blocker is left on the WPA2
path, and it is well cornered: everything measurable inside the driver has
been verified correct, so the next move is to measure outside it.

## Where things actually stand

**Working, and verified rather than assumed:**

- Scanning across both bands, repeatably. 42 channels in ~5.6 s.
- **5 GHz receive** — first time this driver has ever seen a 5 GHz frame.
- Association, genuinely: auth and assoc responses arrive, pass the
  addressed-to-us check, and the AID is stable.
- EAPOL M1 receipt, PTK derivation, and M2 construction.
- M2 is **byte-perfect** — the MIC was recomputed independently and matched.
- The USB write for M2 completes in full and the chip drains its TX queue.

**The one blocker:** the access point does not accept M2. It keeps
retransmitting M1. Whether M2 reaches the air is the open question.

**A second, separate problem:** the home router associates us and never sends
M1 at all, where the phone hotspot does send it. Two different failures.

## Tomorrow, in order

### 1. Over-the-air capture, done properly (highest value)

This is the measurement that ends the guessing. Everything inside the driver
checks out, so the question is simply what leaves the antenna.

The first attempt failed for a reason worth not repeating: the monitor was
elsewhere in the house and recorded **nothing at all** from the machine under
test, across 74,000 packets from 373 other transmitters. That looked damning
until the contradiction surfaced — the access point demonstrably answers our
auth and assoc, so it must be receiving us.

Requirements this time:

- **Monitor beside the machine under test.** A laptop next to shredder, not
  across the house.
- **A control first.** Confirm the auth and assoc frames appear in the capture
  before drawing any conclusion about M2. If the control is absent, the
  capture is wrong, not the driver.
- Use `scratchpad/wifi-capture.sh` (v2): it sets `freq` rather than `channel`
  (v1 silently captured zero), releases the interface from NetworkManager,
  sanity-checks that frames arrive before starting, captures unfiltered, and
  restores the interface afterwards. Needs `sudo` in a real terminal.

Three outcomes, each decisive:

| What the capture shows | What it means |
|---|---|
| No M2, but auth/assoc present | The chip accepts M2 and never transmits it — descriptor or MAC-level gate |
| M2 present and well-formed | The access point is receiving and refusing it — look at its expectations, not ours |
| M2 present but malformed | The descriptor corrupts the frame between the driver and the air |

### 2. Try a better access point

The two available ones are both awkward, and this has cost time:

- The **phone hotspot** measures **-86 dBm with a bad FCS from eight feet**,
  where a phone should be nearer -45. On a link that marginal, "ignored our
  M2" and "never received our M2" are indistinguishable. It also
  auto-disables on inactivity and re-randomises its BSSID on restart.
- The **home router** is strong but never sends M1.

A third access point — a spare router, or a different phone — would tell us
whether the M2 refusal follows the driver or the access point. If a strong,
ordinary access point completes the handshake, the hotspot's link margin was
the whole story.

### 3. If the capture says M2 never radiates

Then it is a transmit-path question, and the ordered suspects are:

1. **Chip TX report counters.** Enable the firmware's per-frame report
   (`REG_TX_RPT_CTRL` 0x04EC, `REG_TX_RPT_TIME` 0x04F0) and read the C2H
   result. That distinguishes "never transmitted" from "transmitted, not
   acknowledged" without any external hardware, and the C2H plumbing already
   exists for other events.
2. **The descriptor's remaining fields.** Packet length, offset, queue
   selection, FS/LS/OWN and the checksum have all been checked against the
   reference; `HWSEQ_EN` is confirmed at dword 8 bit 15. What has *not* been
   checked line by line is dword 2 through dword 4 against
   `rtl8814a_xmit.c`.
3. **Bulk-out padding.** The submission is `kTxDescSize + frameLength` with no
   padding and no zero-length packet. Neither 193 nor 132 bytes is a multiple
   of 512, so the classic maxpacket rule does not explain what we see, but it
   is unverified.

### 4. Loose ends worth closing regardless

- **The router never sends M1.** Different failure from the hotspot; check
  whether it wants a WMM/QoS IE in the assoc request, and whether stale client
  state for our MAC is involved.
- **`_TxCallback` reports `submitLength` as 0** in its log line even though
  the byte count is right. Small instrumentation bug, briefly confusing.
- **A latent teardown bug.** `B_BAD_VALUE` from `queue_bulk` provably means a
  NULL data pointer, and it appeared once in a wedged state — so slots were
  handed out with freed buffers. Cleanup does `delete[]`; check whether it
  NULLs.
- **`SIOCGIFSTATS` (8929)** is unhandled and floods the log with
  `Control unknown op=0x22e1`. Answering it keeps that log useful.
- **The interface-state bug** leaves an unkillable `ifconfig` after some join
  attempts, which then blocks the next scan. It forces a reboot between tests
  and is the single biggest drag on iteration speed.

## Testing notes that cost real time today

- **`grep -a` on shredder's syslog.** It contains binary data, so plain `grep`
  misbehaves and silently mixes boots. Mark the position first with
  `MARK=$(wc -l < /var/log/syslog)` and then `awk -v s=$MARK "NR>s"`. Tailing
  a grep produced at least two wrong conclusions.
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

## Before any release from this branch

The tree carries deliberate diagnostics: deauth reason codes, the unicast
counter in the heartbeat, the ANonce/M2 hex dump, the `queue_bulk` failure
dump, and the TX completion log. All earn their place while this is open, but
the hex dump in particular should go before shipping. The repo version is
still 0.1.1; the `0.1.2~test` bump exists only on the build server.
