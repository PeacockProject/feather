/*
 * sha256.h — minimal SHA-256 implementation interface.
 *
 * Vendored from Brad Conte's public-domain crypto-algorithms collection;
 * see src/vendor/README.md for upstream pointer + license.
 *
 * Surface:
 *
 *   sha256_init(&ctx)
 *   sha256_update(&ctx, data, len)
 *   sha256_final(&ctx, hash)   // hash[] is 32 bytes
 *
 * Plus a convenience wrapper for streaming a file:
 *
 *   sha256_file_hex(path, hex_out)   // hex_out is 65 bytes (64+NUL)
 */

#ifndef FTR_VENDOR_SHA256_H
#define FTR_VENDOR_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_BLOCK_SIZE 32 /* bytes in a SHA-256 digest */

typedef struct {
	uint8_t  data[64];
	uint32_t datalen;
	uint64_t bitlen;
	uint32_t state[8];
} SHA256_CTX;

void sha256_init(SHA256_CTX *ctx);
void sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len);
void sha256_final(SHA256_CTX *ctx, uint8_t hash[SHA256_BLOCK_SIZE]);

/* Compute SHA-256 of `path` and write 64 lowercase-hex chars + NUL to
 * `hex_out`. Returns 0 on success, -1 on I/O error. */
int sha256_file_hex(const char *path, char hex_out[65]);

#endif /* FTR_VENDOR_SHA256_H */
