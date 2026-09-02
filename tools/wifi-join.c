/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the GNU General Public License version 2.
 *
 * wifi-join: bring up a WPA2-PSK network on the rtl8814au unofficial
 * driver.
 *
 * Why this tool exists: it worked first, and it is still the most
 * direct way in.  It hands the driver a passphrase and lets the
 * driver run the four-way handshake in the kernel, with no
 * net_server round trip — which makes it a better tool for
 * scripting and for diagnosing the driver in isolation than going
 * through the normal path.
 *
 * It is NOT the only way to connect.  The Deskbar network menu, the
 * Network preferences panel and `ifconfig <dev> join SSID password`
 * all work as of 0.3.0; they go through net_server and
 * wpa_supplicant, and that is the route to prefer for ordinary use.
 *
 * An earlier version of this comment claimed Haiku's stack does not
 * deliver ethertype 0x888E (EAPOL) frames to AF_LINK packet sockets,
 * and that this was why wpa_supplicant could not run the handshake.
 * That was wrong, and it is worth correcting in place because it was
 * an accusation against Haiku rather than against this driver.
 * Haiku delivers EAPOL correctly — binding an AF_LINK socket for
 * 0x888E succeeds.  What actually blocked the supplicant were three
 * faults here: the driver swallowed every EAPOL frame before the
 * supplicant could see one, it never implemented the ioctl that
 * installs the supplicant's keys, and it failed the SSID read-back
 * the supplicant performs immediately after associating.
 *
 * This tool accepts SSID + passphrase on the command line and hands
 * them to the driver via the driver-specific `IOC_HAIKU_JOIN` ioctl.  The driver runs PBKDF2, drives the full
 * 4-way handshake, and installs the keys.  After this tool reports
 * success, the network stack can be brought up over the link via
 * `ifconfig`, `dhcpconfig`, or whatever else manages your IP config.
 *
 * The fd MUST remain open for the lifetime of the connection — the
 * driver tears down its RX loop on the last `close()`.  This tool
 * forks before exiting and the child holds the fd open indefinitely;
 * killing the child (with `kill <PID>`) tells the driver to disconnect.
 *
 * Usage:
 *   wifi-join <dev> <ssid> <passphrase> [bssid]
 *     dev:        e.g. /dev/net/rtl8814au/0
 *     ssid:       network name (1..32 chars)
 *     passphrase: ASCII passphrase (8..63 chars)
 *     bssid:      optional aa:bb:cc:dd:ee:ff; zeros = lookup by SSID
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/sockio.h>
#include <errno.h>


/* Mirror of the driver-side definitions in WiFiIoctl.h.  Kept private
 * here because the userland headers from /system/develop/headers
 * don't ship driver-specific structs. */
#ifndef IFNAMSIZ
#	define IFNAMSIZ 32
#endif

#define SIOCS80211				(SIOCEND + 234)
#define IEEE80211_IOC_HAIKU_JOIN	0x6002

struct ieee80211req {
	char			i_name[IFNAMSIZ];
	unsigned short	i_type;
	short			i_val;
	unsigned short	i_len;
	void*			i_data;
};

struct rtl_haiku_join_psk {
	unsigned char	jp_bssid[6];
	unsigned char	jp_ssid_len;
	unsigned char	jp_pad;
	unsigned char	jp_ssid[32];
	unsigned char	jp_passphrase_len;
	unsigned char	jp_pad2[3];
	unsigned char	jp_passphrase[64];
};


static int
parse_bssid(const char* s, unsigned char out[6])
{
	unsigned int b[6];
	if (sscanf(s, "%x:%x:%x:%x:%x:%x",
			&b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
		return -1;
	for (int i = 0; i < 6; i++) {
		if (b[i] > 0xFF) return -1;
		out[i] = (unsigned char)b[i];
	}
	return 0;
}


static void
usage(const char* argv0)
{
	fprintf(stderr,
		"usage: %s <dev> <ssid> <passphrase> [bssid]\n"
		"  dev:        e.g. /dev/net/rtl8814au/0\n"
		"  ssid:       1..32 chars\n"
		"  passphrase: 8..63 ASCII chars\n"
		"  bssid:      optional, aa:bb:cc:dd:ee:ff (zeros => lookup by SSID)\n"
		"\n"
		"After a successful join this tool forks into the background\n"
		"and holds the device fd open so the connection stays alive.\n"
		"To disconnect, kill the background process: `kill <pid>`.\n",
		argv0);
}


int
main(int argc, char** argv)
{
	if (argc < 4) {
		usage(argv[0]);
		return 1;
	}
	const char* dev = argv[1];
	const char* ssid = argv[2];
	const char* passphrase = argv[3];

	struct rtl_haiku_join_psk req;
	memset(&req, 0, sizeof(req));

	size_t slen = strlen(ssid);
	size_t plen = strlen(passphrase);
	if (slen < 1 || slen > 32) {
		fprintf(stderr, "bad ssid length (%zu): must be 1..32 chars\n", slen);
		return 1;
	}
	if (plen < 8 || plen > 63) {
		fprintf(stderr, "bad passphrase length (%zu): must be 8..63 chars\n",
			plen);
		return 1;
	}
	memcpy(req.jp_ssid, ssid, slen);
	req.jp_ssid_len = (unsigned char)slen;
	memcpy(req.jp_passphrase, passphrase, plen);
	req.jp_passphrase_len = (unsigned char)plen;

	if (argc >= 5) {
		if (parse_bssid(argv[4], req.jp_bssid) < 0) {
			fprintf(stderr, "bad bssid format: %s\n", argv[4]);
			return 1;
		}
	}

	int fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		return 1;
	}

	struct ieee80211req ireq;
	memset(&ireq, 0, sizeof(ireq));
	strncpy(ireq.i_name, dev, IFNAMSIZ - 1);
	ireq.i_type = IEEE80211_IOC_HAIKU_JOIN;
	ireq.i_data = &req;
	ireq.i_len = sizeof(req);

	if (ioctl(fd, SIOCS80211, &ireq, sizeof(ireq)) < 0) {
		fprintf(stderr, "SIOCS80211 IOC_HAIKU_JOIN: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	/* Fork into the background so the calling shell gets its prompt
	 * back immediately — typical pattern: run wifi-join, then
	 * ifconfig/dhcpconfig over the live link.  The child keeps the
	 * fd open; closing it would tear down the in-driver RX loop and
	 * end the session. */
	pid_t pid = fork();
	if (pid < 0) {
		fprintf(stderr, "fork: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	if (pid > 0) {
		/* Parent.  Report success + child pid and exit; the kernel
		 * keeps the fd alive in the child. */
		printf("wifi-join: handshake kicked off for SSID '%s' on %s\n",
			ssid, dev);
		printf("wifi-join: background pid %d holds the device open; "
			"kill it to disconnect.\n", (int)pid);
		printf("wifi-join: bring up IP next, e.g. `ifconfig %s auto-config`\n",
			dev);
		return 0;
	}

	/* Child.  Detach from controlling terminal and wait forever.  A
	 * SIGTERM (default kill) cleanly closes the fd, which in turn
	 * makes the driver disconnect. */
	setsid();
	signal(SIGHUP, SIG_IGN);
	for (;;)
		pause();

	/* unreachable */
	return 0;
}
