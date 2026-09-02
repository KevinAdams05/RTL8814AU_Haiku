#!/bin/sh
# Sustained internet-download test for the rtl8814au driver.
#
# Runs entirely on shredder so that dropping the wired interface cannot strand
# the test: the download, the verification and the restore all happen here.
# Wired has to go down because shredder's default route prefers it, so an
# internet download would otherwise never touch the wifi at all -- and the
# byte counters, not the transfer rate, are what prove which path was used.
#
# A watchdog child restores wired unconditionally after 30 minutes, whatever
# happens to the main flow. Do not remove it.

DEV=/dev/net/rtl8814au/0
WIRED=/dev/net/ipro1000/2
DIR=/boot/home/perf
LOG=$DIR/iso-test.log

rx_bytes() {
	ifconfig $DEV 2>&1 | grep -a "Receive:" \
		| grep -oE "[0-9]+ bytes" | grep -oE "[0-9]+"
}

mkdir -p $DIR
cd $DIR || exit 1
rm -f $DIR/window-open $DIR/test-done $DIR/*.iso
: > $LOG

( sleep 1800
	ifconfig $WIRED up
	sleep 5
	ifconfig $WIRED auto-config
	echo "WATCHDOG restored wired" >> $LOG
) &

echo "== wired down at $(date) ==" >> $LOG
ifconfig $WIRED down
touch $DIR/window-open

BASE=https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/x86_64

for spec in \
	"alpine-standard-3.21.7-x86_64.iso f1a3a93628927b382d31e7b173b12801342641f711d8c591b88582be1b29954a" \
	"alpine-extended-3.21.7-x86_64.iso 648b340f836877716ae9256097e6beb801d413dc8f9d4d9815c60197d3543e18"
do
	NAME=$(echo $spec | awk '{print $1}')
	WANT=$(echo $spec | awk '{print $2}')

	R0=$(rx_bytes)
	T0=$(date +%s)
	wget -q -T 60 -t 3 --read-timeout=120 -O $DIR/$NAME $BASE/$NAME
	RC=$?
	T1=$(date +%s)
	R1=$(rx_bytes)

	SZ=0
	[ -f $DIR/$NAME ] && SZ=$(ls -l $DIR/$NAME | awk '{print $5}')
	GOT=$(sha256sum $DIR/$NAME 2>/dev/null | cut -d' ' -f1)

	echo "file:   $NAME" >> $LOG
	echo "  wget rc:  $RC" >> $LOG
	echo "  bytes:    $SZ" >> $LOG
	echo "  seconds:  $((T1 - T0))" >> $LOG
	echo "  wifi rx:  $((R1 - R0))" >> $LOG
	if [ "$GOT" = "$WANT" ]; then
		echo "  sha256:   MATCH" >> $LOG
	else
		echo "  sha256:   MISMATCH ($GOT)" >> $LOG
	fi
	ifconfig $DEV 2>&1 | grep -aE "Receive:|Transmit:" >> $LOG
	rm -f $DIR/$NAME
done

echo "== restoring wired at $(date) ==" >> $LOG
ifconfig $WIRED up
sleep 5
ifconfig $WIRED auto-config
rm -f $DIR/window-open
touch $DIR/test-done
echo "== done ==" >> $LOG
