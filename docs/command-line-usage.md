# Command-line usage

Most people do not need this file. Connecting through the **Deskbar network
menu** or the **Network preferences panel** works and is the route to prefer --
see the [README](../README.md).

What is here is for scripting, for headless machines, and for diagnosing the
driver in isolation: scanning by hand, joining with `ifconfig`, the bundled
`wifi-join` helper, disconnecting, and getting a connection back after a
reboot.

Throughout, the device is `/dev/net/rtl8814au/0` -- substitute `/1`, `/2` and
so on if you have more than one adapter.

## Scan for networks

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

## Join an open network

Haiku's standard `ifconfig` flow, no special tools needed:

```
ifconfig /dev/net/rtl8814au/0 join <SSID>
ifconfig /dev/net/rtl8814au/0 auto-config
```

**This may not hold.** Open networks go through `net_server`, which currently
tears the association down again immediately — a Haiku-side limitation rather
than the driver's, noted in [known-limitations.md](known-limitations.md). If it
drops, there is no command-line workaround; encrypted networks are unaffected.

## Join a WPA2-PSK network

Two command-line routes, and they get there differently. `ifconfig` goes
through Haiku's `net_server`, which hands the join to `wpa_supplicant`:

```
ifconfig /dev/net/rtl8814au/0 join <SSID> <passphrase>
ifconfig /dev/net/rtl8814au/0 auto-config
```

`wifi-join` instead hands the passphrase to the driver and runs the handshake
in the kernel, with no `net_server` round trip:

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

## Disconnect

Kill the background `wifi-join` process:

```
kill <pid>
```

This closes the device fd, which tells the driver to tear down the
link.

## Reconnect after reboot

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

## Why are there two ways to connect?

Both work, and they get there differently.

The **Deskbar and `ifconfig join`** route goes through Haiku's `net_server`,
which always hands a wireless join to `wpa_supplicant`. The supplicant runs
the four-way handshake and passes the resulting keys down to the driver. This
is the normal Haiku path and the one to prefer.

**`wifi-join`** instead hands the driver the passphrase directly and lets it
run the handshake itself, in the kernel. It exists because it worked first,
and it is still useful: it needs no `net_server` round trip, which makes it a
better tool for scripting and for diagnosing the driver in isolation.


One genuine Haiku-side limitation remains: **open (unencrypted) networks**
have to go through `net_server`, and it tears the association down again
immediately. That one is not the driver's doing.

See [docs/wpa2-in-driver.md](wpa2-in-driver.md) for the full
design.
