>[!NOTE]
>An LLM was used to aid in development of this code.

# rtl8814au — Documentation

Reference for understanding, hacking on, and continuing development of the
Haiku rtl8814au USB Wi-Fi driver.

## Contents

| Document | What's in it |
|---|---|
| [architecture.md](architecture.md) | Driver structure, components, threading model, IOC layout |
| [hardware-init.md](hardware-init.md) | Power-on sequence, EFUSE, MAC init, firmware load, PHY init |
| [firmware.md](firmware.md) | Firmware blob layout, IDDMA load procedure, the 8-byte XOR trailers |
| [rx-path.md](rx-path.md) | Bulk-IN flow, deframing, ring buffer, 802.11 → ethernet conversion, EAPOL diversion |
| [tx-path.md](tx-path.md) | Write() entry, ethernet → 802.11 conversion, descriptor build, queue selection |
| [wifi-management.md](wifi-management.md) | Scan flow, join state machine, MLME, the Haiku ioctl glue |
| [wpa2-in-driver.md](wpa2-in-driver.md) | The in-driver WPA2-PSK implementation (Path B) — why we don't use wpa_supplicant, the 4-way handshake state machine, crypto primitives |
| [ioctl-reference.md](ioctl-reference.md) | Quick reference for SIOCS80211 / SIOCG80211 IOC handlers and what userland calls them |
| [build-and-deploy.md](build-and-deploy.md) | Cross-building from a Linux Haiku build server, deploy strategy, the dual-driver gotcha |
| [development-history.md](development-history.md) | Chronological summary of how we got here — the bring-up milestones, the dead ends, the diagnostic tools |
| [known-issues.md](known-issues.md) | Current limitations, open work, the path to a 1.0 release |

## Diagrams

All diagrams are SVGs in [diagrams/](diagrams/) — open the raw file in any browser.

| Diagram | What it shows |
|---|---|
| [architecture.svg](diagrams/architecture.svg) | Block diagram of the driver and how it interacts with USB, the chip, and the Haiku network stack |
| [eapol-diversion.svg](diagrams/eapol-diversion.svg) | Why EAPOL frames are intercepted in `_RxFrameReceived` instead of going through the Haiku network stack |
| [wpa2-state-machine.svg](diagrams/wpa2-state-machine.svg) | The 4-way handshake state machine running in-driver |

## Conventions

- All Haiku-specific code follows the [Haiku coding guidelines](https://www.haiku-os.org/development/coding-guidelines/) — tabs for indentation, BSD-style braces, `f` prefix for member fields, etc.
- Comments explain the **why**, not the what.  Subtle bug-driven decisions get a note.
- Diagrams are SVG only; no ASCII-art block diagrams.
