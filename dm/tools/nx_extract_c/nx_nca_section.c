/*
 * nx_nca_section.c — NCA section decryption, hash verification, and
 * decompression for the on-device extractor.
 *
 * Given an opened NCA, this module:
 *   1. Reads the section's FS header (0x200 bytes at 0x400 + i*0x200)
 *   2. Decrypts the section data with AES-128-CTR (section key = title
 *      key for rights-ID NCAs, key-area key for key-area NCAs)
 *   3. Verifies the hash tree (SHA256 hierarchical for PFS0 sections,
 *      IVFC for RomFS sections)
 *   4. Decompresses the section if it has a BKTR compression layer
 *      (NONE = plain copy, ZEROS = zero-fill, LZ4 = LZ4 block decode)
 *
 * The result is the full decrypted, verified, decompressed section data
 * in a newly allocated buffer.
 */
#include "nx_nca_section.h"
#include "aes.h"
#include "lz4.h"
#include "sha256.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define NX_SECTOR_SIZE 0x200ULL
#define NX_FS_HEADER_SIZE 0x200ULL

/* BKTR constants */
#define NX_BKTR_NODE_SIZE    0x4000ULL
#define NX_BKTR_NODE_HEADER  0x10ULL
#define NX_BKTR_ENTRY_SIZE   0x18ULL
#define NX_COMPRESSION_NONE  0
#define NX_COMPRESSION_ZEROS 1
#define NX_COMPRESSION_LZ4   3
#define NX_MAX_LZ4_ENTRY     (1ULL << 20)

/* Hash block size exponent range (from libhac MIN_HASH_BLOCK_EXP..MAX) */
#define NX_MIN_HASH_BLOCK_EXP 12
#define NX_MAX_HASH_BLOCK_EXP 24

/* Helper: u32 LE read */
static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64(const uint8_t *p) {
    return (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32);
}

/* ------------------------------------------------------------------ */
/* Section prepare                                                    */
/* ------------------------------------------------------------------ */

int nx_nca_section_prepare(const nx_nca *nca, int section_index,
                           const uint8_t title_key[16],
                           nx_nca_section *out) {
    memset(out, 0, sizeof(*out));

    if (!nca || section_index < 0 || section_index >= 4)
        return NX_SECT_ERR_DATA;

    /* Check section is enabled */
    if (section_index >= 4)
        return NX_SECT_ERR_DATA;

    /* Section bounds from the NCA section table */
    uint32_t sec_start = nca->sect_offset[section_index];
    uint32_t sec_end   = nca->sect_size[section_index];

    if (sec_start == 0 || sec_end == 0 || sec_start >= sec_end)
        return NX_SECT_ERR_DATA;

    out->start = (uint64_t)sec_start * NX_SECTOR_SIZE;
    out->size  = (uint64_t)(sec_end - sec_start) * NX_SECTOR_SIZE;
    out->section_index = section_index;

    /* Read FS header from the decrypted NCA header */
    const uint8_t *fsh = nca->fs_header[section_index];

    /* FS type (byte 2), hash type (byte 3), encryption type (byte 4) */
    out->fs_type = fsh[2];
    int hash_type = fsh[3];
    int enc_type = fsh[4];

    /* Resolve AUTO hash type based on FS type */
    if (hash_type == NX_HASH_AUTO) {
        hash_type = (out->fs_type == NX_FS_PFS0) ? NX_HASH_SHA256 : NX_HASH_IVFC;
    }
    out->hash_type = hash_type;

    /* Upper counter for AES-CTR at 0x140 (u64 LE) */
    uint64_t upper = rd_u64(fsh + 0x140);
    for (int i = 0; i < 8; i++)
        out->upper[i] = (uint8_t)(upper >> (56 - i * 8));

    /* Section key: the title key is passed in; for rights-ID NCAs it's
     * the decrypted title key, for key-area NCAs it's the key-area key. */
    memcpy(out->key, title_key, 16);

    /* Encryption type: if NONE, skip decryption */
    if (enc_type == 0) {
        /* No encryption — key is irrelevant */
        memset(out->key, 0, 16);
    }

    /* Compression info at 0x178-0x1A0 (u64 offset + u64 size) */
    uint64_t comp_off = rd_u64(fsh + 0x178);
    uint64_t comp_size = rd_u64(fsh + 0x180);
    if (comp_off != 0 && comp_size != 0) {
        out->has_compression = 1;
        out->comp_table_offset = comp_off;
        out->comp_table_size = comp_size;
    } else {
        out->has_compression = 0;
    }

    out->valid = 1;
    return NX_SECT_OK;
}

