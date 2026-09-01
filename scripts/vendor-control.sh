#!/bin/bash
# Control experiment: how often does the VENDOR Linux driver fail to join the
# same access point our Haiku driver fails on?
#
# Everything so far has compared our driver against itself, and every such
# comparison is drowning in block noise that swings from 0/10 to 10/10. Nothing
# has tested whether the ~30% failure rate is specific to our driver at all. If
# a mature driver on the same chip family shows a similar rate against this
# access point, the week's hunt has been aimed at the wrong thing.
#
# Managed mode only. Creating a monitor interface on this driver deadlocks the
# machine in cfg80211_rtw_add_virtual_intf and needs a reboot -- do not.
#
# The profile is never-default so it cannot steal the route used to reach the
# test machine, and it is deleted at the end so the passphrase is not left on
# disk.
#
# Usage: IFACE=wlx... SSID=... PASS=... vendor-control.sh [cycles]

set -u
IFACE=${IFACE:?set IFACE}
SSID=${SSID:?set SSID}
PASS=${PASS:?set PASS}
CYCLES=${1:-60}
CON=vendorctl

cleanup() {
	nmcli con down "$CON" >/dev/null 2>&1
	nmcli con delete "$CON" >/dev/null 2>&1
	echo "  profile deleted (passphrase not left on disk)"
}
trap cleanup EXIT

nmcli con delete "$CON" >/dev/null 2>&1
nmcli con add type wifi ifname "$IFACE" con-name "$CON" ssid "$SSID" \
	wifi-sec.key-mgmt wpa-psk wifi-sec.psk "$PASS" \
	ipv4.method disabled ipv6.method ignore \
	connection.autoconnect no >/dev/null 2>&1 || {
		echo "could not create the profile" >&2; exit 1; }

ok=0; fail=0
for i in $(seq 1 "$CYCLES"); do
	nmcli con down "$CON" >/dev/null 2>&1
	sleep 2
	if timeout 45 nmcli con up "$CON" >/dev/null 2>&1; then
		# Confirm it really associated rather than just returning 0.
		if iw dev "$IFACE" link 2>/dev/null | grep -qi "Connected to"; then
			ok=$((ok+1)); r=ok
		else
			fail=$((fail+1)); r="FAIL(no link)"
		fi
	else
		fail=$((fail+1)); r=FAIL
	fi
	printf "  cycle %2d/%d: %-13s  running: %d ok, %d failed\n" \
		"$i" "$CYCLES" "$r" "$ok" "$fail"
done

echo
echo "=== vendor driver, $SSID, $CYCLES join cycles ==="
echo "  $ok ok, $fail failed  ($(( 100 * fail / (ok+fail) ))% failure)"
