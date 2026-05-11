/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * WPA2Crypto.cpp — kernel-side crypto primitives for in-driver WPA2.
 *
 * SHA-1 and HMAC-SHA1: RFC 3174 / RFC 2104 reference implementations.
 * AES-128: compact textbook implementation using S-box + inverse S-box;
 *   no precomputed T-tables to keep the static-data footprint small.
 * PRF-384: IEEE 802.11i §8.5.1.1.
 * AES key unwrap: RFC 3394 §2.2.2.
 *
 * No allocations, no globals other than const tables.
 */


#include "WPA2Crypto.h"

#include <KernelExport.h>
#include <string.h>


namespace wpa2_crypto {


// ---------------------------------------------------------------------------
// SHA-1 (RFC 3174)
// ---------------------------------------------------------------------------

struct Sha1Ctx {
	uint32	state[5];
	uint64	bitCount;
	uint8	buffer[64];
};


static inline uint32
rotl32(uint32 x, uint32 n)
{
	return (x << n) | (x >> (32 - n));
}


static void
sha1_transform(uint32 state[5], const uint8 block[64])
{
	uint32 W[80];
	for (uint32 i = 0; i < 16; i++) {
		W[i] = ((uint32)block[i * 4] << 24)
			| ((uint32)block[i * 4 + 1] << 16)
			| ((uint32)block[i * 4 + 2] << 8)
			| (uint32)block[i * 4 + 3];
	}
	for (uint32 i = 16; i < 80; i++)
		W[i] = rotl32(W[i - 3] ^ W[i - 8] ^ W[i - 14] ^ W[i - 16], 1);

	uint32 a = state[0], b = state[1], c = state[2];
	uint32 d = state[3], e = state[4];

	for (uint32 i = 0; i < 80; i++) {
		uint32 f, k;
		if (i < 20)      { f = (b & c) | ((~b) & d);          k = 0x5A827999; }
		else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1; }
		else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDC; }
		else             { f = b ^ c ^ d;                     k = 0xCA62C1D6; }
		uint32 t = rotl32(a, 5) + f + e + k + W[i];
		e = d; d = c; c = rotl32(b, 30); b = a; a = t;
	}
	state[0] += a; state[1] += b; state[2] += c;
	state[3] += d; state[4] += e;
}


static void
sha1_init(Sha1Ctx* ctx)
{
	ctx->state[0] = 0x67452301;
	ctx->state[1] = 0xEFCDAB89;
	ctx->state[2] = 0x98BADCFE;
	ctx->state[3] = 0x10325476;
	ctx->state[4] = 0xC3D2E1F0;
	ctx->bitCount = 0;
}


static void
sha1_update(Sha1Ctx* ctx, const uint8* data, uint32 len)
{
	uint32 i = (uint32)((ctx->bitCount >> 3) & 63);
	ctx->bitCount += (uint64)len << 3;
	if (i + len < 64) {
		memcpy(&ctx->buffer[i], data, len);
		return;
	}
	uint32 first = 64 - i;
	memcpy(&ctx->buffer[i], data, first);
	sha1_transform(ctx->state, ctx->buffer);
	uint32 offset = first;
	while (offset + 64 <= len) {
		sha1_transform(ctx->state, data + offset);
		offset += 64;
	}
	memcpy(ctx->buffer, data + offset, len - offset);
}


static void
sha1_final(Sha1Ctx* ctx, uint8 digest[20])
{
	uint64 bitLen = ctx->bitCount;
	uint32 i = (uint32)((ctx->bitCount >> 3) & 63);
	ctx->buffer[i++] = 0x80;
	if (i > 56) {
		while (i < 64)
			ctx->buffer[i++] = 0;
		sha1_transform(ctx->state, ctx->buffer);
		i = 0;
	}
	while (i < 56)
		ctx->buffer[i++] = 0;
	for (uint32 j = 0; j < 8; j++)
		ctx->buffer[56 + j] = (uint8)(bitLen >> (56 - j * 8));
	sha1_transform(ctx->state, ctx->buffer);
	for (uint32 j = 0; j < 5; j++) {
		digest[j * 4]     = (uint8)(ctx->state[j] >> 24);
		digest[j * 4 + 1] = (uint8)(ctx->state[j] >> 16);
		digest[j * 4 + 2] = (uint8)(ctx->state[j] >> 8);
		digest[j * 4 + 3] = (uint8)ctx->state[j];
	}
}


