#!/bin/bash
# Capture the air during 5 GHz joins, keeping only the failures -- with no
# reboot between attempts.
#
# The predecessor of this script rebooted shredder for every attempt, because
# `ifconfig down` used to hang and the interface could not be brought back.
# Both faults are fixed, so an attempt is now scan -> join -> check -> down ->
# up, about 70 seconds. That matters for more than speed: the very first
# reboot-free run reproduced a reason-15 failure that 28 reboot-based attempts
# never produced once, which suggests reboot-cycling was changing the
# conditions rather than sampling them.
#
# The question this is here to answer: when the handshake dies with reason 15
# after we have built M2 more than once, does our M2 reach the air at all? The
# driver says it submitted the frame and the USB write completed, and the
# submissions are byte-identical to ones that work, so the air is the only
# place left to tell a lost frame from a rejected one.
#
# Needs a monitor-mode interface on the capture host, parked on the access
# point's channel, and cap_net_admin,cap_net_raw on tcpdump and iw so nothing
# has to run as root.
#
# Note the passphrase reaches the test machine as a shell argument, so it is
# visible in that machine's process list while an attempt runs. That is
# acceptable on a test bench and on a network you own; it is not a way to
# handle a credential you care about.
#
# Usage: MON=mon0 HOST=user@<ip> SSID=<network> \
#            air-noreboot.sh <passphrase> [attempts]

set -u

MON=${MON:-mon0}
HOST=${HOST:?set HOST to user@<test-machine-ip>}
SSID=${SSID:?set SSID to the network to join}
DEV=${DEV:-/dev/net/rtl8814au/0}
PASS=${1:?usage: air-noreboot.sh <passphrase> [attempts]}
ATTEMPTS=${2:-8}
OUT=$PWD/air-nr
mkdir -p "$OUT"

rsh() { timeout "$1" ssh -o BatchMode=yes -o ConnectTimeout=15 "$HOST" "$2" 2>/dev/null; }

cleanup() { pkill -f "tcpdump -i $MON" 2>/dev/null; echo "==> stopped"; }
trap cleanup EXIT

if ! iw dev "$MON" info >/dev/null 2>&1; then
	echo "ERROR: $MON is gone; monitor mode needs rebuilding by hand" >&2
	exit 1
fi

kept=0
for a in $(seq 1 "$ATTEMPTS"); do
	PCAP="$OUT/attempt-$a.pcap"
	rm -f "$PCAP"
	tcpdump -i "$MON" -w "$PCAP" -s 0 -Z "$USER" >/dev/null 2>&1 &
	CAP=$!
	sleep 2
	if ! kill -0 "$CAP" 2>/dev/null; then
		echo "ERROR: tcpdump would not start" >&2
		exit 1
	fi

	VERDICT=$(timeout 120 ssh -o BatchMode=yes -o ConnectTimeout=15 "$HOST" \
		"sh -s '$SSID' '$PASS' '$DEV'" 2>/dev/null <<'REMOTE'
SSID=$1; PASS=$2; DEV=$3
M=$(wc -l < /var/log/syslog)
timeout 20 ifconfig $DEV scan >/dev/null 2>&1; sleep 22
timeout 40 wifi-join $DEV "$SSID" "$PASS" >/dev/null 2>&1
sleep 10
V=$(awk -v s=$M 'NR>s' /var/log/syslog | grep -a rtl8814au)
A=$(echo "$V" | grep -ac "ASSOCIATED")
M2=$(echo "$V" | grep -ac "M2 handed to the chip")
OK=$(echo "$V" | grep -acE "CCMP enabled|GTK installed|MIC MATCH")
R=$(echo "$V" | grep -a "DEAUTH" | grep -a "toUs=1" | tail -1 | sed 's/.*\(reason=[0-9]*\).*/\1/')
echo "assoc=$A M2=$M2 ok=$OK ${R}"
echo "$V" | grep -aE "ASSOCIATED|EAPOL|M2|DEAUTH|txdesc|TX wait" | tail -25 > /boot/home/last-attempt.txt
timeout 30 ifconfig $DEV down >/dev/null 2>&1
timeout 20 ifconfig $DEV up >/dev/null 2>&1
REMOTE
)
	sleep 1
	kill "$CAP" 2>/dev/null; wait "$CAP" 2>/dev/null

	OKC=$(echo "$VERDICT" | sed -n 's/.*ok=\([0-9]*\).*/\1/p')
	if [ "${OKC:-0}" -gt 0 ]; then
		rm -f "$PCAP"
		echo "attempt $a: OK        ($VERDICT)  [capture discarded]"
	else
		kept=$((kept+1))
		rsh 30 'cat /boot/home/last-attempt.txt' > "$OUT/attempt-$a.syslog"
		SZ=$(stat -c %s "$PCAP" 2>/dev/null || echo 0)
		echo "attempt $a: **FAILED** ($VERDICT)  [kept $(basename "$PCAP"), $SZ bytes]"
	fi
done
echo "==> $kept failure(s) captured in $OUT"
