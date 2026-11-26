/*
 * sha256.h — Minimal SHA-256 hash for container hash-tree verification.
 */
#ifndef NX_SHA256_H
#define NX_SHA256_H

#include <stdint.h>
#include <stddef.h>

#define NX_SHA256_DIGEST_SIZE 32
#define NX_SHA256_BLOCK_SIZE  64

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  data[64];
    size_t   datalen;
} nx_sha256_ctx;

void nx_sha256_init(nx_sha256_ctx *ctx);
void nx_sha256_update(nx_sha256_ctx *ctx, const uint8_t *data, size_t len);
void nx_sha256_final(nx_sha256_ctx *ctx, uint8_t hash[32]);

/* One-shot convenience */
void nx_sha256_hash(const uint8_t *data, size_t len, uint8_t hash[32]);

#endif /* NX_SHA256_H */
