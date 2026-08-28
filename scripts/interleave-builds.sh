#!/bin/bash
# Compare driver builds by interleaving them in blocks within one session.
#
# Why interleaved rather than one run each: the join failure rate on this
# hardware drifts badly over hours. The same build has measured 4/20 one
# afternoon, 12/30 the same evening, and 12/18 partway through a single run.
# Run each build once and the comparison credits that drift to whichever build
# happened to run when conditions were worse -- which is a confident wrong
# answer, the expensive kind. Alternating in short blocks exposes every build to
# the same conditions.
#
# Each block costs a reboot: the driver is a kernel add-on loaded from
# packagefs and cannot be swapped live.
#
# Expects $BUILDS_DIR/variant<LABEL>.hpkg for each label given.
#
# Usage: BUILDS_DIR=<dir> HOST=user@<ip> SSID=<network> \
#            interleave-builds.sh <passphrase> <per-block> <rounds> <label>...

set -u

BUILDS_DIR=${BUILDS_DIR:?set BUILDS_DIR to the directory holding variant*.hpkg}
HOST=${HOST:?set HOST to user@<test-machine-ip>}
SSID=${SSID:?set SSID to the network to join}
DEV=${DEV:-/dev/net/rtl8814au/0}
PKG=${PKG:-rtl8814au-0.3.0-1-x86_64.hpkg}
PASS=${1:?usage: interleave-builds.sh <passphrase> <per-block> <rounds> <label>...}
PER=${2:?per-block attempt count}
ROUNDS=${3:?number of rounds}
shift 3
LABELS=("$@")
[ ${#LABELS[@]} -ge 2 ] || { echo "give at least two build labels" >&2; exit 1; }

IP=${HOST#*@}
RESULTS=$BUILDS_DIR/interleave-results.txt
: > "$RESULTS"

install_variant() {
	local label=$1 file="$BUILDS_DIR/variant$1.hpkg"
	[ -f "$file" ] || { echo "  missing $file" >&2; return 1; }
	scp -o BatchMode=yes "$file" "$HOST:/boot/home/$PKG" >/dev/null 2>&1 || return 1
	ssh -o BatchMode=yes "$HOST" \
		"rm -f /boot/system/packages/rtl8814au-*.hpkg \
		 && cp /boot/home/$PKG /boot/system/packages/ \
		 && sync && nohup shutdown -r >/dev/null 2>&1 &" >/dev/null 2>&1
	# Wait for it to actually go down first. Checking only for "it answers"
	# succeeds against the system that has not rebooted yet, and the block then
	# runs against the wrong build -- which is the whole thing this script
	# exists to avoid.
	for _ in $(seq 1 60); do
		ping -c1 -W1 "$IP" >/dev/null 2>&1 || break
		sleep 2
	done
	for _ in $(seq 1 90); do
		ssh -o BatchMode=yes -o ConnectTimeout=8 "$HOST" "ls $DEV" \
			>/dev/null 2>&1 && return 0
		sleep 4
	done
	return 1
}

run_block() {
	local label=$1 n=$2 ok=0 fail=0 verdict okc
	for _ in $(seq 1 "$n"); do
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
	echo "$label $ok $fail" >> "$RESULTS"
	echo "  build $label: $ok ok, $fail failed"
}

for round in $(seq 1 "$ROUNDS"); do
	for label in "${LABELS[@]}"; do
		echo "=== round $round, build $label: installing and rebooting ==="
		if install_variant "$label"; then
			run_block "$label" "$PER"
		else
			echo "  ERROR: build $label did not come back; block skipped" >&2
		fi
	done
done

echo
echo "=== totals ==="
awk '{ok[$1]+=$2; fail[$1]+=$3}
     END {for (v in ok) printf "  build %-4s %2d ok, %2d failed  (%3.0f%% failure)\n",
          v, ok[v], fail[v], 100*fail[v]/(ok[v]+fail[v])}' "$RESULTS" | sort
