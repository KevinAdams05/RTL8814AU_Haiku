>[!NOTE]
>An LLM was used to aid in development of this code.

>[!WARNING]
>This is alpha version code. It works, but it is not finished.
>
>Current state in short: **both bands work, and so do both ways of
>connecting.** A WPA2-PSK network associates, completes the four-way
>handshake, installs CCMP keys, gets a DHCP lease, and carries ICMP and TCP
>on 2.4 GHz *and* 5 GHz — verified with a full SSH session over the air at
>every ping payload size from 56 to 1472 bytes. You can connect from the
>Deskbar network menu, the Network preferences panel, `ifconfig join`, or the
>bundled `wifi-join` helper.
>
>**5 GHz is much the better band**: about 54 Mbit/s sending and 15 receiving,
>against 11-32 and 2-3 on 2.4 GHz.
>
>Those results are from an ASUS USB-AC68. On an Edimax AC1750, 2.4 GHz is
>verified to the same standard, but **5 GHz associates and then stalls** —
>the data queue stops draining. That one is open; see the CHANGELOG.
>
>The main shortfall is receive throughput on 2.4 GHz. Three deliberate
>simplifications account for most of it — every data frame is sent at a fixed
>rate with no rate adaptation, CCMP runs in software rather than on the chip's
>engine, and A-MPDU aggregation is disabled. All three are addressable.

**Bug reports (please attach listdev output, syslog and/or screenshots) and PRs welcome! See "Logging Bugs / How to Help" section below**

# rtl8814au (Unofficial) - Haiku Driver

