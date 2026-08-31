#!/bin/bash
# Interleave join attempts across two networks, to test whether anything about
# the network -- the access point, the band, the radio -- is a participant in the
# reason-15 defect.
#
# Budget six blocks per network. Three is not enough: two independent
# three-block comparisons of the same two builds once pointed in opposite
# directions, each decided by a single anomalous block.
#
# No reboot is needed between blocks -- only the SSID changes, not the driver --
# so blocks can be short and the two networks see near-identical conditions.
# That matters because the failure rate here drifts enough that sequential
# comparison is worthless.
#
# Passphrases are taken from the environment and never written anywhere.
#
# Usage: HOST=user@<ip> SSID_A=.. PASS_A=.. SSID_B=.. PASS_B=.. \
#            ssid-interleave.sh <per-block> <rounds>
set -u
HOST=${HOST:?}; DEV=${DEV:-/dev/net/rtl8814au/0}
SSID_A=${SSID_A:?}; PASS_A=${PASS_A:?}
SSID_B=${SSID_B:?}; PASS_B=${PASS_B:?}
PER=${1:-4}; ROUNDS=${2:-4}
OUT=${OUT:-$(dirname "$0")/../network-results.txt}
: > "$OUT"

attempt() {
	local ssid=$1 pass=$2
	timeout 140 ssh -o BatchMode=yes -o ConnectTimeout=15 "$HOST" \
		"sh -s '$ssid' '$pass' '$DEV'" 2>/dev/null <<'REMOTE'
SSID=$1; PASS=$2; DEV=$3
M=$(wc -l < /var/log/syslog)
timeout 20 ifconfig $DEV scan >/dev/null 2>&1; sleep 22
timeout 40 wifi-join $DEV "$SSID" "$PASS" >/dev/null 2>&1
sleep 10
V=$(awk -v s=$M 'NR>s' /var/log/syslog | grep -a rtl8814au)
echo "ok=$(echo "$V" | grep -acE 'CCMP enabled|GTK installed|MIC MATCH') m2=$(echo "$V" | grep -ac 'built M2') r15=$(echo "$V" | grep -ac 'reason=15')"
timeout 30 ifconfig $DEV down >/dev/null 2>&1
timeout 20 ifconfig $DEV up >/dev/null 2>&1
REMOTE
}

for round in $(seq 1 "$ROUNDS"); do
	for which in A B; do
		if [ "$which" = A ]; then ssid=$SSID_A; pass=$PASS_A; else ssid=$SSID_B; pass=$PASS_B; fi
		ok=0; fail=0
		# A wedged interface fails every attempt for an unrelated reason and
		# silently inflates the rate. It already produced one false finding
		# here, so stop rather than measure noise.
		if timeout 30 ssh -o BatchMode=yes "$HOST" \
			"ifconfig $DEV 2>&1 | grep -q 'Interface not found'" 2>/dev/null; then
			echo "ABORT: interface no longer registered; reboot needed" >&2
			exit 1
		fi
		for _ in $(seq 1 "$PER"); do
			v=$(attempt "$ssid" "$pass")
			c=$(echo "$v" | sed -n 's/.*ok=\([0-9]*\).*/\1/p')
			# Tell "the join failed" apart from "the machine is gone".
			#
			# The attempt returns nothing at all if ssh could not run --
			# which is what a kernel panic looks like from here, and this
			# machine has hit one. Counting that as a join failure would
			# silently fill the rest of the run with phantom failures and
			# there would be no way to tell afterwards.
			if [ -z "$v" ]; then
				echo "ABORT: no response from $HOST -- it may have panicked." >&2
				echo "       Results so far are usable; later ones would not" >&2
				echo "       have been." >&2
				echo "$ssid $ok $fail" >> "$OUT"
				exit 2
			fi
			if [ "${c:-0}" -gt 0 ]; then ok=$((ok+1)); else fail=$((fail+1)); fi
		done
		echo "$ssid $ok $fail" >> "$OUT"
		echo "  round $round  $ssid: $ok ok, $fail failed"
	done
done
echo
echo "=== totals ==="
awk '{ok[$1]+=$2; fail[$1]+=$3}
     END {for (s in ok) printf "  %-22s %2d ok, %2d failed  (%3.0f%% failure)\n",
          s, ok[s], fail[s], 100*fail[s]/(ok[s]+fail[s])}' "$OUT" | sort
