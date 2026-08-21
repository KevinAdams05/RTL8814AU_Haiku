>[!NOTE]
>An LLM was used to aid in development of this code.

# rtl8814au — Documentation

Reference for how the rtl8814au USB Wi-Fi driver for Haiku works
internally — what each source file does, what state lives where,
and which design choices were forced by chip- or Haiku-specific
quirks.  Aimed at people reading or contributing to the source.

For *using* the driver, see the [top-level README](../README.md).

## Contents

| Document | What's in it |
|---|---|
| [architecture.md](architecture.md) | Driver structure, components, threading model, IOC layout |
| [hardware-init.md](hardware-init.md) | Power-on sequence, EFUSE, MAC init, firmware load, PHY init |
| [firmware.md](firmware.md) | Firmware blob layout, IDDMA load procedure, the 8-byte XOR trailers |
| [rx-path.md](rx-path.md) | Bulk-IN flow, descriptor parsing, SW CCMP decrypt, 802.11 → ethernet conversion, EAPOL diversion |
| [tx-path.md](tx-path.md) | Write() entry, ethernet → 802.11 conversion, SW CCMP encrypt, descriptor build, queue selection |
| [wifi-management.md](wifi-management.md) | Scan flow, join state machine, MLME, the Haiku ioctl glue |
| [phy-channel-and-band.md](phy-channel-and-band.md) | RF register access (the two routes), what a channel change entails, the 2.4/5 GHz band switch |
| [NEXT_SESSION.md](NEXT_SESSION.md) | Current state (the link works), what to do next — throughput first — the negative results worth not repeating, and the testing traps |
| [wpa-supplicant-and-deskbar.md](wpa-supplicant-and-deskbar.md) | What it takes to connect from the Deskbar: the net_server/wpa_supplicant chain, the net80211 ioctl contract, how EAPOL really reaches userland, and why the in-driver handshake conflicts with the supplicant |
| [wpa2-in-driver.md](wpa2-in-driver.md) | The in-driver WPA2-PSK implementation — why we don't use wpa_supplicant, why we don't use the chip's HW crypto, the 4-way handshake state machine, SW CCMP, and what was actually wrong for so long |
| [ioctl-reference.md](ioctl-reference.md) | Quick reference for SIOCS80211 / SIOCG80211 IOC handlers and what userland calls them |
| [build-and-deploy.md](build-and-deploy.md) | Building the .hpkg on a Haiku machine, installing it, fast-path edit-build-test iteration |
| [STYLE_GUIDE.md](STYLE_GUIDE.md) | Coding style for this driver — formatting, naming, the logging and register-access idioms, and the style checker |

## Diagrams

All diagrams are SVGs in [diagrams/](diagrams/) — open the raw file
in any browser.

| Diagram | What it shows |
|---|---|
| [architecture.svg](diagrams/architecture.svg) | Block diagram of the driver and how it interacts with USB, the chip, and the Haiku network stack |
| [eapol-diversion.svg](diagrams/eapol-diversion.svg) | Why EAPOL frames are intercepted in `_RxFrameReceived` instead of going through the Haiku network stack |
| [wpa2-state-machine.svg](diagrams/wpa2-state-machine.svg) | The 4-way handshake state machine running in-driver |
| [firmware-blob.svg](diagrams/firmware-blob.svg) | Layout of `rtl8814aufw.bin`, including the 8-byte XOR trailers per section |
| [rx-pipeline.svg](diagrams/rx-pipeline.svg) | RX flow from chip USB bulk-IN through to Haiku network stack |
| [tx-pipeline.svg](diagrams/tx-pipeline.svg) | TX flow from the Haiku network stack through SW CCMP encrypt to the chip |
| [scan-flow.svg](diagrams/scan-flow.svg) | `ifconfig scan` walk-through from userland to `B_NETWORK_WLAN_SCANNED` |
| [rf-register-access.svg](diagrams/rf-register-access.svg) | The separate write (3-wire LSSI) and read (direct-mapped window) routes to an RF register |
| [deskbar-to-driver.svg](diagrams/deskbar-to-driver.svg) | Deskbar to driver: commands down through `net_server` and `wpa_supplicant`, and events back up. This route works; the diagram's description used to say "where it currently breaks" |
| [rfe-board-class.svg](diagrams/rfe-board-class.svg) | How the RF front-end board class from EFUSE `0x0CA` picks the per-band RFE pinmux values, and what happens on an unrecognised class |

## Conventions

- All Haiku-specific code follows the [Haiku coding guidelines](https://www.haiku-os.org/development/coding-guidelines/) — tabs for indentation, BSD-style braces, `f` prefix for member fields, etc.
- Comments explain the **why**, not the what.  Subtle bug-driven decisions get a note.
- Diagrams are SVG only; no ASCII-art block diagrams.
