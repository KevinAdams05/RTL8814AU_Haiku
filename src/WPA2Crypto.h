/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * WPA2Crypto.h — kernel-side cryptographic primitives for the
 * in-driver WPA2-PSK 4-way handshake.
 *
 * Standalone implementations: no allocations, no kernel-only
 * dependencies, no globals.  All functions are reentrant.
 *
 * Per IEEE 802.11i and RFC 3174 / RFC 3394.  Primitives are
 * intentionally minimal — encrypt-only AES is exposed since CCMP
 * data-frame encryption happens in the chip's hardware crypto
 * engine; AES decrypt is only here for RFC 3394 key unwrap on
 * the GTK delivered in EAPOL-Key Message 3.
 */

#ifndef _RTL8814AU_WPA2_CRYPTO_H
#define _RTL8814AU_WPA2_CRYPTO_H

#include <SupportDefs.h>


namespace wpa2_crypto {

// SHA-1 hash of `len` bytes at `data` into the 20-byte `digest`.
void sha1(const uint8* data, uint32 len, uint8 digest[20]);

// HMAC-SHA1 per RFC 2104.  Output is the full 20-byte MAC; callers
// that need a 16-byte MIC simply truncate.
void hmac_sha1(const uint8* key, uint32 keyLen,
	const uint8* msg, uint32 msgLen, uint8 mac[20]);

// PRF-X per IEEE 802.11i §8.5.1.1: prf_384 produces 48 bytes (the
// 384-bit pairwise expansion needed for CCMP PTK derivation).
//
//   PRF-384(K, A, B) = HMAC-SHA1(K, A || 0x00 || B || 0)
//                   || HMAC-SHA1(K, A || 0x00 || B || 1)
//                   || HMAC-SHA1(K, A || 0x00 || B || 2)[0..7]
void prf_384(const uint8* key, uint32 keyLen,
	const char* label, uint32 labelLen,
	const uint8* data, uint32 dataLen,
	uint8 output[48]);

// AES-128 ECB block cipher.  16-byte key, 16-byte block.
void aes128_encrypt(const uint8 key[16], const uint8 plaintext[16],
	uint8 ciphertext[16]);
void aes128_decrypt(const uint8 key[16], const uint8 ciphertext[16],
	uint8 plaintext[16]);

// PBKDF2-HMAC-SHA1 (RFC 2898 §5.2).  WPA2-PSK derives the PMK as
// PBKDF2(passphrase, SSID, 4096, 32) — 32 bytes out, ~8200 SHA-1
// blocks total.  Fast enough to run synchronously from a kernel
// ioctl handler (sub-millisecond on this hardware in practice).
void pbkdf2_hmac_sha1(const uint8* password, uint32 passwordLen,
	const uint8* salt, uint32 saltLen, uint32 iterations,
	uint8* output, uint32 outputLen);


// RFC 3394 AES Key Wrap unwrap.  Used to extract the GTK from
// EAPOL-Key Message 3.  `cipherLen` must be 8*n+8 for n >= 1.
// On success, plaintext output is `cipherLen - 8` bytes.
// Returns true if the integrity check (IV match) passed.
bool aes_unwrap(const uint8 kek[16], const uint8* cipher, uint32 cipherLen,
	uint8* plaintext);


// AES-CCMP frame decrypt for IEEE 802.11i (CCM with M=8, L=2).
//
// Used when the chip's hardware crypto engine declines to decrypt a
// CCMP frame (signaled via the SWDEC bit in the RX descriptor) — we
// fall back to in-driver software decrypt so DHCP / IP traffic on a
// WPA2-PSK link still works.
//
// Input frame layout (in-place):
//   [0..hdrLen-1]                802.11 header
//   [hdrLen..hdrLen+7]           CCMP IV header (PN0|PN1|0|KeyIDByte|PN2..PN5)
//   [hdrLen+8..frameLen-9]       encrypted body
//   [frameLen-8..frameLen-1]     8-byte MIC
//
// On success: encrypted body is replaced with plaintext in place,
// MIC verifies, returns `true`.  Caller is then responsible for
// stripping the 8-byte CCMP IV header (memmove plaintext over IV)
// and trimming the 8-byte MIC, and clearing FC[1] bit 6 (Protected).
//
// On any failure (bad length, MIC mismatch): returns `false`; the
// frame buffer's contents are unspecified (partial decrypt
// possible) and the caller must drop the frame.
//
// hdrLen: 24 for non-QoS data frames, 26 for QoS data.  We do not
// support 4-address frames (Addr4 present) here — those are AP/WDS
// scenarios that don't apply to a STA-mode driver.
bool ccmp_decrypt(const uint8 tk[16], uint8* frame, uint32 frameLen,
	uint32 hdrLen);


// AES-CCMP frame encrypt for IEEE 802.11i (CCM with M=8, L=2).
//
// Mirror of ccmp_decrypt.  Used when the chip's HW encrypt engine
// also fails to encrypt our outbound CCMP frames (same chip-state
// issue as RX) and we have to do it in software before submitting
// the frame on the data path.
//
// Input frame layout (in-place; buffer must have at least
// frameLen+16 bytes of capacity for the IV+MIC expansion):
//   [0..hdrLen-1]                802.11 header (FC[1] Protected=0)
//   [hdrLen..frameLen-1]         plaintext body (LLC + payload)
//
// On return (success): frame is rewritten in place to:
//   [0..hdrLen-1]                802.11 header (Protected bit set)
//   [hdrLen..hdrLen+7]           CCMP IV header (PN encoded + KeyID byte)
//   [hdrLen+8..frameLen+7]       encrypted body
//   [frameLen+8..frameLen+15]    8-byte encrypted MIC
// Returns the new frame length (= frameLen + 16) on success, 0 on
// failure (only failure mode is bad header length).
//
// pn is a 48-bit packet number that must increase monotonically per
// (key, peer) pair — caller is responsible for maintaining the
// counter and incrementing before each call.
//
// keyId is the CCMP key id (0 for pairwise, 1..3 for group).
uint32 ccmp_encrypt(const uint8 tk[16], uint8* frame, uint32 frameLen,
	uint32 hdrLen, uint64 pn, uint8 keyId);

}	// namespace wpa2_crypto


#endif	// _RTL8814AU_WPA2_CRYPTO_H