void
sha1(const uint8* data, uint32 len, uint8 digest[20])
{
	Sha1Ctx ctx;
	sha1_init(&ctx);
	sha1_update(&ctx, data, len);
	sha1_final(&ctx, digest);
}


void
hmac_sha1(const uint8* key, uint32 keyLen,
	const uint8* msg, uint32 msgLen, uint8 mac[20])
{
	uint8 keyBuf[64];
	if (keyLen > 64) {
		// Hash long keys down to 20 bytes per RFC 2104 §2.
		sha1(key, keyLen, keyBuf);
		memset(keyBuf + 20, 0, 44);
	} else {
		memcpy(keyBuf, key, keyLen);
		memset(keyBuf + keyLen, 0, 64 - keyLen);
	}
	uint8 ipad[64];
	uint8 opad[64];
	for (uint32 i = 0; i < 64; i++) {
		ipad[i] = keyBuf[i] ^ 0x36;
		opad[i] = keyBuf[i] ^ 0x5C;
	}
	Sha1Ctx ctx;
	uint8 inner[20];
	sha1_init(&ctx);
	sha1_update(&ctx, ipad, 64);
	sha1_update(&ctx, msg, msgLen);
	sha1_final(&ctx, inner);
	sha1_init(&ctx);
	sha1_update(&ctx, opad, 64);
	sha1_update(&ctx, inner, 20);
	sha1_final(&ctx, mac);
}


// ---------------------------------------------------------------------------
// PRF-384 (IEEE 802.11i §8.5.1.1)
// ---------------------------------------------------------------------------

void
prf_384(const uint8* key, uint32 keyLen,
	const char* label, uint32 labelLen,
	const uint8* data, uint32 dataLen,
	uint8 output[48])
{
	// Working buffer: label || 0x00 || data || counter.
	uint8 buf[256];
	uint32 bufLen = labelLen + 1 + dataLen + 1;
	if (bufLen > sizeof(buf)) {
		memset(output, 0, 48);
		return;
	}
	memcpy(buf, label, labelLen);
	buf[labelLen] = 0;
	memcpy(buf + labelLen + 1, data, dataLen);

	// PRF iterates HMAC-SHA1 with a single-byte counter at the end.
	// 384/160 = 2.4 -> 3 iterations, last truncated to 8 bytes.
	uint8 hash[20];
	for (uint8 i = 0; i < 3; i++) {
		buf[bufLen - 1] = i;
		hmac_sha1(key, keyLen, buf, bufLen, hash);
		uint32 chunk = (i < 2) ? 20 : 8;
		memcpy(output + i * 20, hash, chunk);
	}
}


// ---------------------------------------------------------------------------
// PBKDF2-HMAC-SHA1 (RFC 2898 §5.2)
// ---------------------------------------------------------------------------

void
pbkdf2_hmac_sha1(const uint8* password, uint32 passwordLen,
	const uint8* salt, uint32 saltLen, uint32 iterations,
	uint8* output, uint32 outputLen)
{
	// Each block produces 20 bytes (SHA-1 length).
	uint32 blockCount = (outputLen + 19) / 20;
	uint8 saltAndIndex[256];
	if (saltLen + 4 > sizeof(saltAndIndex)) {
		memset(output, 0, outputLen);
		return;
	}
	memcpy(saltAndIndex, salt, saltLen);

	for (uint32 block = 1; block <= blockCount; block++) {
		// Append big-endian block index.
		saltAndIndex[saltLen]     = (uint8)(block >> 24);
		saltAndIndex[saltLen + 1] = (uint8)(block >> 16);
		saltAndIndex[saltLen + 2] = (uint8)(block >> 8);
		saltAndIndex[saltLen + 3] = (uint8)block;

		uint8 U[20];
		uint8 T[20];
		hmac_sha1(password, passwordLen, saltAndIndex, saltLen + 4, U);
		memcpy(T, U, 20);

		for (uint32 iter = 1; iter < iterations; iter++) {
			hmac_sha1(password, passwordLen, U, 20, U);
			for (uint32 i = 0; i < 20; i++)
				T[i] ^= U[i];
		}

		uint32 copyLen = (block * 20 <= outputLen) ? 20 : (outputLen - (block - 1) * 20);
		memcpy(output + (block - 1) * 20, T, copyLen);
	}
}


