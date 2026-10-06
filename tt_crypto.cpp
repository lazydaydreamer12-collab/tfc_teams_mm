// tt_crypto.cpp - SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104), hex, and OS
// random bytes. Small and dependency-free so both builds share it.

#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
// SystemFunction036 is RtlGenRandom, exported by advapi32 on every Windows.
extern "C" BOOLEAN NTAPI SystemFunction036(PVOID RandomBuffer, ULONG RandomBufferLength);
#pragma comment(lib, "advapi32.lib")
#endif

#include "tt_crypto.h"

static const unsigned int K256[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void Block(TTSha256 *c, const unsigned char *p)
{
	unsigned int w[64];
	for (int i = 0; i < 16; i++)
		w[i] = ((unsigned int)p[4 * i] << 24) | ((unsigned int)p[4 * i + 1] << 16)
		     | ((unsigned int)p[4 * i + 2] << 8) | (unsigned int)p[4 * i + 3];
	for (int i = 16; i < 64; i++)
	{
		unsigned int s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		unsigned int s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	unsigned int a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
	unsigned int e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
	for (int i = 0; i < 64; i++)
	{
		unsigned int S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
		unsigned int ch = (e & f) ^ (~e & g);
		unsigned int t1 = h + S1 + ch + K256[i] + w[i];
		unsigned int S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
		unsigned int mj = (a & b) ^ (a & cc) ^ (b & cc);
		unsigned int t2 = S0 + mj;
		h = g; g = f; f = e; e = d + t1;
		d = cc; cc = b; b = a; a = t1 + t2;
	}
	c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
	c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void TT_Sha256Init(TTSha256 *c)
{
	static const unsigned int iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	memcpy(c->h, iv, sizeof(iv));
	c->bufLen = 0;
	c->total = 0;
}

void TT_Sha256Update(TTSha256 *c, const void *data, size_t len)
{
	const unsigned char *p = (const unsigned char *)data;
	c->total += len;
	while (len > 0)
	{
		size_t take = 64 - c->bufLen;
		if (take > len)
			take = len;
		memcpy(c->buf + c->bufLen, p, take);
		c->bufLen += (unsigned int)take;
		p += take;
		len -= take;
		if (c->bufLen == 64)
		{
			Block(c, c->buf);
			c->bufLen = 0;
		}
	}
}

void TT_Sha256Final(TTSha256 *c, unsigned char out[32])
{
	unsigned long long bits = c->total * 8ULL;
	unsigned char pad = 0x80;
	TT_Sha256Update(c, &pad, 1);
	unsigned char zero = 0;
	while (c->bufLen != 56)
		TT_Sha256Update(c, &zero, 1);
	unsigned char len[8];
	for (int i = 0; i < 8; i++)
		len[i] = (unsigned char)(bits >> (56 - 8 * i));
	TT_Sha256Update(c, len, 8);
	for (int i = 0; i < 8; i++)
	{
		out[4 * i] = (unsigned char)(c->h[i] >> 24);
		out[4 * i + 1] = (unsigned char)(c->h[i] >> 16);
		out[4 * i + 2] = (unsigned char)(c->h[i] >> 8);
		out[4 * i + 3] = (unsigned char)c->h[i];
	}
	memset(c, 0, sizeof(*c));
}

void TT_Sha256(const void *data, size_t len, unsigned char out[32])
{
	TTSha256 c;
	TT_Sha256Init(&c);
	TT_Sha256Update(&c, data, len);
	TT_Sha256Final(&c, out);
}

void TT_HmacSha256(const void *key, size_t keyLen, const void *msg, size_t msgLen, unsigned char out[32])
{
	unsigned char k[64], ipad[64], opad[64], inner[32];
	memset(k, 0, sizeof(k));
	if (keyLen > 64)
		TT_Sha256(key, keyLen, k);
	else if (keyLen)
		memcpy(k, key, keyLen);
	for (int i = 0; i < 64; i++)
	{
		ipad[i] = (unsigned char)(k[i] ^ 0x36);
		opad[i] = (unsigned char)(k[i] ^ 0x5c);
	}
	TTSha256 c;
	TT_Sha256Init(&c);
	TT_Sha256Update(&c, ipad, 64);
	TT_Sha256Update(&c, msg, msgLen);
	TT_Sha256Final(&c, inner);
	TT_Sha256Init(&c);
	TT_Sha256Update(&c, opad, 64);
	TT_Sha256Update(&c, inner, 32);
	TT_Sha256Final(&c, out);
	memset(k, 0, sizeof(k));
	memset(ipad, 0, sizeof(ipad));
	memset(opad, 0, sizeof(opad));
}

void TT_Hex(const unsigned char *in, size_t len, char *out)
{
	static const char d[] = "0123456789abcdef";
	for (size_t i = 0; i < len; i++)
	{
		out[2 * i] = d[in[i] >> 4];
		out[2 * i + 1] = d[in[i] & 15];
	}
	out[2 * len] = 0;
}

static int HexVal(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

int TT_Unhex(const char *in, unsigned char *out, size_t outMax)
{
	size_t n = strlen(in);
	if (n % 2 || n / 2 > outMax)
		return -1;
	for (size_t i = 0; i < n / 2; i++)
	{
		int hi = HexVal(in[2 * i]), lo = HexVal(in[2 * i + 1]);
		if (hi < 0 || lo < 0)
			return -1;
		out[i] = (unsigned char)(hi * 16 + lo);
	}
	return (int)(n / 2);
}

bool TT_RandomBytes(unsigned char *out, size_t len)
{
#ifdef _WIN32
	return SystemFunction036(out, (ULONG)len) != FALSE;
#else
	FILE *f = fopen("/dev/urandom", "rb");
	if (!f)
		return false;
	size_t got = fread(out, 1, len, f);
	fclose(f);
	return got == len;
#endif
}

bool TT_SameBytes(const unsigned char *a, const unsigned char *b, size_t len)
{
	unsigned char d = 0;
	for (size_t i = 0; i < len; i++)
		d |= (unsigned char)(a[i] ^ b[i]);
	return d == 0;
}