/* ------------------------------------------------------------------ */
/* Hash tree verification                                             */
/* ------------------------------------------------------------------ */

/* Verify a SHA256 hierarchical hash tree (for PFS0 sections).
 * FS header layout:
 *   0x08: master hash (32 bytes)
 *   0x28: block_size (u32)
 *   0x2C: count (u32) — must be 2
 *   0x30: level 0: offset (u64), size (u64)
 *   0x40: level 1: offset (u64), size (u64)
 * Returns NX_SECT_OK or NX_SECT_ERR_HASH. */
static int verify_sha256_tree(const uint8_t *decrypted, uint64_t size,
                              const uint8_t *fsh, int *out_data_off) {
    /* Validate block size exponent */
    uint32_t block_size = rd_u32(fsh + 0x28);
    uint32_t count = rd_u32(fsh + 0x2C);

    if (count != 2)
        return NX_SECT_ERR_DATA;
    if (block_size == 0 ||
        block_size < (1u << NX_MIN_HASH_BLOCK_EXP) ||
        block_size > (1u << NX_MAX_HASH_BLOCK_EXP))
        return NX_SECT_ERR_DATA;
    if ((block_size & (block_size - 1)) != 0)  /* not a power of two */
        return NX_SECT_ERR_DATA;

    /* Level 0 = hash table, level 1 = data */
    uint64_t lvl0_off = rd_u64(fsh + 0x30);
    uint64_t lvl0_sz  = rd_u64(fsh + 0x38);
    uint64_t lvl1_off = rd_u64(fsh + 0x40);
    uint64_t lvl1_sz  = rd_u64(fsh + 0x48);

    /* Validate level sizes */
    uint64_t num_blocks = (lvl1_sz + block_size - 1) / block_size;
    if (lvl0_sz != num_blocks * 32)
        return NX_SECT_ERR_DATA;

    /* Verify each data block hash */
    for (uint64_t b = 0; b < num_blocks; b++) {
        uint8_t hash[32];
        const uint8_t *data_block = decrypted + lvl1_off + b * block_size;
        uint64_t block_actual = (b == num_blocks - 1)
            ? (lvl1_sz - b * block_size)
            : block_size;
        nx_sha256_hash(data_block, block_actual, hash);

        if (memcmp(hash, decrypted + lvl0_off + b * 32, 32) != 0)
            return NX_SECT_ERR_HASH;
    }

    /* Hash the hash table */
    uint8_t table_hash[32];
    nx_sha256_hash(decrypted + lvl0_off, lvl0_sz, table_hash);

    /* Compare against master hash at 0x08 */
    if (memcmp(table_hash, fsh + 0x08, 32) != 0)
        return NX_SECT_ERR_HASH;

    /* The data starts after the hash tree levels.
     * The PFS0 data offset = section_start + level 0 offset. */
    if (out_data_off)
        *out_data_off = lvl1_off;

    return NX_SECT_OK;
}

/* Verify an IVFC hash tree (for RomFS sections).
 * FS header layout:
 *   0x08: "IVFC" magic
 *   0x0C: reserved (0x14 bytes)
 *   0x14: level count (u32) — 2..7
 *   0x18: levels (0x18 bytes each): offset (u64), size (u64), exp (u32)
 *   0xC8: master hash (32 bytes)
 * The last level is the data; intermediate levels are hash tables.
 * Returns NX_SECT_OK or NX_SECT_ERR_HASH. */
