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
| [wpa2-in-driver.md](wpa2-in-driver.md) | The in-driver WPA2-PSK implementation — why we don't use wpa_supplicant, why we don't use the chip's HW crypto, the 4-way handshake state machine, SW CCMP |
| [ioctl-reference.md](ioctl-reference.md) | Quick reference for SIOCS80211 / SIOCG80211 IOC handlers and what userland calls them |
| [build-and-deploy.md](build-and-deploy.md) | Building the .hpkg on a Haiku machine, installing it, fast-path edit-build-test iteration |

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

## Conventions

- All Haiku-specific code follows the [Haiku coding guidelines](https://www.haiku-os.org/development/coding-guidelines/) — tabs for indentation, BSD-style braces, `f` prefix for member fields, etc.
- Comments explain the **why**, not the what.  Subtle bug-driven decisions get a note.
- Diagrams are SVG only; no ASCII-art block diagrams.