// ---------------------------------------------------------------------------
// AES-128 (FIPS 197).  Compact reference using S-box + inverse S-box.
// ---------------------------------------------------------------------------

static const uint8 sBox[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
	0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
	0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
	0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
	0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
	0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
	0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
	0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
	0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};


static const uint8 invSBox[256] = {
	0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
	0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
	0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
	0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
	0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
	0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
	0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
	0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
	0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
	0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
	0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
	0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
	0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
	0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
	0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
	0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d
};


static const uint8 rcon[11] = {
	0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};


// AES-128 has Nk=4, Nb=4, Nr=10.  Round-key schedule: 11 round keys
// of 16 bytes each = 176 bytes.
static void
aes128_key_expansion(const uint8 key[16], uint8 schedule[176])
{
	memcpy(schedule, key, 16);
	uint32 generated = 16;
	uint32 rconIdx = 1;
	uint8 t[4];
	while (generated < 176) {
		// Take last 4 bytes
		for (uint32 i = 0; i < 4; i++)
			t[i] = schedule[generated - 4 + i];
		if (generated % 16 == 0) {
			// RotWord + SubWord + Rcon
			uint8 tmp = t[0];
			t[0] = sBox[t[1]] ^ rcon[rconIdx++];
			t[1] = sBox[t[2]];
			t[2] = sBox[t[3]];
			t[3] = sBox[tmp];
		}
		for (uint32 i = 0; i < 4; i++) {
			schedule[generated] = schedule[generated - 16] ^ t[i];
			generated++;
		}
	}
}


// GF(2^8) multiplication used in MixColumns.
static inline uint8
xtime(uint8 b)
{
	return (uint8)((b << 1) ^ ((b & 0x80) ? 0x1b : 0));
}


static inline uint8
gmul(uint8 a, uint8 b)
{
	uint8 result = 0;
	for (uint32 i = 0; i < 8; i++) {
		if (b & 1)
			result ^= a;
		uint8 hi = a & 0x80;
		a <<= 1;
		if (hi)
			a ^= 0x1b;
		b >>= 1;
	}
	return result;
}


// Operate on a 4x4 column-major state.
static void
add_round_key(uint8 state[16], const uint8* roundKey)
{
	for (uint32 i = 0; i < 16; i++)
		state[i] ^= roundKey[i];
}


static void
sub_bytes(uint8 state[16])
{
	for (uint32 i = 0; i < 16; i++)
		state[i] = sBox[state[i]];
}


static void
inv_sub_bytes(uint8 state[16])
{
	for (uint32 i = 0; i < 16; i++)
		state[i] = invSBox[state[i]];
}


static void
shift_rows(uint8 s[16])
{
	uint8 t = s[1];   s[1] = s[5];   s[5] = s[9];   s[9] = s[13];  s[13] = t;
	t = s[2];   uint8 u = s[6];   s[2] = s[10];  s[6] = s[14]; s[10] = t;     s[14] = u;
	t = s[15];  s[15] = s[11];  s[11] = s[7];   s[7] = s[3];   s[3] = t;
}


static void
inv_shift_rows(uint8 s[16])
{
	uint8 t = s[13];  s[13] = s[9];  s[9] = s[5];  s[5] = s[1];  s[1] = t;
	t = s[2];   uint8 u = s[6];   s[2] = s[10];  s[6] = s[14]; s[10] = t;     s[14] = u;
	t = s[3];   s[3] = s[7];   s[7] = s[11];  s[11] = s[15]; s[15] = t;
}


