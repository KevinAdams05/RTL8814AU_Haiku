>[!NOTE]
>An LLM was used to aid in development of this code.

**Bug reports (please attach listdev output, syslog and/or screenshots) and PRs welcome! See "Logging Bugs / How to Help" section below**

# rtl8814au (Unofficial) - Haiku Driver

This is a driver for the Realtek rtl8814au series of USB wifi adapters. It is marked as "unoffical" because it is not developed by the Haiku maintainers/project team. There are no current plans to upstream into Haiku code. The driver will be distributed via a standalone package, or you can build for the source.

This is a native Haiku driver, not based on the FreeBSD compatibility layer. Realtek doesn't publish a driver datasheet for this chipset, so the Linux driver was used as a reference for things such as registers and init. However, the driver wasn't copied directly. The goal is a native "Haiku-first" development philosophy.

Since this driver will be released as a standalone package we did not change any Haiku code. There are a couple things that could have been done on the OS level, but instead we had to do in the driver. This is outlined in more detail in the technical documentation.

---

## Tested Hardware

This driver was physically tested on the following devices:


| Field | ASUS USB-AC68 | Edimax EW-7833UAC |
|---|---|---|
| **Brand / Model** | ASUS USB-AC68 | Edimax EW-7833UAC |
| **Marketing class** | 802.11ac AC1900, 4×4 dual-band | 802.11ac AC1750, 4×4 dual-band |
| **USB VID:PID** | `0b05:1817` | `7392:a833` |
| **USB Manufacturer string** | `Realtek` | `Realtek` |
| **USB Product string** | `802.11ac NIC` | `Edimax AC1750 USB` |
| **Serial** | `123456` | `123456` |
| **Chipset** | Realtek RTL8814AU | Realtek RTL8814AU |
| **USB version / speed** | USB 3.0 SuperSpeed (5 Gbps) | enumerates at both USB 2.0 HS and USB 3.0 SS |
| **MxPwr** | 864 mA | not captured (same chip → same expectation) |

---


## Documentation

Detailed docs live in [docs/](docs/).  Highlights:

- [Architecture overview](docs/architecture.md) — how the driver is organized, threading model, where to start reading
- [In-driver WPA2-PSK](docs/wpa2-in-driver.md) — why we don't use `wpa_supplicant`, the in-kernel 4-way handshake design
- [Hardware initialization](docs/hardware-init.md) — power-on, EFUSE, MAC init, firmware load, PHY config
- [RX path](docs/rx-path.md) and [TX path](docs/tx-path.md) — frame conversion, EAPOL diversion, descriptor build
- [Wi-Fi management](docs/wifi-management.md) — scanning, auth + assoc state machine, H2C/C2H mailbox
- [Firmware](docs/firmware.md) — blob layout, IDDMA load procedure, the 8-byte trailer gotcha
- [IOCTL reference](docs/ioctl-reference.md) — the 80211 IOC handlers and what userland calls them
- [Building and deploying](docs/build-and-deploy.md) — cross-build recipe, deploy strategy, package gotchas
- [Development history](docs/development-history.md) — chronological summary of milestones and dead ends
- [Known issues and roadmap](docs/known-issues.md) — what's done, what's not, the path to a 1.0 release

Diagrams in [docs/diagrams/](docs/diagrams/) — all SVG.

---

## Logging Bugs / How to Help

