// AES-128 block cipher for the on-device NSP/XCI extractor.
// Compact, self-contained, no external dependencies. The tables are generated
// at compile time from the GF(2^8) definition so the code stays small and
// verifiable against FIPS-197.
#include "aes.h"

// ---- GF(2^8) multiply (x^8 + x^4 + x^3 + x + 1) ----
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) r ^= a;
        uint8_t hi = a & 0x80;
        a <<= 1;
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return r;
}

// S-box: multiplicative inverse in GF(2^8) followed by the affine transform.
// Built once at first use (the 256-entry table is small and the init is cheap
// relative to a single AES operation on a game file).
static uint8_t g_sbox[256], g_rsbox[256];
static int g_ready = 0;

static void build_tables(void) {
    if (g_ready) return;
    for (int i = 0; i < 256; i++) {
        // Multiplicative inverse: brute force (256 iterations, once).
        uint8_t inv = 0;
        if (i) for (int j = 1; j < 256; j++) { if (gmul((uint8_t)i, (uint8_t)j) == 1) { inv = (uint8_t)j; break; } }
        // Affine transform: b ^= rotl(b,1) ^ rotl(b,2) ^ rotl(b,3) ^ rotl(b,4) ^ 0x63
        uint8_t b = inv, r = b;
        for (int k = 1; k <= 4; k++) { r = (uint8_t)((r << 1) | (r >> 7)); b ^= r; }
        g_sbox[i] = (uint8_t)(b ^ 0x63);
    }
    for (int i = 0; i < 256; i++) g_rsbox[g_sbox[i]] = (uint8_t)i;
    g_ready = 1;
}

static uint8_t xtime(uint8_t a) { return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1b : 0)); }

void aes128_init(aes128_ctx *c, const uint8_t key[16]) {
    build_tables();
    // Load the key as 4 column-words (big-endian within each word).
    for (int i = 0; i < 4; i++)
        c->rk[i] = ((uint32_t)key[4 * i] << 24) | ((uint32_t)key[4 * i + 1] << 16)
                 | ((uint32_t)key[4 * i + 2] << 8) | (uint32_t)key[4 * i + 3];
    uint32_t rcon = 1;
    for (int i = 4; i < 44; i++) {
        uint32_t t = c->rk[i - 1];
        if (i % 4 == 0) {
            // RotWord + SubWord + Rcon
            t = ((t << 8) | (t >> 24));
            t = ((uint32_t)g_sbox[(t >> 24) & 0xff] << 24) | ((uint32_t)g_sbox[(t >> 16) & 0xff] << 16)
              | ((uint32_t)g_sbox[(t >> 8) & 0xff] << 8) | (uint32_t)g_sbox[t & 0xff];
            t ^= rcon << 24;
            rcon = xtime((uint8_t)rcon);
        }
        c->rk[i] = c->rk[i - 4] ^ t;
    }
}