static void
mix_columns(uint8 s[16])
{
	for (uint32 c = 0; c < 4; c++) {
		uint8 a0 = s[c * 4], a1 = s[c * 4 + 1];
		uint8 a2 = s[c * 4 + 2], a3 = s[c * 4 + 3];
		uint8 b0 = xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3;
		uint8 b1 = a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3;
		uint8 b2 = a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3);
		uint8 b3 = (xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3);
		s[c * 4] = b0; s[c * 4 + 1] = b1;
		s[c * 4 + 2] = b2; s[c * 4 + 3] = b3;
	}
}


static void
inv_mix_columns(uint8 s[16])
{
	for (uint32 c = 0; c < 4; c++) {
		uint8 a0 = s[c * 4], a1 = s[c * 4 + 1];
		uint8 a2 = s[c * 4 + 2], a3 = s[c * 4 + 3];
		uint8 b0 = gmul(a0, 0x0e) ^ gmul(a1, 0x0b) ^ gmul(a2, 0x0d) ^ gmul(a3, 0x09);
		uint8 b1 = gmul(a0, 0x09) ^ gmul(a1, 0x0e) ^ gmul(a2, 0x0b) ^ gmul(a3, 0x0d);
		uint8 b2 = gmul(a0, 0x0d) ^ gmul(a1, 0x09) ^ gmul(a2, 0x0e) ^ gmul(a3, 0x0b);
		uint8 b3 = gmul(a0, 0x0b) ^ gmul(a1, 0x0d) ^ gmul(a2, 0x09) ^ gmul(a3, 0x0e);
		s[c * 4] = b0; s[c * 4 + 1] = b1;
		s[c * 4 + 2] = b2; s[c * 4 + 3] = b3;
	}
}


void
aes128_encrypt(const uint8 key[16], const uint8 plaintext[16],
	uint8 ciphertext[16])
{
	uint8 schedule[176];
	aes128_key_expansion(key, schedule);

	uint8 state[16];
	memcpy(state, plaintext, 16);

	add_round_key(state, schedule);
	for (uint32 round = 1; round < 10; round++) {
		sub_bytes(state);
		shift_rows(state);
		mix_columns(state);
		add_round_key(state, schedule + round * 16);
	}
	sub_bytes(state);
	shift_rows(state);
	add_round_key(state, schedule + 160);

	memcpy(ciphertext, state, 16);
}


void
aes128_decrypt(const uint8 key[16], const uint8 ciphertext[16],
	uint8 plaintext[16])
{
	uint8 schedule[176];
	aes128_key_expansion(key, schedule);

	uint8 state[16];
	memcpy(state, ciphertext, 16);

	add_round_key(state, schedule + 160);
	for (uint32 round = 9; round >= 1; round--) {
		inv_shift_rows(state);
		inv_sub_bytes(state);
		add_round_key(state, schedule + round * 16);
		inv_mix_columns(state);
	}
	inv_shift_rows(state);
	inv_sub_bytes(state);
	add_round_key(state, schedule);

	memcpy(plaintext, state, 16);
}


// ---------------------------------------------------------------------------
// RFC 3394 AES Key Wrap — unwrap only.
// ---------------------------------------------------------------------------

bool
aes_unwrap(const uint8 kek[16], const uint8* cipher, uint32 cipherLen,
	uint8* plaintext)
{
	if (cipherLen < 16 || (cipherLen % 8) != 0)
		return false;
	uint32 n = (cipherLen / 8) - 1;	// number of plaintext blocks (8 bytes each)
	if (n < 1)
		return false;

	uint8 A[8];
	memcpy(A, cipher, 8);
	memcpy(plaintext, cipher + 8, n * 8);

	uint8 block[16];
	for (int j = 5; j >= 0; j--) {
		for (int i = (int)n; i >= 1; i--) {
			uint64 t = (uint64)n * j + (uint64)i;
			uint8 tBytes[8];
			for (uint32 k = 0; k < 8; k++)
				tBytes[k] = (uint8)(t >> (56 - k * 8));

			// A ^= t; B = AES-DEC(KEK, A | R[i])
			uint8 in[16];
			for (uint32 k = 0; k < 8; k++)
				in[k] = A[k] ^ tBytes[k];
			memcpy(in + 8, plaintext + (i - 1) * 8, 8);

			aes128_decrypt(kek, in, block);

			memcpy(A, block, 8);
			memcpy(plaintext + (i - 1) * 8, block + 8, 8);
		}
	}

	// Integrity check: A must equal IV = 0xA6A6A6A6A6A6A6A6.
	for (uint32 i = 0; i < 8; i++) {
		if (A[i] != 0xA6)
			return false;
	}
	return true;
}