Bugs are welcome! To log a bug, [please log it here in github as an issue](https://github.com/KevinAdams05/rtl8814au_unofficial/issues), and include as much detail as possible.

**From Haiku**
- attach your syslog file
- note if you are using a USB2 or USB3 port
- if you hit KDL then please include a picture of the screen.
- attach the output of the following script
```
#!/bin/sh
# haiku-wifi-dump.sh — Bash works on Haiku
echo "=== listdev ==="
listdev
echo
echo "=== listusb -v ==="
listusb -v
echo
echo "=== Loaded kernel images ==="
listimage | grep -iE 'wifi|wlan|802|rtw|iwl|net|ether'
echo
echo "=== syslog (network) ==="
tail -500 /var/log/syslog | grep -iE 'rtl|rtw|wifi|wlan|usb|net'
echo
echo "=== Network interfaces ==="
ifconfig
echo
echo "=== Firmware on disk ==="
ls /system/data/firmware/ 2>/dev/null
ls ~/config/non-packaged/data/firmware/ 2>/dev/null
```

**From Linux** (if you can boot to a Linux live USB)
- attach the output of the following script
```
#!/bin/bash
# linux-wifi-dump.sh — pipe to a file
echo "=== USB Devices ==="
lsusb
echo
echo "=== Tree ==="
lsusb -t
echo
echo "=== dmesg (USB/Wi-Fi) ==="
dmesg | grep -iE 'usb|wlan|wifi|rtw|iwl|ath|brcm|80211'
echo
echo "=== Loaded Kernel Modules ==="
lsmod | grep -iE 'rtw|iwl|ath|brcm|mac80211|cfg80211|usb'
echo
echo "=== /sys/kernel/debug/usb/devices ==="
sudo cat /sys/kernel/debug/usb/devices
echo
echo "=== Wireless interface ==="
iw dev
ip -s link
```

PRs are welcome! However, please test all code changes on physical hardware before opening the PR! On your PR indicate which device you tested on, and include the device ID.


---

## Installation

### From a prebuilt `.hpkg`

1. Download `rtl8814au-<version>-x86_64.hpkg` from the [Releases page](https://github.com/KevinAdams05/rtl8814au_unofficial/releases).
2. Copy it into one of:
   - `~/config/packages/` — installs only for your user (recommended)
   - `/system/packages/` — installs system-wide (needs root)
3. Reboot.  On boot, packagefs activates the package and the driver appears at `/dev/net/rtl8814au/0`.
4. Join a network:
   - **Open:** `ifconfig /dev/net/rtl8814au/0 join <SSID>`
   - **WPA2-PSK (AES):** see [In-driver WPA2-PSK](docs/wpa2-in-driver.md) for the current workflow.  The standard Haiku `wpa_supplicant` flow does **not** work because of a Haiku stack bug with EAPOL delivery on AF_LINK; this driver runs the 4-way handshake in-kernel and uses an SW CCMP fallback for the data path.

To uninstall, delete the `.hpkg` from the `packages/` directory and reboot.

### Building from source

You need:
- A working Haiku x86_64 source tree + cross-tools (see the [official build doc](https://www.haiku-os.org/development/build-haiku-from-source/)).
- A Linux or Haiku machine to run the build on.
- (Windows) a way to `ssh` and `scp` to that build machine.

#### One-shot build via `build.ps1` (Windows host + Linux build server)

The intended developer flow.  Edit `package/build.ps1`'s `$RemoteHost` / `$RemoteDir` / `$OutputDir` to match your environment, then:

```powershell
.\package\build.ps1
```

It will:
1. `scp` the `src/`, `firmware/`, `package/`, and `LICENSE` to the build server.
2. Run `package/build-hpkg.sh` over SSH, which stages the source into the Haiku tree, runs `jam -j4 rtl8814au`, and wraps the resulting kernel addon binary + firmware blob in a `.hpkg`.
3. `scp` the produced `.hpkg` back to your `$OutputDir`.

#### Manual build (on the build host directly)

If you have the project tree available on the build host:

```bash
# On the build host, with HAIKU_BUILD pointing at your haiku checkout
cd /path/to/this/repo
bash package/build-hpkg.sh
ls -lh build/rtl8814au-*.hpkg
```

Override the defaults via env vars if your Haiku tree lives elsewhere:

```bash
HAIKU_BUILD=/srv/haiku/src HAIKU_ARCH=x86_64 bash package/build-hpkg.sh
```

The build script copies the driver source into `$HAIKU_BUILD/src/add-ons/kernel/drivers/network/wlan/rtl8814au/` and the firmware blob into `$HAIKU_BUILD/data/system/data/firmware/rtl8814au/` so that the Haiku tree's existing `KernelAddon` jam machinery can link it.

---

## Source Material
coming soon


---

## License

Same as Haiku — MIT.
