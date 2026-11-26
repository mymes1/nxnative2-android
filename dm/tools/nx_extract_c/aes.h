// AES-128 block cipher for the on-device NSP/XCI extractor (nx_extract_c).
// A compact, self-contained implementation of the AES-128 key schedule and
// single-block encryption/decryption. Used for:
//   - AES-ECB (NCA key-area and title-key decryption)
//   - AES-CTR (NCA section decryption, via aes_ctr below)
//   - AES-XTS (NCA header decryption, via aes_xts below)
// No external dependencies: the NDK has no AES, and the host must not link
// OpenSSL. Tested against FIPS-197 known-answer vectors.
#pragma once
#include <stdint.h>
#include <stddef.h>

// One AES-128 key schedule (expanded for 11 rounds).
typedef struct {
    uint32_t rk[44];   // 4 words per round × 11 rounds (round 0 is the key itself)
} aes128_ctx;

void aes128_init(aes128_ctx *c, const uint8_t key[16]);

// Encrypt/decrypt one 16-byte block in place.
void aes128_encrypt_block(const aes128_ctx *c, uint8_t block[16]);
void aes128_decrypt_block(const aes128_ctx *c, uint8_t block[16]);

// AES-128-CTR over `len` bytes at stream offset `offset` (in bytes).
// `key` is the 16-byte AES key; `upper` is the high 8 bytes of every counter
// block (the low 8 bytes are the big-endian block index, starting at
// `offset / 16`). An unaligned `offset` skips the first `offset % 16`
// keystream bytes. In-place: `buf` is XORed with the keystream.
void aes128_ctr(const uint8_t key[16], const uint8_t upper[8],
                uint64_t offset, uint8_t *buf, size_t len);

// AES-256-XTS decryption (two AES-128 keys). `key` is the 32-byte XTS key
// (data half first, tweak half second). Decrypts `len` bytes in place, in
// sectors of `sector_size` bytes, starting at sector number `first_sector`.
// The tweak is the big-endian sector number, multiplied by x in GF(2^128)
// per 16-byte block. A trailing partial sector is left as is.
void aes256_xts_decrypt(const uint8_t key[32], uint8_t *buf, size_t len,
                        size_t sector_size, uint64_t first_sector);