// AES-CCMP decrypt per IEEE 802.11-2012 §11.4.4.
//
// CCM parameters for 802.11 CCMP: M=8 (8-byte MIC), L=2 (2-byte length
// counter).  Nonce is 13 bytes.  Adata flag is set since AAD is non-empty.
//
// The CCM CBC-MAC initial block B_0 has:
//   B_0[0]    = 0x40 | ((M-2)/2 << 3) | (L-1)
//             = 0x40 | (3 << 3) | 1 = 0x59
//   B_0[1..13]= 13-byte CCM nonce
//   B_0[14..15]= big-endian length of plaintext data
//
// The CCM CTR initial block A_0 has:
//   A_0[0]    = (L-1) = 0x01
//   A_0[1..13]= same 13-byte CCM nonce
//   A_0[14..15]= big-endian counter (starts at 0; A_0 unused except to
//             encrypt MIC tag, A_1..A_n encrypt plaintext blocks)
//
// CCMP-specific 13-byte nonce assembly (§11.4.4.2):
//   nonce[0]  = priority byte: low 4 bits = TID for QoS data, 0 for non-QoS;
//               bit 4 = 1 if management frame (0 for data)
//   nonce[1..6] = A2 (transmitter address; for FromDS=1 frames this is
//                 the AP's BSSID)
//   nonce[7..12]= packet number (PN), big-endian (PN5 PN4 PN3 PN2 PN1 PN0).
//                 PN comes from the CCMP IV header bytes [0,1,4,5,6,7].
//
// AAD (Additional Authentication Data) for non-QoS, non-Addr4 frames is
// 22 bytes laid out as:
//   FC0' | FC1' | A1 | A2 | A3 | SC'
// where:
//   FC0' = FC0 & 0x8F  (clear subtype bits 4..6)
//   FC1' = FC1 & 0xC7  (clear Retry/PwrMgmt/MoreData bits 3..5)
//   SC'  = SC & 0x000F (clear sequence number bits, keep frag bits)
// Padded to 32 bytes (two AES blocks) for CBC-MAC.  CBC-MAC processes
// B_0, then [Adata-length-prefix (2) || AAD || zero-pad to block], then
// plaintext padded to block.  Final block's first 8 bytes = MIC tag.
bool
ccmp_decrypt(const uint8 tk[16], uint8* frame, uint32 frameLen, uint32 hdrLen)
{
	if (hdrLen != 24 && hdrLen != 26)
		return false;	// Addr4 / unsupported header layouts
	if (frameLen < hdrLen + 8 + 8)	// hdr + IV + MIC at minimum
		return false;

	const uint8* iv = frame + hdrLen;
	uint8* body = frame + hdrLen + 8;
	uint32 bodyLen = frameLen - hdrLen - 8 - 8;	// strip IV + MIC
	const uint8* receivedMic = frame + frameLen - 8;

	// PN reassembly: IV bytes [0,1,4,5,6,7] = PN0,PN1,PN2,PN3,PN4,PN5
	// (low to high).  We store as 13-byte nonce big-endian: PN5 first.
	uint8 nonce[13];
	uint8 priorityByte = 0;	// non-QoS data: 0
	if (hdrLen == 26) {
		// QoS data: TID is low 4 bits of QoS Control field (offset 24
		// in the 802.11 header).  Standard says priorityByte's bit 4
		// (Management) should be 0 for data; bits 0..3 = TID.
		priorityByte = frame[24] & 0x0F;
	}
	nonce[0] = priorityByte;
	memcpy(&nonce[1], frame + 10, 6);	// A2
	nonce[7] = iv[7];	// PN5
	nonce[8] = iv[6];	// PN4
	nonce[9] = iv[5];	// PN3
	nonce[10] = iv[4];	// PN2
	nonce[11] = iv[1];	// PN1
	nonce[12] = iv[0];	// PN0

	// Build AAD (we only support non-Addr4 frames; total = 22 bytes
	// for non-QoS, 24 bytes for QoS).
	uint8 aad[32];
	memset(aad, 0, sizeof(aad));
	uint32 aadLen;
	aad[0] = (uint8)(frame[0] & 0x8Fu);
	aad[1] = (uint8)(frame[1] & 0xC7u);
	memcpy(&aad[2], frame + 4, 6);		// A1
	memcpy(&aad[8], frame + 10, 6);		// A2
	memcpy(&aad[14], frame + 16, 6);	// A3
	aad[20] = (uint8)(frame[22] & 0x0Fu);	// SC frag low nibble
	aad[21] = 0;
	if (hdrLen == 26) {
		aad[22] = (uint8)(frame[24] & 0x0Fu);	// QoS TID
		aad[23] = 0;
		aadLen = 24;
	} else {
		aadLen = 22;
	}

	// CBC-MAC over B_0 || AAD-length-prefix(2) || AAD || pad || body || pad.
	// Result's first 8 bytes XOR S_0 should equal received MIC.
	uint8 b0[16];
	b0[0] = 0x59;	// flags: Adata=1, M=8 (3<<3), L=2 (L-1=1)
	memcpy(&b0[1], nonce, 13);
	b0[14] = (uint8)(bodyLen >> 8);
	b0[15] = (uint8)bodyLen;

	uint8 macState[16];
	aes128_encrypt(tk, b0, macState);

	// Process AAD: 2-byte length prefix + AAD bytes, padded to block size.
	uint8 macBlock[16];
	memset(macBlock, 0, sizeof(macBlock));
	macBlock[0] = (uint8)(aadLen >> 8);
	macBlock[1] = (uint8)aadLen;
	uint32 firstChunk = (aadLen <= 14) ? aadLen : 14;
	memcpy(&macBlock[2], aad, firstChunk);
	for (uint32 i = 0; i < 16; i++)
		macBlock[i] ^= macState[i];
	aes128_encrypt(tk, macBlock, macState);

	if (aadLen > 14) {
		memset(macBlock, 0, sizeof(macBlock));
		memcpy(macBlock, aad + 14, aadLen - 14);
		for (uint32 i = 0; i < 16; i++)
			macBlock[i] ^= macState[i];
		aes128_encrypt(tk, macBlock, macState);
	}

	// CTR-decrypt body in place AND fold plaintext blocks into CBC-MAC.
	// CTR keystream block A_i = AES(K, [0x01 || nonce(13) || counter_BE(2)]).
	// Counter starts at 1 for body (0 reserved for MIC tag mask).
	uint8 ctrBlock[16];
	ctrBlock[0] = 0x01;
	memcpy(&ctrBlock[1], nonce, 13);

	uint32 offset = 0;
	uint32 counter = 1;
	while (offset < bodyLen) {
		ctrBlock[14] = (uint8)(counter >> 8);
		ctrBlock[15] = (uint8)counter;
		uint8 keystream[16];
		aes128_encrypt(tk, ctrBlock, keystream);

		uint32 chunk = bodyLen - offset;
		if (chunk > 16)
			chunk = 16;

		// XOR ciphertext with keystream to produce plaintext in place.
		for (uint32 i = 0; i < chunk; i++)
			body[offset + i] ^= keystream[i];

		// Fold plaintext (now in `body[offset..]`) into CBC-MAC.  Pad
		// the last short block with zeros.
		memset(macBlock, 0, sizeof(macBlock));
		memcpy(macBlock, body + offset, chunk);
		for (uint32 i = 0; i < 16; i++)
			macBlock[i] ^= macState[i];
		aes128_encrypt(tk, macBlock, macState);

		offset += chunk;
		counter++;
	}

	// MIC tag = first 8 bytes of (macState XOR S_0), where S_0 = AES(K, A_0).
	uint8 a0[16];
	a0[0] = 0x01;
	memcpy(&a0[1], nonce, 13);
	a0[14] = 0;
	a0[15] = 0;
	uint8 s0[16];
	aes128_encrypt(tk, a0, s0);

	uint8 computedMic[8];
	for (uint32 i = 0; i < 8; i++)
		computedMic[i] = macState[i] ^ s0[i];

	if (memcmp(computedMic, receivedMic, 8) != 0)
		return false;

	return true;
}