static int verify_ivfc_tree(const uint8_t *decrypted, uint64_t size,
                            const uint8_t *fsh, int *out_data_off,
                            uint64_t *out_data_size) {
    if (memcmp(fsh + 0x08, "IVFC", 4) != 0)
        return NX_SECT_ERR_DATA;

    uint32_t count = rd_u32(fsh + 0x14);
    if (count < 2 || count > 7)
        return NX_SECT_ERR_DATA;

    /* Read all levels */
    uint64_t lvl_off[6], lvl_sz[6], lvl_block[6];
    for (uint32_t l = 0; l < count - 1; l++) {
        const uint8_t *lv = fsh + 0x18 + l * 0x18;
        lvl_off[l]  = rd_u64(lv);
        lvl_sz[l]   = rd_u64(lv + 8);
        uint32_t exp = rd_u32(lv + 0x10);
        if (exp < NX_MIN_HASH_BLOCK_EXP || exp > NX_MAX_HASH_BLOCK_EXP)
            return NX_SECT_ERR_DATA;
        lvl_block[l] = 1ULL << exp;
    }

    /* Master hash at 0xC8 */
    const uint8_t *master = fsh + 0xC8;

    /* Verify bottom-up: level count-2 is data, verified by level count-3. */
    uint64_t data_off = lvl_off[count - 2];
    uint64_t data_sz  = lvl_sz[count - 2];

    uint32_t hash_level = count - 3;
    uint64_t block_size = lvl_block[hash_level];

    uint64_t num_blocks = (data_sz + block_size - 1) / block_size;

    for (uint64_t b = 0; b < num_blocks; b++) {
        uint8_t hash[32];
        const uint8_t *block = decrypted + data_off + b * block_size;
        uint64_t actual = (b == num_blocks - 1)
            ? (data_sz - b * block_size)
            : block_size;
        /* IVFC zero-pads the last partial block */
        nx_sha256_hash(block, actual, hash);

        if (memcmp(hash, decrypted + lvl_off[hash_level] + b * 32, 32) != 0)
            return NX_SECT_ERR_HASH;
    }

    /* Now hash each level and verify against the level above, up to master */
    uint8_t cur_hash[32];
    {
        uint32_t cur_level = hash_level;
        while (cur_level > 0) {
            uint8_t hash[32];
            nx_sha256_hash(decrypted + lvl_off[cur_level], lvl_sz[cur_level], hash);
            if (memcmp(hash, decrypted + lvl_off[cur_level - 1], 32) != 0)
                return NX_SECT_ERR_HASH;
            cur_level--;
        }
    }

    /* Final: verify the top level against the master hash */
    nx_sha256_hash(decrypted + lvl_off[0], lvl_sz[0], cur_hash);
    if (memcmp(cur_hash, master, 32) != 0)
        return NX_SECT_ERR_HASH;

    if (out_data_off)  *out_data_off = data_off;
    if (out_data_size) *out_data_size = data_sz;

    return NX_SECT_OK;
}

/* ------------------------------------------------------------------ */
/* BKTR compression tree reading                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t virt;       /* virtual offset in decompressed data */
    uint64_t phys;       /* physical offset in verified data */
    uint8_t  kind;       /* 0=NONE, 1=ZEROS, 3=LZ4 */
    uint32_t phys_size;  /* physical size (bytes) */
} nx_compressed_entry;

/* Read all entries from a BKTR bucket tree.
 * info is the compression info from the FS header (0x178: table_offset,
 * 0x180: table_size, then BKTR header at +0x10).
 * Returns the tree end offset (= decompressed data size) or negative. */