// Encrypt one block: state is 4 rows × 4 columns, column-major.
// Standard structure: initial AddRoundKey, then 9 full rounds
// (SubBytes, ShiftRows, MixColumns, AddRoundKey), then the final round
// (SubBytes, ShiftRows, AddRoundKey — no MixColumns).
void aes128_encrypt_block(const aes128_ctx *c, uint8_t block[16]) {
    uint32_t s[4];
    for (int i = 0; i < 4; i++)
        s[i] = ((uint32_t)block[4 * i] << 24) | ((uint32_t)block[4 * i + 1] << 16)
             | ((uint32_t)block[4 * i + 2] << 8) | (uint32_t)block[4 * i + 3];
    // Initial AddRoundKey (round 0)
    for (int i = 0; i < 4; i++) s[i] ^= c->rk[i];
    for (int r = 1; r <= 9; r++) {
        // SubBytes + ShiftRows
        uint8_t t[16];
        for (int i = 0; i < 4; i++) {
            t[4 * i]     = g_sbox[(s[i] >> 24) & 0xff];
            t[4 * i + 1] = g_sbox[(s[(i + 1) % 4] >> 16) & 0xff];
            t[4 * i + 2] = g_sbox[(s[(i + 2) % 4] >> 8) & 0xff];
            t[4 * i + 3] = g_sbox[s[(i + 3) % 4] & 0xff];
        }
        // MixColumns + AddRoundKey
        for (int i = 0; i < 4; i++) {
            uint8_t a0 = t[4 * i], a1 = t[4 * i + 1], a2 = t[4 * i + 2], a3 = t[4 * i + 3];
            uint8_t b0 = xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3;
            uint8_t b1 = a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3;
            uint8_t b2 = a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3);
            uint8_t b3 = (xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3);
            s[i] = ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
            s[i] ^= c->rk[4 * r + i];
        }
    }
    // Final round: SubBytes + ShiftRows + AddRoundKey (no MixColumns)
    {
        uint8_t t[16];
        for (int i = 0; i < 4; i++) {
            t[4 * i]     = g_sbox[(s[i] >> 24) & 0xff];
            t[4 * i + 1] = g_sbox[(s[(i + 1) % 4] >> 16) & 0xff];
            t[4 * i + 2] = g_sbox[(s[(i + 2) % 4] >> 8) & 0xff];
            t[4 * i + 3] = g_sbox[s[(i + 3) % 4] & 0xff];
        }
        for (int i = 0; i < 4; i++) {
            uint32_t w = ((uint32_t)t[4 * i] << 24) | ((uint32_t)t[4 * i + 1] << 16)
                       | ((uint32_t)t[4 * i + 2] << 8) | (uint32_t)t[4 * i + 3];
            w ^= c->rk[40 + i];
            block[4 * i]     = (w >> 24) & 0xff;
            block[4 * i + 1] = (w >> 16) & 0xff;
            block[4 * i + 2] = (w >> 8) & 0xff;
            block[4 * i + 3] = w & 0xff;
        }
    }
}

void aes128_decrypt_block(const aes128_ctx *c, uint8_t block[16]) {
    uint32_t s[4];
    for (int i = 0; i < 4; i++)
        s[i] = ((uint32_t)block[4 * i] << 24) | ((uint32_t)block[4 * i + 1] << 16)
             | ((uint32_t)block[4 * i + 2] << 8) | (uint32_t)block[4 * i + 3];
    // Straightforward inverse cipher (FIPS-197 §5.3.4):
    // AddRoundKey(Nr), then for r = Nr-1..1: AddRoundKey(r), InvMixColumns,
    // InvShiftRows, InvSubBytes, then AddRoundKey(0).
    // Initial AddRoundKey (round 10)
    for (int i = 0; i < 4; i++) s[i] ^= c->rk[40 + i];
    // InvShiftRows + InvSubBytes (undo the final round of encryption)
    {
        uint8_t t[16];
        for (int i = 0; i < 4; i++) {
            t[4 * i]     = g_rsbox[(s[i] >> 24) & 0xff];
            t[4 * i + 1] = g_rsbox[(s[(i + 3) % 4] >> 16) & 0xff];
            t[4 * i + 2] = g_rsbox[(s[(i + 2) % 4] >> 8) & 0xff];
            t[4 * i + 3] = g_rsbox[s[(i + 1) % 4] & 0xff];
        }
        for (int i = 0; i < 4; i++)
            s[i] = ((uint32_t)t[4 * i] << 24) | ((uint32_t)t[4 * i + 1] << 16)
                 | ((uint32_t)t[4 * i + 2] << 8) | (uint32_t)t[4 * i + 3];
    }
    for (int r = 9; r >= 1; r--) {
        // AddRoundKey(r)
        for (int i = 0; i < 4; i++) s[i] ^= c->rk[4 * r + i];
        // InvMixColumns
        for (int i = 0; i < 4; i++) {
            uint8_t a0 = (s[i] >> 24) & 0xff, a1 = (s[i] >> 16) & 0xff;
            uint8_t a2 = (s[i] >> 8) & 0xff, a3 = s[i] & 0xff;
            uint8_t b0 = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
            uint8_t b1 = gmul(a0, 9)  ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
            uint8_t b2 = gmul(a0, 13) ^ gmul(a1, 9)  ^ gmul(a2, 14) ^ gmul(a3, 11);
            uint8_t b3 = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9)  ^ gmul(a3, 14);
            s[i] = ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
        }
        // InvShiftRows + InvSubBytes
        {
            uint8_t t[16];
            for (int i = 0; i < 4; i++) {
                t[4 * i]     = g_rsbox[(s[i] >> 24) & 0xff];
                t[4 * i + 1] = g_rsbox[(s[(i + 3) % 4] >> 16) & 0xff];
                t[4 * i + 2] = g_rsbox[(s[(i + 2) % 4] >> 8) & 0xff];
                t[4 * i + 3] = g_rsbox[s[(i + 1) % 4] & 0xff];
            }
            for (int i = 0; i < 4; i++)
                s[i] = ((uint32_t)t[4 * i] << 24) | ((uint32_t)t[4 * i + 1] << 16)
                     | ((uint32_t)t[4 * i + 2] << 8) | (uint32_t)t[4 * i + 3];
        }
    }
    // Final AddRoundKey(0)
    for (int i = 0; i < 4; i++) {
        uint32_t w = s[i] ^ c->rk[i];
        block[4 * i]     = (w >> 24) & 0xff;
        block[4 * i + 1] = (w >> 16) & 0xff;
        block[4 * i + 2] = (w >> 8) & 0xff;
        block[4 * i + 3] = w & 0xff;
    }
}

