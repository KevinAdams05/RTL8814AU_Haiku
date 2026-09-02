>[!NOTE]
>**Version 1.0.0.** The driver works, but a join can still fail
>and need retrying. There may also be other bugs, please log them if you run into issues. This is
>not part of the official Haiku distribution.


**Bug reports (please attach listdev output, syslog and/or screenshots) and PRs welcome! See "Logging Bugs / How to Help" section below**

# rtl8814au (Unofficial) - Haiku Driver

This is a driver for the Realtek rtl8814au series of USB wifi adapters. It is not developed by the Haiku maintainers/project team. There are no current plans to upstream into Haiku code. The driver is distributed as a standalone package, or you can build it from source.

This is a native Haiku driver, not based on the FreeBSD compatibility layer. Realtek doesn't publish a driver datasheet for this chipset, so the Linux driver was used as a reference for things such as registers and init. However, the driver wasn't copied directly. The goal is a native "Haiku-first" development philosophy.

---

## Hardware Compatibility List

Every device the driver claims is listed below, in the same order as
`kSupportedDevices` in `src/RTL8814AU.h`. Only the first two have been in a
machine; the rest are claimed on the strength of their USB ID appearing in the
reference driver's RTL8814AU table.

🚧 - in-progress
✅ - tested and confirmed
🟨 - not tested
🟥 - tested and does not work

| Device | Tested |  ID | Class |
|---|---|---|---|
| ASUS USB-AC68|✅| 0b05:1817 | 802.11ac AC1900, 4×4 dual-band |
| Edimax EW-7833UAC |✅| 7392:a833 | 802.11ac AC1750, 4×4 dual-band |
| ASUS USB-AC68 (rev 2) | 🟨 | 0b05:1852 |  |
| Netgear A7000 | 🟨 | 0846:9054 |  |
| D-Link DWA-192 | 🟨 | 2001:331a |  |
| TP-Link Archer T9UH | 🟨 | 2357:0106 |  |
| TRENDnet TEW-809UB | 🟨 | 20f4:809a |  |
| Elecom WDB-867DU3S | 🟨 | 056e:400b |  |
| Elecom WDC-867DU3S | 🟨 | 056e:400d |  |

The class column is empty for the untested rows. I will add in the details when/if I get devices to test, or based on reports from other users.


---

## Known Limitations

A join can fail and need retrying, throughput is well below what the hardware
is sold on, and a couple of limits are Haiku's rather than the driver's. All of
it is measured rather than estimated, and written up in
**[docs/known-limitations.md](docs/known-limitations.md)** — worth a look
before you install.

---


## Documentation

Detailed docs live in [docs/](docs/).  Highlights:

- [Known limitations](docs/known-limitations.md) — what does not work yet, measured rather than estimated
- [Command-line usage](docs/command-line-usage.md) — scanning, `ifconfig join`, `wifi-join`, reconnecting after a reboot
- [Throughput](docs/throughput.md) — measured rates, and how to measure them without the routing trap
- [Architecture overview](docs/architecture.md) — how the driver is organized, threading model, where to start reading
- [Hardware initialization](docs/hardware-init.md) — power-on, EFUSE, MAC init, firmware load, PHY config
- [RX path](docs/rx-path.md) and [TX path](docs/tx-path.md) — frame conversion, EAPOL diversion, descriptor build
- [Wi-Fi management](docs/wifi-management.md) — scanning, auth + assoc state machine, H2C/C2H mailbox
- [Firmware](docs/firmware.md) — blob layout, IDDMA load procedure, the 8-byte trailer gotcha
- [IOCTL reference](docs/ioctl-reference.md) — the 80211 IOC handlers and what userland calls them
- [Building and deploying](docs/build-and-deploy.md) — cross-build recipe, deploy strategy, package gotchas

---

## Logging Bugs

Bugs are welcome! To log a bug, [please log it here in github as an issue](https://github.com/KevinAdams05/RTL8814AU_Haiku/issues), and include as much detail as possible.

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


---

## Contributing

PRs are welcome! However, please follow the guidelines below.

- Test all code changes on physical hardware before opening the PR!
- On your PR indicate which device you tested on, and include the device ID.
- Please adhere to the [style guide](docs/STYLE_GUIDE.md) (which is pretty much standard Haiku coding style).
- Make sure to update documentation as needed.

---

## Installation

### From a prebuilt `.hpkg`

1. Download `rtl8814au-<version>-x86_64.hpkg` from the [Releases page](https://github.com/KevinAdams05/RTL8814AU_Haiku/releases).
2. **Double-click it.** HaikuDepot opens and installs it for you.
3. **Reboot.** This is not optional for a kernel driver — packagefs keeps
   serving the previously loaded driver until the machine restarts, so until
   you reboot you are still running whatever was there before, or nothing.
4. Check the device turned up:

   ```
   ls /dev/net/rtl8814au/0
   ```

5. Connect to a network — see [How to Use](#how-to-use) below.

To uninstall, remove it in HaikuDepot and reboot.

If you would rather use the Terminal, `pkgman install
rtl8814au-<version>-x86_64.hpkg` does the same thing, and dropping the file
into `~/config/packages/` (for you) or `/system/packages/` (for everyone)
still works — useful on a headless machine.

### Building from source

See [docs/build-and-deploy.md](docs/build-and-deploy.md) for more detail.

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

### Connect to a network

Click the network icon in the **Deskbar** and pick your network, or use the
**Network** preferences panel if you want more control. Enter the passphrase
when asked. That is all most people need.

Both routes go through Haiku's `net_server`, which hands the join to
`wpa_supplicant` — the normal Haiku path, and the one to prefer. WPA2-PSK
(AES/CCMP) works on both 2.4 GHz and 5 GHz; **prefer 5 GHz where you have it**,
since receive is faster there.

Two caveats, both Haiku's rather than the driver's: **open (unencrypted)
networks** are torn down again immediately by `net_server`, and Haiku does not
**auto-connect at boot** — there is a workaround in
[docs/command-line-usage.md](docs/command-line-usage.md#reconnect-after-reboot).

### Doing it from the command line

Scanning by hand, `ifconfig join`, the bundled `wifi-join` helper,
disconnecting, and reconnecting automatically after a reboot are all covered in
**[docs/command-line-usage.md](docs/command-line-usage.md)**. You do not need
any of it for normal use.

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

**GNU General Public License, version 2.**

This driver is original code written for Haiku, not a port. But its register
semantics, power-on ordering, firmware-load procedure and descriptor layouts
were worked out with Realtek's GPL-licensed Linux vendor driver open alongside
as a reference, and several faults were found by diffing our behaviour against
it. Licensing under the same terms removes any question about how closely that
reference was consulted.

Note that this is a change from MIT, and it means the driver cannot be
contributed to Haiku's own tree, which is MIT-licensed. It is distributed as a
standalone package.
