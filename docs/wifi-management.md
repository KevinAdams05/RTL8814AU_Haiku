# Wi-Fi management

The 802.11 management plane: scanning, joining, the auth + assoc
state machine, and the firmware H2C/C2H mailbox.  Most of this lives
in `WiFiManagement.cpp` plus the join state machine in `Device.cpp`.

## Scanning

![scan flow](diagrams/scan-flow.svg)

When userland issues `ifconfig <dev> scan`, the driver receives
`SIOCS80211 IOC_SCAN_REQ` and calls `_DoScanRequest`, which delegates
to `WiFiManager::StartScan`.  That does three things:

1. Purge stale entries from the BSS list (anything older than the
   previous scan).
2. Send an `IOC_SCAN_EN` H2C command to the firmware.
3. Spawn a one-shot scan-notifier thread, which performs the scan.

**The host drives the channel sweep, not the firmware.**
`_SendScanCommand` can only pass a channel list to the firmware when the
list fits the six-byte H2C payload, which is four channels; the real
42-channel list therefore takes the `payload[1] = 0` "firmware scans all
supported channels" path.  The firmware does not sweep, and never sends
the `kC2H_ScanComplete` that would claim it had.  Left to itself the chip
simply stays on whatever channel it was on.

So `Device::_ScanSweep` hops the chip across `kChannelList2G` followed by
`kChannelList5G`, dwelling `kScanDwellTime` (120 ms) on each — longer than
the usual 100 ms beacon interval, so a passive listen reliably hears one.
A full 42-channel sweep takes about 5.6 seconds.  Beacons and probe
responses arrive through the normal RX path throughout:
`_RxFrameReceived` dispatches them to `_ParseBeaconOrProbe`, which adds
entries to `fBssList` (max 64 BSSes, oldest purged).  The only thing the
sweep changes is which channel we are listening to.

The sweep runs on the notifier thread deliberately.  Setting a channel is
a long series of USB register writes — the RF synthesizer on all four
paths, plus band-specific AGC tables, RFE routing and per-channel TX power
on a band change — and must not run on the USB callback thread.

Two guards:

- It refuses to sweep while connected.  Hopping away from the AP's
  channel to collect beacons would drop the link.
- It restores the pre-scan channel afterwards, so an unrelated join does
  not inherit wherever the sweep happened to end.

When the sweep finishes, the notifier calls `WiFiManager::FinishScan` and
fires `B_NETWORK_WLAN_SCANNED` on the network monitor port.  Userland then
fetches results via `SIOCG80211 IOC_SCAN_RESULTS`.

`FinishScan` is not optional bookkeeping.  `StartScan` sets the state to
`kWiFiStateScanning`, and the only other thing that clears it is
`_HandleScanComplete`, which runs off the firmware event that never
arrives.  Without `FinishScan` the state stuck for the rest of the boot,
so exactly **one scan per boot** ever ran and every later one returned
`B_BUSY` — and because joining resolves the SSID through `FindBssBySsid`,
any AP that was not beaconing during that single window could not be
joined until the machine was rebooted.  `StartScan` additionally treats a
scan still in progress after `kScanStaleTimeout` (15 s) as finished, in
case a notifier dies without calling `FinishScan`, and drains the
completion semaphore so this scan's waiters cannot return early on the
previous scan's release.

## Join state machine

Driven by `Device::_DoJoin`.  Two state variables:

- `fJoinState` — `kJoinIdle` / `kJoinAuthenticating` /
  `kJoinAssociating` / `kJoinConnected`.
- (For WPA2:) `fEapolState` — see [wpa2-in-driver.md](wpa2-in-driver.md).

`_DoJoin(bssid, ssid, ssid_len)`:

1.  If we got stuck in a previous attempt (state == auth or assoc),
    log it and reset to idle.
2.  If the caller passed all-zero or broadcast bssid, look up the
    BSS by SSID in `fBssList` to recover bssid + channel.  Return
    `B_NAME_NOT_FOUND` if the network isn't in the recent scan
    list.
3.  Park the chip on the AP's channel via `PhyConfig::SetChannel`.
4.  Set MSR = `kMSR_Infra` so the chip's MAC auto-ACKs frames
    addressed to us.  Without this, the AP's responses get retried
    a few times and then we get silently kicked from the client
    table.
5.  Program `kRegBSSID` so the chip's frame filter accepts mgmt
    traffic from this BSS.
6.  Save `fJoinBssid`, `fJoinSsid`, `fJoinSsidLength`.
7.  Set `fJoinState = kJoinAuthenticating`.
8.  Call `_SendAuthRequest`.

## Auth request / response

We use only **Open System** authentication (algorithm 0).  WPA2-PSK's
key derivation runs *after* assoc as the EAPOL handshake; the open
auth is just a placeholder.

`_SendAuthRequest` builds:

