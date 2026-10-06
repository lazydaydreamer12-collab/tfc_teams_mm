// tt_crypto.h - SHA-256 and HMAC-SHA256, used to encrypt the stored Discord
// webhook (tt_secret.cpp).
// Plain C++, the same on Windows and Linux.
#ifndef TFC_TEAMS_CRYPTO_H
#define TFC_TEAMS_CRYPTO_H

#include <stddef.h>

struct TTSha256
{
	unsigned int  h[8];
	unsigned char buf[64];
	unsigned int  bufLen;
	unsigned long long total;
};

void TT_Sha256Init(TTSha256 *c);
void TT_Sha256Update(TTSha256 *c, const void *data, size_t len);
void TT_Sha256Final(TTSha256 *c, unsigned char out[32]);
void TT_Sha256(const void *data, size_t len, unsigned char out[32]);

void TT_HmacSha256(const void *key, size_t keyLen, const void *msg, size_t msgLen, unsigned char out[32]);

// Lower-case hex. out must hold 2 * len + 1.
void TT_Hex(const unsigned char *in, size_t len, char *out);
// Returns bytes written, or -1 on a bad character / odd length / too long.
int  TT_Unhex(const char *in, unsigned char *out, size_t outMax);

// Random bytes from the operating system (Linux /dev/urandom, Windows
// RtlGenRandom). false if none could be had.
bool TT_RandomBytes(unsigned char *out, size_t len);

// Constant-time compare.
bool TT_SameBytes(const unsigned char *a, const unsigned char *b, size_t len);

#endif