static long bktr_read_entries(const uint8_t *verified, uint64_t verified_size,
                               const uint8_t *comp_info,
                               nx_compressed_entry **out_entries,
                               uint64_t *out_count,
                               uint64_t *out_data_size) {
    uint64_t table_offset = rd_u64(comp_info);
    uint64_t table_size   = rd_u64(comp_info + 8);

    if (memcmp(comp_info + 0x10, "BKTR", 4) != 0)
        return NX_SECT_ERR_DATA;

    uint32_t count = rd_u32(comp_info + 0x18);

    /* Compute layout */
    uint64_t per_set = (NX_BKTR_NODE_SIZE - NX_BKTR_NODE_HEADER) / NX_BKTR_ENTRY_SIZE;
    uint64_t offsets_per_node = (NX_BKTR_NODE_SIZE - NX_BKTR_NODE_HEADER) / 8;
    uint64_t sets = (count + per_set - 1) / per_set;

    uint64_t l2_nodes = 0;
    if (sets > offsets_per_node) {
        uint64_t n = (sets + offsets_per_node - 1) / offsets_per_node;
        if (n > offsets_per_node)
            return NX_SECT_ERR_DATA;
        l2_nodes = (sets - (offsets_per_node - (n - 1))) / offsets_per_node;
        if ((sets - (offsets_per_node - (n - 1))) % offsets_per_node != 0)
            l2_nodes++;
    }

    uint64_t node_storage = NX_BKTR_NODE_SIZE * (l2_nodes + 1);

    if (node_storage + sets * NX_BKTR_NODE_SIZE > table_size)
        return NX_SECT_ERR_DATA;
    if (table_offset + table_size > verified_size)
        return NX_SECT_ERR_DATA;

    /* Read L1 node */
    const uint8_t *l1 = verified + table_offset;
    uint64_t tree_end = rd_u64(l1 + 8);

    /* Allocate entries */
    *out_entries = calloc(count, sizeof(nx_compressed_entry));
    if (!*out_entries)
        return NX_SECT_ERR_NOMEM;
    *out_count = 0;

    uint64_t seen = 0;
    uint64_t prev = 0;
    int have_prev = 0;

    for (uint64_t set = 0; set < sets; set++) {
        const uint8_t *node = verified + table_offset + node_storage + set * NX_BKTR_NODE_SIZE;
        uint32_t n = rd_u32(node + 4);
        if (n > per_set)
            return NX_SECT_ERR_DATA;

        for (uint32_t k = 0; k < n; k++) {
            const uint8_t *e = node + NX_BKTR_NODE_HEADER + k * NX_BKTR_ENTRY_SIZE;
            uint64_t off = rd_u64(e);
            if ((!have_prev && off != 0) || (have_prev && off <= prev))
                return NX_SECT_ERR_DATA;
            prev = off;
            have_prev = 1;

            nx_compressed_entry *ent = &(*out_entries)[(*out_count)++];
            ent->virt       = off;
            ent->phys       = rd_u64(e + 8);
            ent->kind       = e[0x10];
            ent->phys_size  = rd_u32(e + 0x14);
        }
        seen += n;
    }

    if (seen != count)
        return NX_SECT_ERR_DATA;
    if (have_prev && prev >= tree_end)
        return NX_SECT_ERR_DATA;

    *out_data_size = tree_end;
    return NX_SECT_OK;
}

/* Decompress the full verified data using the BKTR tree into out_buf.
 * Returns the decompressed size or a negative error. */
static long bktr_decompress_all(const uint8_t *verified, uint64_t verified_size,
                                const uint8_t *comp_info,
                                uint8_t **out_buf,
                                uint64_t *out_size) {
    nx_compressed_entry *entries = NULL;
    uint64_t count = 0, data_size = 0;
    long rc = bktr_read_entries(verified, verified_size, comp_info,
                                &entries, &count, &data_size);
    if (rc != NX_SECT_OK) {
        free(entries);
        return rc;
    }

    uint8_t *buf = malloc(data_size > 0 ? data_size : 1);
    if (!buf) {
        free(entries);
        return NX_SECT_ERR_NOMEM;
    }

    for (uint64_t i = 0; i < count; i++) {
        const nx_compressed_entry *e = &entries[i];
        uint64_t end = (i + 1 < count) ? entries[i + 1].virt : data_size;
        uint64_t len = end - e->virt;

        if (e->kind == NX_COMPRESSION_NONE) {
            if (e->phys + len > verified_size) {
                free(buf); free(entries);
                return NX_SECT_ERR_DATA;
            }
            memcpy(buf + e->virt, verified + e->phys, len);
        } else if (e->kind == NX_COMPRESSION_ZEROS) {
            memset(buf + e->virt, 0, len);
        } else if (e->kind == NX_COMPRESSION_LZ4) {
            if (len > NX_MAX_LZ4_ENTRY || e->phys_size > NX_MAX_LZ4_ENTRY) {
                free(buf); free(entries);
                return NX_SECT_ERR_DATA;
            }
            /* Sanity: LZ4 can expand at most ~255x */
            if (len > (uint64_t)(e->phys_size + 1) * 255) {
                free(buf); free(entries);
                return NX_SECT_ERR_DATA;
            }
            long n = lz4_decompress_block(verified + e->phys, e->phys_size,
                                          buf + e->virt, len);
            if (n != (long)len) {
                free(buf); free(entries);
                return NX_SECT_ERR_DECOMP;
            }
        } else {
            free(buf); free(entries);
            return NX_SECT_ERR_DATA;
        }
    }

    free(entries);
    *out_buf = buf;
    *out_size = data_size;
    return NX_SECT_OK;
}