void aes128_ctr(const uint8_t key[16], const uint8_t upper[8],
                uint64_t offset, uint8_t *buf, size_t len) {
    aes128_ctx c;
    aes128_init(&c, key);
    size_t skip = (size_t)(offset % 16);
    uint64_t block_idx = offset / 16;
    size_t done = 0;
    uint8_t ks[16];
    while (done < len) {
        if (skip == 0) {
            // Build the counter block: upper (8 bytes) + big-endian block index.
            for (int i = 0; i < 8; i++) ks[i] = upper[i];
            for (int i = 0; i < 8; i++) ks[8 + i] = (uint8_t)((block_idx >> (56 - 8 * i)) & 0xff);
            aes128_encrypt_block(&c, ks);
            block_idx++;
        }
        size_t take = 16 - skip;
        if (take > len - done) take = len - done;
        for (size_t i = 0; i < take; i++) buf[done + i] ^= ks[skip + i];
        done += take;
        skip = 0;
    }
}

void aes256_xts_decrypt(const uint8_t key[32], uint8_t *buf, size_t len,
                        size_t sector_size, uint64_t first_sector) {
    if (sector_size == 0) return;
    aes128_ctx data_c, tweak_c;
    aes128_init(&data_c, key);
    aes128_init(&tweak_c, key + 16);
    size_t off = 0;
    uint64_t sector = first_sector;
    while (off < len) {
        size_t sec_len = len - off;
        if (sec_len > sector_size) sec_len = sector_size;
        // Tweak for this sector: AES-encrypt the big-endian sector number.
        uint8_t tweak[16];
        for (int i = 0; i < 8; i++) tweak[i] = 0;
        for (int i = 0; i < 8; i++) tweak[8 + i] = (uint8_t)((sector >> (56 - 8 * i)) & 0xff);
        aes128_encrypt_block(&tweak_c, tweak);
        // Decrypt each 16-byte block in the sector, advancing the tweak.
        size_t b = 0;
        while (b < sec_len) {
            uint8_t *blk = buf + off + b;
            for (int i = 0; i < 16; i++) blk[i] ^= tweak[i];
            aes128_decrypt_block(&data_c, blk);
            for (int i = 0; i < 16; i++) blk[i] ^= tweak[i];
            // Multiply tweak by x in GF(2^128), little-endian convention.
            uint8_t carry = tweak[15] >> 7;
            for (int i = 15; i > 0; i--) tweak[i] = (uint8_t)((tweak[i] << 1) | (tweak[i - 1] >> 7));
            tweak[0] = (uint8_t)((tweak[0] << 1) ^ (carry ? 0x87 : 0));
            b += 16;
        }
        off += sec_len;
        sector++;
    }
}