This is a driver for the Realtek rtl8814au series of USB wifi adapters. It is marked as "unofficial" because it is not developed by the Haiku maintainers/project team. There are no current plans to upstream into Haiku code. The driver is distributed as a standalone package, or you can build it from source.

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
4. Connect to a network — see the [How to Use](#how-to-use) section below.

To uninstall, delete the `.hpkg` from the `packages/` directory and reboot.

### Building from source

You need a Haiku x86_64 machine with:

- A configured Haiku source tree with cross-tools built — follow the
  [official Haiku build doc](https://www.haiku-os.org/development/build-haiku-from-source/)
  for the `configure --build-cross-tools x86_64` setup.
- A checkout of this repo.

```sh
# From the project root, with HAIKU_BUILD pointing at your haiku tree
# (defaults to ~/haiku-build/haiku if unset)
HAIKU_BUILD=$HOME/haiku-build/haiku bash package/build-hpkg.sh
ls -lh build/rtl8814au-*.hpkg
```

The script copies our `src/*` into the Haiku tree's
`src/add-ons/kernel/drivers/network/wlan/rtl8814au/`, copies the
firmware blob into `data/system/data/firmware/rtl8814au/`, runs
`jam -q -j4 rtl8814au` from the `generated.x86_64/` directory, and
wraps the kernel addon + firmware blob + LICENSE into the .hpkg.

To install the .hpkg you just built, drop it into
`~/config/packages/` and reboot — same flow as a prebuilt download.

See [docs/build-and-deploy.md](docs/build-and-deploy.md) for more
detail.

---

## How to Use

Once the package is installed and you've rebooted, the device shows
up as `/dev/net/rtl8814au/0` (or `/1`, `/2`, ... if you have more
than one).  Verify with:

```
ifconfig /dev/net/rtl8814au/0
```

You should see `Hardware type: Ethernet, Address: <your MAC>` and
the interface marked `up broadcast`.

### Scan for networks

```
ifconfig /dev/net/rtl8814au/0 scan
```

Returns the BSS list — SSIDs, signal strengths, security types.

A scan sweeps all 42 supported channels across both bands and takes about
5.6 seconds, so the results of the scan you just asked for may land just
after the command returns — run it twice if the network you want is not
listed yet.  The sweep is driven by the driver rather than the chip's
firmware, which does not sweep on its own.

Scanning is skipped while connected, since hopping away from the access
point's channel to collect beacons would drop the link.  Disconnect first
if you need fresh results.

### Join an open network

Use Haiku's standard `ifconfig` flow.  No special tools needed:

```
ifconfig /dev/net/rtl8814au/0 join <SSID>
ifconfig /dev/net/rtl8814au/0 auto-config
```

### Join a WPA2-PSK network

Any of these work:

- the **Deskbar** network menu
- the **Network** preferences panel
- `ifconfig /dev/net/rtl8814au/0 join <SSID> <passphrase>`
- the bundled `wifi-join` helper, below

The first three go through Haiku's `net_server`, which hands the join to
`wpa_supplicant`; `wifi-join` instead drives the handshake inside the driver.
Both routes are supported and end in the same place.

```
wifi-join /dev/net/rtl8814au/0 <SSID> <passphrase>
ifconfig /dev/net/rtl8814au/0 auto-config
```

`wifi-join` runs the WPA2-PSK 4-way handshake against the AP, then
forks into the background and keeps the device fd open so the
connection stays alive.  It prints the background process ID; the
connection lives until that pid dies.

Example:

```
$ wifi-join /dev/net/rtl8814au/0 MyHomeWiFi 'super secret pw'
wifi-join: handshake kicked off for SSID 'MyHomeWiFi' on /dev/net/rtl8814au/0
wifi-join: background pid 1234 holds the device open; kill it to disconnect.
wifi-join: bring up IP next, e.g. `ifconfig /dev/net/rtl8814au/0 auto-config`

$ ifconfig /dev/net/rtl8814au/0 auto-config
$ ping -c 3 192.168.1.1
PING 192.168.1.1 (192.168.1.1): 56 data bytes
64 bytes from 192.168.1.1: icmp_seq=0 ttl=64 time=0.402 ms
...
```

### Disconnect

Kill the background `wifi-join` process:

```
kill <pid>
```

This closes the device fd, which tells the driver to tear down the
link.

### Reconnect after reboot

Haiku does not currently auto-connect to a saved WiFi network at
boot.  After every reboot you need to run `wifi-join` again.

This is **not specific to this driver** — the same limitation
affects every WiFi driver on Haiku (the `iprowifi4965` and
`rtl8188ee` user threads converge on the same workaround).  The
underlying issue is in `net_server` / `wpa_supplicant` /
`wireless_networks` persistence, well outside this driver's scope.
See the Haiku forum discussion at
[Wi-Fi auto connect after boot](https://discuss.haiku-os.org/t/wi-fi-auto-connect-after-boot/13156)
for the current state of community workarounds and upstream activity.

**Workaround** — add the join + auto-config commands to
`~/config/settings/boot/UserBootscript` so they run at every login:

```sh
# At the bottom of ~/config/settings/boot/UserBootscript

# Bring down ethernet first if you only want WiFi
# ifconfig /dev/net/<your_ethernet>/0 down

# Connect WiFi and request DHCP
wifi-join /dev/net/rtl8814au/0 'YourSSID' 'YourPassphrase'
ifconfig /dev/net/rtl8814au/0 auto-config
```

The passphrase is in plaintext in this file — protect it accordingly
(`chmod 600 ~/config/settings/boot/UserBootscript`).  Equivalent
behavior can also be achieved by dropping an executable shell script
into the per-user-launch directory at `~/config/settings/boot/launch/`
if you prefer to keep boot commands separated by purpose.

### Why are there two ways to connect?

Both work, and they get there differently.

The **Deskbar and `ifconfig join`** route goes through Haiku's `net_server`,
which always hands a wireless join to `wpa_supplicant`. The supplicant runs
the four-way handshake and passes the resulting keys down to the driver. This
is the normal Haiku path and the one to prefer.

**`wifi-join`** instead hands the driver the passphrase directly and lets it
run the handshake itself, in the kernel. It exists because it worked first,
and it is still useful: it needs no `net_server` round trip, which makes it a
better tool for scripting and for diagnosing the driver in isolation.

An earlier version of this file claimed the Deskbar route was impossible
because of a Haiku kernel bug — that EAPOL frames were not delivered to
userland `AF_LINK` packet sockets. **That was wrong, and worth correcting
plainly since it was an accusation against Haiku rather than this driver.**
Haiku's stack delivers EAPOL correctly; a test binding an `AF_LINK` socket
for ethertype 0x888E succeeds. What actually blocked the Deskbar was three
faults in this driver: it swallowed every EAPOL frame before the supplicant
could see one, it never implemented the ioctl that installs the supplicant's
keys, and it failed the SSID read-back the supplicant performs right after
associating — which made the supplicant conclude the association was not real
and tear it down.

One genuine Haiku-side limitation remains: **open (unencrypted) networks**
have to go through `net_server`, and it tears the association down again
immediately. That one is not the driver's doing.

See [docs/wpa2-in-driver.md](docs/wpa2-in-driver.md) for the full
design.

---

## Source Material

This is a from-scratch Haiku driver, but the registers, init
sequences, and firmware-load procedure are unrecognizable without
public reference work.  The most useful sources during development
were:

- **morrownr's Linux 8814au fork** — `github.com/morrownr/8814au`.
  The cold-start register replay and the firmware-load procedure
  are derived from observation of this driver in action via USB
  packet captures.  Logic only, never copied code (see
  [docs/architecture.md](docs/architecture.md)).
- **`rtwn`** — the FreeBSD Realtek WiFi driver
  (`src/sys/dev/rtwn/`).  Used as a sanity check for register
  semantics and the security CAM programming pattern.
- **IEEE 802.11-2012** — the spec.  Most of the WPA2 4-way handshake
  and CCMP encryption code comes straight out of §11.4.

---

## License

Same as Haiku — MIT.