// AES-CCMP encrypt per IEEE 802.11-2012 §11.4.4.4.  Mirror of
// ccmp_decrypt: build the same CCM B_0 / nonce / AAD, run AES-CBC-MAC
// over (B_0 || AAD-len-prefix || AAD || pad || plaintext || pad) to
// produce a 16-byte tag; first 8 bytes XOR S_0 = encrypted MIC tag.
// Then AES-CTR (counter starts at 1) encrypts the body in place.
// Finally rewrites the buffer with the CCMP IV header at [hdrLen..+7]
// and sets the Protected bit in FC[1].
uint32
ccmp_encrypt(const uint8 tk[16], uint8* frame, uint32 frameLen, uint32 hdrLen,
	uint64 pn, uint8 keyId)
{
	if (hdrLen != 24 && hdrLen != 26)
		return 0;	// Addr4 / unsupported

	uint32 bodyLen = frameLen - hdrLen;

	// Make room for the 8-byte CCMP IV by shifting the body 8 bytes
	// later in the buffer.  Caller guarantees frameLen+16 capacity.
	memmove(frame + hdrLen + 8, frame + hdrLen, bodyLen);
	uint8* body = frame + hdrLen + 8;
	uint8* iv = frame + hdrLen;

	// Set the Protected Frame bit in FC[1] *before* building AAD.
	// IEEE 802.11i specifies the AAD computation uses the modified
	// MAC header where Protected Frame is set to 1 (the frame is
	// protected, after all).  Both sender and receiver compute the
	// MIC over this byte; if we leave it 0 here, our MIC won't match
	// what the AP computes when it receives the frame and the AP
	// silently drops the frame.
	frame[1] |= 0x40;

	// Build the CCMP IV header.
	//   IV[0]=PN0, IV[1]=PN1, IV[2]=0 (reserved), IV[3]=KeyID byte,
	//   IV[4]=PN2, IV[5]=PN3, IV[6]=PN4, IV[7]=PN5.
	// KeyID byte: bit 5 = ExtIV (always 1 for CCMP), bits 6..7 = keyId.
	iv[0] = (uint8)(pn & 0xFF);				// PN0
	iv[1] = (uint8)((pn >> 8) & 0xFF);		// PN1
	iv[2] = 0;
	iv[3] = (uint8)(0x20 | ((keyId & 0x03) << 6));
	iv[4] = (uint8)((pn >> 16) & 0xFF);		// PN2
	iv[5] = (uint8)((pn >> 24) & 0xFF);		// PN3
	iv[6] = (uint8)((pn >> 32) & 0xFF);		// PN4
	iv[7] = (uint8)((pn >> 40) & 0xFF);		// PN5

	// Nonce (13 bytes): same construction as in decrypt — priority
	// byte (0 for non-QoS data), then A2 (transmitter address; for
	// our STA-side TX with ToDS=1 this is our own MAC at offset 10
	// of the 802.11 header), then PN big-endian (PN5 first).
	uint8 nonce[13];
	uint8 priorityByte = 0;
	if (hdrLen == 26)
		priorityByte = frame[24] & 0x0F;
	nonce[0] = priorityByte;
	memcpy(&nonce[1], frame + 10, 6);
	nonce[7] = (uint8)((pn >> 40) & 0xFF);	// PN5
	nonce[8] = (uint8)((pn >> 32) & 0xFF);	// PN4
	nonce[9] = (uint8)((pn >> 24) & 0xFF);	// PN3
	nonce[10] = (uint8)((pn >> 16) & 0xFF);	// PN2
	nonce[11] = (uint8)((pn >> 8) & 0xFF);	// PN1
	nonce[12] = (uint8)(pn & 0xFF);			// PN0

	// AAD (same as decrypt path).
	uint8 aad[32];
	memset(aad, 0, sizeof(aad));
	uint32 aadLen;
	aad[0] = (uint8)(frame[0] & 0x8Fu);
	aad[1] = (uint8)(frame[1] & 0xC7u);
	memcpy(&aad[2], frame + 4, 6);
	memcpy(&aad[8], frame + 10, 6);
	memcpy(&aad[14], frame + 16, 6);
	aad[20] = (uint8)(frame[22] & 0x0Fu);
	aad[21] = 0;
	if (hdrLen == 26) {
		aad[22] = (uint8)(frame[24] & 0x0Fu);
		aad[23] = 0;
		aadLen = 24;
	} else {
		aadLen = 22;
	}

	// CBC-MAC chain: B_0 || AAD-len-prefix(2) || AAD || pad || body || pad.
	uint8 b0[16];
	b0[0] = 0x59;
	memcpy(&b0[1], nonce, 13);
	b0[14] = (uint8)(bodyLen >> 8);
	b0[15] = (uint8)bodyLen;

	uint8 macState[16];
	aes128_encrypt(tk, b0, macState);

	uint8 macBlock[16];
	memset(macBlock, 0, sizeof(macBlock));
	macBlock[0] = (uint8)(aadLen >> 8);
	macBlock[1] = (uint8)aadLen;
	uint32 firstChunk = (aadLen <= 14) ? aadLen : 14;
	memcpy(&macBlock[2], aad, firstChunk);
	for (uint32 i = 0; i < 16; i++)
		macBlock[i] ^= macState[i];
	aes128_encrypt(tk, macBlock, macState);
	if (aadLen > 14) {
		memset(macBlock, 0, sizeof(macBlock));
		memcpy(macBlock, aad + 14, aadLen - 14);
		for (uint32 i = 0; i < 16; i++)
			macBlock[i] ^= macState[i];
		aes128_encrypt(tk, macBlock, macState);
	}

	// Fold plaintext into CBC-MAC, then CTR-encrypt the body.  The
	// loop processes one 16-byte block per iteration: first XOR the
	// plaintext block into macState and run AES on it, then run AES
	// on the CTR block to produce keystream and XOR with the body to
	// encrypt in place.  Counter starts at 1; counter 0 is reserved
	// for masking the MIC tag.
	uint8 ctrBlock[16];
	ctrBlock[0] = 0x01;
	memcpy(&ctrBlock[1], nonce, 13);

	uint32 offset = 0;
	uint32 counter = 1;
	while (offset < bodyLen) {
		uint32 chunk = bodyLen - offset;
		if (chunk > 16)
			chunk = 16;

		// Fold plaintext into CBC-MAC (must happen *before* CTR
		// overwrites the body).
		memset(macBlock, 0, sizeof(macBlock));
		memcpy(macBlock, body + offset, chunk);
		for (uint32 i = 0; i < 16; i++)
			macBlock[i] ^= macState[i];
		aes128_encrypt(tk, macBlock, macState);

		// CTR-encrypt this block of body in place.
		ctrBlock[14] = (uint8)(counter >> 8);
		ctrBlock[15] = (uint8)counter;
		uint8 keystream[16];
		aes128_encrypt(tk, ctrBlock, keystream);
		for (uint32 i = 0; i < chunk; i++)
			body[offset + i] ^= keystream[i];

		offset += chunk;
		counter++;
	}

	// Encrypted MIC tag = (CBC-MAC[0..7] XOR S_0[0..7]) where
	// S_0 = AES(K, A_0) with counter=0.
	uint8 a0[16];
	a0[0] = 0x01;
	memcpy(&a0[1], nonce, 13);
	a0[14] = 0;
	a0[15] = 0;
	uint8 s0[16];
	aes128_encrypt(tk, a0, s0);

	uint8* mic = body + bodyLen;
	for (uint32 i = 0; i < 8; i++)
		mic[i] = macState[i] ^ s0[i];

	// Protected bit was already set up-front so it would be folded
	// into the AAD; nothing more to do here.

	return frameLen + 16;
}


}	// namespace wpa2_crypto