```
FC: type=mgmt, subtype=Auth
DA = BSSID, SA = our MAC, BSSID = BSSID
SeqCtrl = (++fJoinSeqCounter) << 4
Body: auth_alg=0, auth_seq=1, status=0
```

`_HandleAuthResponse` checks the status code and transitions to
`kJoinAssociating`, then calls `_SendAssocRequest`.

## Assoc request / response

`_SendAssocRequest` builds the body with:

- Capability info (ESS, plus Privacy bit when `fPrivacy != 0`)
- Listen interval = 20 TUs
- SSID IE
- Supported Rates IE: 1, 2, 5.5, 11, 6, 9, 12, 18 Mbps (with the
  4 mandatory rates marked basic)
- Extended Supported Rates IE: 24, 36, 48, 54 Mbps
- RSN IE if `fWpaIeLength > 0` — set via either `IOC_APPIE` (the
  wpa_supplicant code path; doesn't actually drive the handshake on
  Haiku, but does deposit the IE), or synthesized in-driver by the
  in-driver WPA2 path when `IOC_HAIKU_JOIN` is invoked with a PMK

`_HandleAssocResponse` reads cap-info, status, AID:

```
RX assoc response cap=0x1011 status=0 aid=50
```

`cap=0x1011` = ESS + Privacy + Short Slot.  `status=0` = success.

On success:

1.  `fJoinState = kJoinConnected`.
2.  `WiFiManager::MarkConnected(bssid, ssid)` flips manager state
    so userland sees the link as up via
    `ETHER_GET_LINK_STATE`.
3.  Release `fLinkStateSem` so net_server re-polls immediately.
4.  Publish `B_NETWORK_WLAN_JOINED` so wpa_supplicant
    (when running) knows we're associated.
5.  Release `fPostAssocSem` so the post-assoc worker fires the H2C
    setup (RA_INFO + MEDIA_STATUS_RPT).

## Post-assoc worker

Synchronous USB control transfers can't run from the bulk-IN
callback context.  But the firmware needs RA_INFO programmed
*before* it'll TX our data frames at any rate other than basic;
without it the rate-adaptation table for our STA's MACID is empty
and the chip silently drops every queued data frame.

`_PostAssocLoop` runs in its own thread, blocks on `fPostAssocSem`,
and on each release issues the H2C sequence:

1.  Re-write BSSID register (idempotent with `_DoJoin`).
2.  RA_INFO H2C (cmd 0x40): MACID 0, rate_id 12 (`RATEID_IDX_MIX2`),
    BW 20 MHz, rate mask covering OFDM 6–54 Mbps.

    `rate_id` shares the TX descriptor's 5-bit rate-group namespace. It
    was 8, described here and in the code as "OFDM"; 8 is
    `RATEID_IDX_B`, the **CCK-only** group, so the firmware was told
    this peer used CCK while the mask offered it only OFDM. Decoding the
    vendor driver's own RA_INFO from the HMEBOX register writes in a
    usbmon capture gives 12, matching the `RATE_ID` in its transmit
    descriptors.

    The rate mask stays narrower than the vendor's on purpose: every
    frame sets `USE_RATE`, which overrides the rate-adaptation engine,
    so the mask is close to inert, and advertising only rates the driver
    can actually produce is the conservative choice while there is no
    rate adaptation and no HT aggregation.
3.  MEDIA_STATUS_RPT H2C (cmd 0x01): connect=1, MACID=0.

After that, the chip is fully ready to TX/RX user data.  The TX
path uses MACID=1 by convention — the firmware silently drops
data frames sent on MACID=0 in some builds, and aligning the TX
descriptor's MACID with the RA_INFO slot is the documented Realtek
practice across rtwn and the morrownr Linux driver.

## H2C / C2H mailbox

The firmware exposes a host-to-card / card-to-host mailbox.  H2C
commands are 8-byte writes to `kRegH2C0` (a sequence of mailbox
slots cycles through `kH2CMailboxCount`).  C2H events come back as
4+N-byte messages over the USB interrupt-IN endpoint.

We handle a few C2H types:

| Event | Behavior |
|---|---|
| `kC2H_ScanComplete` | release the scan-done sem so the notifier fires.  **Never observed on this chip** — the host-driven sweep in `_ScanSweep` is what actually completes a scan, and `FinishScan` clears the state. |
| `kC2H_ConnectionStatus` | log; eventually drives reconnect logic |
| `kC2H_RateAdaptive` | log only |
| `kC2H_TxReport` | log only |
| `kC2H_Debug` | hex-dump the firmware's own debug bytes |

The interrupt-IN callback (`_InterruptCallback` in `WiFiManagement.cpp`)
re-submits itself to keep the listener alive for the lifetime of
the device.

## Disconnection

`WiFiManager::Disconnect` clears state and resets `fJoinState =
kJoinIdle`.  The driver does not currently send an explicit deauth
frame to the AP; the AP times us out instead, which usually takes a
few seconds.
