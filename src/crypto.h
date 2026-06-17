/* crypto.h — SHA-1, SHA-256, HMAC-SHA1 (vendored, public-domain style).
 * SHA-256 backs the HBP login hash; HMAC-SHA1 backs IPSC packet auth. */
#ifndef CRYPTO_H
#define CRYPTO_H

#include <stdint.h>
#include <stddef.h>

void sha1(const uint8_t *data, size_t len, uint8_t out[20]);
void sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/* HMAC-SHA1; full 20-byte digest written to out. */
void hmac_sha1(const uint8_t *key, size_t keylen,
               const uint8_t *msg, size_t msglen, uint8_t out[20]);

#endif