/* ------------------------------------------------------------------ */
/* Section read                                                       */
/* ------------------------------------------------------------------ */

long nx_nca_section_read(const nx_nca *nca, const nx_nca_section *sec,
                         const uint8_t *nca_data, uint64_t nca_size,
                         uint8_t **out_data) {
    *out_data = NULL;
    if (!nca || !sec || !sec->valid)
        return NX_SECT_ERR_DATA;

    /* Check bounds */
    if (sec->start + sec->size > nca_size)
        return NX_SECT_ERR_RANGE;

    /* Read the FS header for this section */
    const uint8_t *fsh = nca->fs_header[sec->section_index];

    /* Allocate buffer for the full section data */
    uint8_t *decrypted = malloc(sec->size);
    if (!decrypted)
        return NX_SECT_ERR_NOMEM;

    /* Copy section data from the NCA */
    memcpy(decrypted, nca_data + sec->start, sec->size);

    /* AES-CTR decrypt */
    int enc_type = fsh[4];
    if (enc_type == 1) {  /* AES-CTR */
        /* Build the CTR upper bytes from the FS header */
        uint64_t upper = rd_u64(fsh + 0x140);
        uint8_t upper_be[8];
        for (int i = 0; i < 8; i++)
            upper_be[i] = (uint8_t)(upper >> (56 - i * 8));
        aes128_ctr(sec->key, upper_be, 0, decrypted, sec->size);
    }

    /* Hash tree verification */
    int data_off = 0;
    uint64_t data_size = 0;
    int hash_rc;

    if (sec->hash_type == NX_HASH_SHA256) {
        hash_rc = verify_sha256_tree(decrypted, sec->size, fsh, &data_off);
        if (hash_rc != NX_SECT_OK) {
            free(decrypted);
            return hash_rc;
        }
        data_size = sec->size - data_off;
    } else if (sec->hash_type == NX_HASH_IVFC) {
        hash_rc = verify_ivfc_tree(decrypted, sec->size, fsh, &data_off, &data_size);
        if (hash_rc != NX_SECT_OK) {
            free(decrypted);
            return hash_rc;
        }
    } else {
        /* Unknown hash type — skip verification */
        data_off = 0;
        data_size = sec->size;
    }

    /* Compression layer */
    if (sec->has_compression) {
        /* The compression table is in the verified data area.
         * Build the compression info buffer (table offset, size, BKTR header). */
        uint8_t comp_info[0x20];
        nx_wr_u64le(comp_info, sec->comp_table_offset);
        nx_wr_u64le(comp_info + 8, sec->comp_table_size);
        /* Copy the BKTR header (16 bytes starting at comp_table_offset + 0x10
         * in the verified data) */
        uint64_t bktr_off = sec->comp_table_offset + 0x10;
        if (bktr_off + 0x10 > data_size) {
            free(decrypted);
            return NX_SECT_ERR_DATA;
        }
        memcpy(comp_info + 0x10, decrypted + bktr_off, 0x10);

        uint8_t *decompressed = NULL;
        uint64_t decomp_size = 0;
        long rc = bktr_decompress_all(decrypted + data_off, data_size,
                                      comp_info, &decompressed, &decomp_size);
        free(decrypted);
        if (rc != NX_SECT_OK)
            return rc;

        *out_data = decompressed;
        return (long)decomp_size;
    }

    /* No compression: return the verified data (from data_off onward) */
    if (data_off > 0) {
        uint8_t *result = malloc(data_size > 0 ? data_size : 1);
        if (!result)
            return NX_SECT_ERR_NOMEM;
        memcpy(result, decrypted + data_off, data_size);
        free(decrypted);
        *out_data = result;
        return (long)data_size;
    }

    *out_data = decrypted;
    return (long)sec->size;
}
