#!/bin/bash
# Does scanning immediately before every join cause the failures?
#
# Why this is the experiment. The vendor Linux driver joins this access point
# 60 times out of 60 with zero variance, where ours fails about a third of the
# time and swings from 0/10 to 10/10 between blocks -- so the instability is
# ours, not the air (scripts/vendor-control.sh established that). The most
# conspicuous difference between the two harnesses is that ours sweeps all 42
# channels immediately before every single join and the vendor's does not; it
# joins from cached scan results.
#
# A scan is not a passive operation on this chip. It changes channel 42 times,
# switches band, reloads AGC tables and rewrites the RFE pinmux, and the
# per-join state snapshot is taken *after* it -- so a chip left unsettled by a
# sweep would be invisible to every measurement taken so far, would be bursty,
# and would look band-independent. That fits everything.
#
# Single variable. Both arms cycle the interface down and up between attempts,
# exactly as before; the only difference is whether a full sweep runs before
# each join or once per block.
#
#   SCAN    scan, wait, join      -- what every measurement so far has done
#   NOSCAN  scan once per block, then join from the cached BSS list
#
# Usage: HOST=user@<ip> SSID=<net> PASS=<passphrase> \
#            interleave-scan-vs-noscan.sh <per-block> <rounds>

set -u
HOST=${HOST:?set HOST}
SSID=${SSID:?set SSID}
PASS=${PASS:?set PASS}
DEV=${DEV:-/dev/net/rtl8814au/0}
PER=${1:-10}
ROUNDS=${2:-6}
OUT=${OUT:-$(dirname "$0")/../scan-results.txt}
: > "$OUT"

rsh() { timeout "$1" ssh -o BatchMode=yes -o ConnectTimeout=15 "$HOST" "$2" 2>/dev/null; }

# One join. $1 = "scan" or "noscan".
attempt() {
	timeout 140 ssh -o BatchMode=yes -o ConnectTimeout=15 "$HOST" \
		"sh -s '$SSID' '$PASS' '$DEV' '$1'" 2>/dev/null <<'REMOTE'
SSID=$1; PASS=$2; DEV=$3; MODE=$4
M=$(wc -l < /var/log/syslog)
if [ "$MODE" = scan ]; then
	timeout 20 ifconfig $DEV scan >/dev/null 2>&1
	sleep 22
fi
timeout 40 wifi-join $DEV "$SSID" "$PASS" >/dev/null 2>&1
sleep 10
V=$(awk -v s=$M 'NR>s' /var/log/syslog | grep -a rtl8814au)
echo "ok=$(echo "$V" | grep -acE 'CCMP enabled|GTK installed|MIC MATCH') resolved=$(echo "$V" | grep -ac "_DoJoin '")"
timeout 30 ifconfig $DEV down >/dev/null 2>&1
timeout 20 ifconfig $DEV up >/dev/null 2>&1
REMOTE
}

run_block() {
	local mode=$1 n=$2 ok=0 fail=0 unresolved=0 v c r
	# NOSCAN still needs the BSS in the list once, or nothing can resolve.
	if [ "$mode" = noscan ]; then
		rsh 60 "timeout 20 ifconfig $DEV scan >/dev/null 2>&1; sleep 23"
	fi
	for _ in $(seq 1 "$n"); do
		v=$(attempt "$mode")
		if [ -z "$v" ]; then
			echo "ABORT: no response from $HOST -- possible panic" >&2
			echo "$mode $ok $fail" >> "$OUT"; exit 2
		fi
		c=$(echo "$v" | sed -n 's/.*ok=\([0-9]*\).*/\1/p')
		r=$(echo "$v" | sed -n 's/.*resolved=\([0-9]*\).*/\1/p')
		# A join that never resolved the BSS is a stale-cache miss, not a
		# handshake failure. Counting it as one would flatter the SCAN arm.
		[ "${r:-0}" -eq 0 ] && unresolved=$((unresolved+1))
		if [ "${c:-0}" -gt 0 ]; then ok=$((ok+1)); else fail=$((fail+1)); fi
	done
	echo "$mode $ok $fail $unresolved" >> "$OUT"
	echo "  $mode: $ok ok, $fail failed ($unresolved never resolved the BSS)"
}

for round in $(seq 1 "$ROUNDS"); do
	echo "=== round $round ==="
	for mode in scan noscan; do
		if rsh 30 "ifconfig $DEV 2>&1 | grep -q 'Interface not found'"; then
			echo "ABORT: interface unregistered; reboot needed" >&2; exit 1
		fi
		run_block "$mode" "$PER"
	done
done

echo
echo "=== totals ==="
awk '{ok[$1]+=$2; fail[$1]+=$3; un[$1]+=$4}
     END {for (m in ok) printf "  %-7s %2d ok, %2d failed (%3.0f%%), %d unresolved\n",
          m, ok[m], fail[m], 100*fail[m]/(ok[m]+fail[m]), un[m]}' "$OUT" | sort
