#!/bin/bash
# Compare two USB adapters against one access point, interleaved in blocks.
#
# The question this answers: the response-timing fix took one adapter from 20%
# join failures to 1.7% and did nothing measurable for the other, with no
# per-device code in between. Is that a real adapter difference, or were the two
# simply measured at different hours? The failure rate here drifts enough that
# only interleaving can tell them apart.
#
# Swapping needs a human, so this does not try to hide that. It runs a block,
# then prints SWAP NEEDED and waits -- polling until the other adapter appears,
# for as long as it takes. Swap whenever convenient; the run continues on its
# own.
#
# It labels each block by the adapter it actually detects, never by whichever
# one it expected. A mislabelled block would be worse than no data, and after a
# hot swap the driver keeps the previous adapter's EFUSE state, so every block
# starts with a reboot.
#
# Usage: HOST=user@<ip> SSID=<net> PASS=<passphrase> \
#            interleave-adapters.sh <per-block> <rounds>

set -u
HOST=${HOST:?set HOST to user@<test-machine-ip>}
SSID=${SSID:?set SSID}
PASS=${PASS:?set PASS}
DEV=${DEV:-/dev/net/rtl8814au/0}
PER=${1:-10}
ROUNDS=${2:-3}
IP=${HOST#*@}
OUT=$(dirname "$0")/../adapter-results.txt
[ -w "$(dirname "$OUT")" ] || OUT=/tmp/adapter-results.txt
: > "$OUT"

ASUS=0b05:1817
EDIMAX=7392:a833

rsh() { timeout "$1" ssh -o BatchMode=yes -o ConnectTimeout=15 "$HOST" "$2" 2>/dev/null; }

detect() {
	# Print a short label for whichever adapter is plugged in, or nothing.
	local ids
	ids=$(rsh 40 'listusb 2>/dev/null' | grep -oiE "0b05:1817|7392:a833" | head -1)
	case "$ids" in
		0b05:1817) echo ASUS ;;
		7392:a833) echo EDIMAX ;;
		*) echo "" ;;
	esac
}

reboot_and_wait() {
	rsh 30 'nohup shutdown -r >/dev/null 2>&1 &' >/dev/null 2>&1
	for _ in $(seq 1 60); do
		ping -c1 -W1 "$IP" >/dev/null 2>&1 || break
		sleep 2
	done
	for _ in $(seq 1 90); do
		rsh 20 "ls $DEV" >/dev/null 2>&1 && return 0
		sleep 4
	done
	return 1
}

run_block() {
	local label=$1 n=$2 ok=0 fail=0 verdict okc
	for _ in $(seq 1 "$n"); do
		# A wedged interface fails every attempt for an unrelated reason.
		if rsh 30 "ifconfig $DEV 2>&1 | grep -q 'Interface not found'"; then
			echo "  interface wedged mid-block; rebooting and continuing"
			reboot_and_wait || { echo "  ERROR: did not come back" >&2; return 1; }
		fi
		verdict=$(timeout 140 ssh -o BatchMode=yes -o ConnectTimeout=15 "$HOST" \
			"sh -s '$SSID' '$PASS' '$DEV'" 2>/dev/null <<'REMOTE'
SSID=$1; PASS=$2; DEV=$3
M=$(wc -l < /var/log/syslog)
timeout 20 ifconfig $DEV scan >/dev/null 2>&1; sleep 22
timeout 40 wifi-join $DEV "$SSID" "$PASS" >/dev/null 2>&1
sleep 10
V=$(awk -v s=$M 'NR>s' /var/log/syslog | grep -a rtl8814au)
echo "ok=$(echo "$V" | grep -acE 'CCMP enabled|GTK installed|MIC MATCH')"
timeout 30 ifconfig $DEV down >/dev/null 2>&1
timeout 20 ifconfig $DEV up >/dev/null 2>&1
REMOTE
)
		okc=$(echo "$verdict" | sed -n 's/.*ok=\([0-9]*\).*/\1/p')
		if [ "${okc:-0}" -gt 0 ]; then ok=$((ok+1)); else fail=$((fail+1)); fi
	done
	echo "$label $ok $fail" >> "$OUT"
	echo "  block done -- $label: $ok ok, $fail failed"
}

wait_for_swap() {
	local from=$1
	echo ""
	echo "=================== SWAP NEEDED ==================="
	echo "  Remove the $from and plug in the other adapter."
	echo "  No rush -- this waits, and reboots by itself after."
	echo "==================================================="
	local seen
	for _ in $(seq 1 2400); do          # up to ~2 hours
		seen=$(detect)
		if [ -n "$seen" ] && [ "$seen" != "$from" ]; then
			echo "  detected $seen"
			return 0
		fi
		sleep 3
	done
	echo "  ERROR: no swap seen; giving up" >&2
	return 1
}

current=$(detect)
[ -n "$current" ] || { echo "no RTL8814AU detected at all" >&2; exit 1; }
echo "starting with $current in the machine"

for round in $(seq 1 "$ROUNDS"); do
	for half in 1 2; do
		echo "=== round $round, $current: rebooting for a clean attach ==="
		reboot_and_wait || { echo "ERROR: did not come back" >&2; exit 1; }
		got=$(detect)
		if [ "$got" != "$current" ]; then
			echo "  note: expected $current, found $got -- labelling by what is there"
			current=$got
		fi
		run_block "$current" "$PER" || exit 1
		# Skip the final swap; the run is over.
		if [ "$round" -eq "$ROUNDS" ] && [ "$half" -eq 2 ]; then break; fi
		wait_for_swap "$current" || exit 1
		current=$(detect)
	done
done

echo ""
echo "=== totals ==="
awk '{ok[$1]+=$2; fail[$1]+=$3}
     END {for (a in ok) printf "  %-8s %2d ok, %2d failed  (%3.0f%% failure)\n",
          a, ok[a], fail[a], 100*fail[a]/(ok[a]+fail[a])}' "$OUT" | sort
