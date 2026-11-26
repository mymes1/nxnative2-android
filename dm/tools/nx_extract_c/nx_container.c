/*
 * nx_container.c — Nintendo container parsers for the on-device extractor.
 *
 * Pure C, no external dependencies beyond aes.h (AES-128) and the system
 * SHA-256 (CC_SHA256_*).  Implements PFS0/HFS0, XCI, NCA header decryption,
 * and RomFS traversal.
 */
#include "nx_container.h"
#include "aes.h"
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Little-endian helpers                                              */
/* ------------------------------------------------------------------ */

uint16_t nx_rd_u16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t nx_rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t nx_rd_u64le(const uint8_t *p) {
    return (uint64_t)nx_rd_u32le(p) | ((uint64_t)nx_rd_u32le(p + 4) << 32);
}

void nx_wr_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

void nx_wr_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

void nx_wr_u64le(uint8_t *p, uint64_t v) {
    nx_wr_u32le(p, (uint32_t)(v & 0xFFFFFFFF));
    nx_wr_u32le(p + 4, (uint32_t)(v >> 32));
}

/* ------------------------------------------------------------------ */
/* PFS0 / HFS0                                                        */
/* ------------------------------------------------------------------ */

int nx_pfs0_parse(const uint8_t *data, uint64_t data_len, uint64_t base,
                  nx_pfs0 *out) {
    memset(out, 0, sizeof(*out));

    if (base + 0x10 > data_len) return NX_CNT_ERR_RANGE;

    const uint8_t *hdr = data + base;
    if (memcmp(hdr, NX_PFS0_MAGIC, 4) == 0) {
        out->is_hfs0 = 0;
    } else if (memcmp(hdr, NX_HFS0_MAGIC, 4) == 0) {
        out->is_hfs0 = 1;
    } else {
        return NX_CNT_ERR_MAGIC;
    }

    uint64_t entry_size = out->is_hfs0 ? NX_HFS0_ENTRY_SIZE : NX_PFS0_ENTRY_SIZE;
    uint32_t count = nx_rd_u32le(hdr + 4);
    uint32_t str_size = nx_rd_u32le(hdr + 8);

    out->header_offset = base;
    out->header_size = 0x10 + (uint64_t)count * entry_size + str_size;
    out->data_start = out->header_offset + out->header_size;

    if (out->header_size > data_len || out->data_start > data_len)
        return NX_CNT_ERR_RANGE;

    out->count = count;
    out->entries = calloc(count > 0 ? count : 1, sizeof(nx_pfs0_entry));
    if (!out->entries) return NX_CNT_ERR_NOMEM;

    const uint8_t *str_base = hdr + 0x10 + (uint64_t)count * entry_size;
    uint64_t names_total = 0;

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = hdr + 0x10 + (uint64_t)i * entry_size;
        uint64_t rel_off = nx_rd_u64le(e);
        uint64_t size   = nx_rd_u64le(e + 8);
        uint32_t name_off = nx_rd_u32le(e + 16);

        if (name_off >= str_size) {
            nx_container_free(out);
            return NX_CNT_ERR_DATA;
        }

        /* Read the name (NUL-terminated, but bounded by str_size) */
        const uint8_t *name_start = str_base + name_off;
        size_t max_name = str_size - name_off;
        if (max_name > sizeof(out->entries[i].name) - 1)
            max_name = sizeof(out->entries[i].name) - 1;

        size_t nlen = 0;
        while (nlen < max_name && name_start[nlen] != 0) nlen++;
        memcpy(out->entries[i].name, name_start, nlen);
        out->entries[i].name[nlen] = '\0';
        names_total += nlen;
        if (names_total > str_size) {
            nx_container_free(out);
            return NX_CNT_ERR_DATA;
        }

        out->entries[i].offset = out->data_start + rel_off;
        out->entries[i].size   = size;

        if (out->entries[i].offset > data_len ||
            out->entries[i].offset + size > data_len) {
            nx_container_free(out);
            return NX_CNT_ERR_RANGE;
        }
    }

    out->valid = 1;
    return NX_CNT_OK;
}

void nx_container_free(nx_pfs0 *pfs) {
    if (!pfs) return;
    free(pfs->entries);
    pfs->entries = NULL;
    pfs->count = 0;
    pfs->valid = 0;
}

/* ------------------------------------------------------------------ */
/* XCI                                                                */
/* ------------------------------------------------------------------ */

int nx_xci_parse(const uint8_t *data, uint64_t data_len, nx_xci *out) {
    memset(out, 0, sizeof(*out));

    /* The card header is at 0x100 ("HEAD") or 0x1000 (with key area). */
    uint64_t hdr_off = 0;
    if (data_len < NX_XCI_HEADER_SIZE) return NX_CNT_ERR_RANGE;

    if (data_len >= 0x100 + 4 && memcmp(data + 0x100, "HEAD", 4) != 0) {
        hdr_off = NX_XCI_KEY_AREA;
        if (data_len < hdr_off + NX_XCI_HEADER_SIZE) return NX_CNT_ERR_RANGE;
        if (memcmp(data + hdr_off + 0x100, "HEAD", 4) != 0)
            return NX_CNT_ERR_MAGIC;
    } else if (data_len >= 0x100 + 4 && memcmp(data + 0x100, "HEAD", 4) == 0) {
        hdr_off = 0;
    } else {
        return NX_CNT_ERR_MAGIC;
    }

    out->header_offset = hdr_off;

    const uint8_t *hdr = data + hdr_off;
    uint64_t root_off  = hdr_off + nx_rd_u64le(hdr + 0x130);
    uint64_t root_hdr_size = nx_rd_u64le(hdr + 0x138);

    out->root_offset = root_off;
    out->root_header_size = root_hdr_size;

    /* The root is an HFS0 partition. Limit is the image size rounded up
     * to the media unit (0x200). */
    uint64_t limit = (data_len + 0x1FF) & ~0x1FFULL;
    int rc = nx_pfs0_parse(data, limit, root_off, &out->root);
    if (rc != NX_CNT_OK) return rc;

    out->valid = 1;
    return NX_CNT_OK;
}

/* ------------------------------------------------------------------ */
/* NCA                                                                */
/* ------------------------------------------------------------------ */

int nx_nca_open(const uint8_t *data, uint64_t data_len,
                const uint8_t header_key[32],
                const uint8_t key_area_key[16],
                const uint8_t titlekek[16],
                nx_nca *out) {
    memset(out, 0, sizeof(*out));

    if (data_len < NX_NCA_HEADER_SIZE) return NX_CNT_ERR_RANGE;

    /* Work on a copy of the first 0xC00 bytes */
    uint8_t *hdr = malloc(NX_NCA_HEADER_SIZE);
    if (!hdr) return NX_CNT_ERR_NOMEM;
    memcpy(hdr, data, NX_NCA_HEADER_SIZE);

    /* Decrypt the header with AES-256-XTS, sector size 0x200.
     * First 0x400 bytes: sector 0.  For NCA3, bytes 0x400..: sector 2.
     * For NCA2, sectors 0..3 at 0x400 + i * 0x200. */
    aes256_xts_decrypt(header_key, hdr, 0x400, 0x200, 0);

    if (memcmp(hdr + 0x200, "NCA3", 4) == 0) {
        aes256_xts_decrypt(header_key, hdr + 0x400, 0x800, 0x200, 2);
    } else if (memcmp(hdr + 0x200, "NCA2", 4) == 0) {
        for (int i = 0; i < 4; i++) {
            aes256_xts_decrypt(header_key, hdr + 0x400 + i * 0x200, 0x200,
                               0x200, (uint64_t)i);
        }
    } else {
        free(hdr);
        return NX_CNT_ERR_CRYPTO;
    }

    memcpy(out->magic, hdr + 0x200, 4);
    out->distribution_type          = hdr[0x204];
    out->content_type               = hdr[0x205];
    out->key_generation             = hdr[0x206];
    out->key_area_key_index         = hdr[0x207];
    out->signature_key_generation   = hdr[0x221];
    out->key_generation_old         = hdr[0x220];
    out->program_id                 = nx_rd_u64le(hdr + 0x210);
    memcpy(out->rights_id, hdr + 0x230, 16);

    /* Decrypt the key area (at 0x320, 16 bytes) with AES-128-ECB.
     * `key_area_key` is the correct key for this NCA's key generation,
     * already selected by the caller from prod.keys. */
    if (key_area_key) {
        uint8_t encrypted_key[16];
        memcpy(encrypted_key, hdr + 0x320, 16);
        aes128_ctx ctx;
        aes128_init(&ctx, key_area_key);
        aes128_decrypt_block(&ctx, encrypted_key);
        memcpy(out->key_area_key, encrypted_key, 16);
    }

    if (titlekek) {
        memcpy(out->titlekek, titlekek, 16);
    }

    /* Section table at 0x240: 4 entries, each 0x10 bytes.
     * Each entry: offset (u32, in 0x200 sectors), size (u32, in 0x200 sectors). */
    for (int i = 0; i < 4; i++) {
        const uint8_t *se = hdr + 0x240 + i * 0x10;
        out->sect_offset[i] = nx_rd_u32le(se);
        out->sect_size[i]   = nx_rd_u32le(se + 4);
    }

    /* FS headers at 0x400 + i * 0x200 */
    for (int i = 0; i < 4; i++) {
        memcpy(out->fs_header[i], hdr + 0x400 + i * 0x200, 0x200);
    }

    free(hdr);
    return NX_CNT_OK;
}

void nx_nca_fs_header_parse(const uint8_t raw[0x200], nx_nca_fs_header *out) {
    memset(out, 0, sizeof(*out));
    out->fs_type         = raw[2];
    out->hash_type       = raw[3];
    out->encryption_type = raw[4];
    out->is_patch        = (nx_rd_u64le(raw + 0x108) != 0) ? 1 : 0;
    out->has_sparse      = (nx_rd_u16le(raw + 0x170) != 0) ? 1 : 0;

    uint64_t comp_off = nx_rd_u64le(raw + 0x178);
    uint64_t comp_sz  = nx_rd_u64le(raw + 0x180);
    out->has_compression = (comp_off != 0 && comp_sz != 0) ? 1 : 0;
    out->comp_table_offset = comp_off;
    out->comp_table_size   = comp_sz;

    /* Upper counter at 0x140: u64 LE, stored big-endian in the AES-CTR key */
    out->upper_counter = nx_rd_u64le(raw + 0x140);
}

/* ------------------------------------------------------------------ */
/* RomFS                                                              */
/* ------------------------------------------------------------------ */

/* RomFS entries are variable-sized: fixed fields followed by a u32
 * name length and the name bytes inline.  Pointers (sibling, child,
 * parent) are byte offsets into the table, not entry indices.
 *
 * Dir entry (min 0x18 bytes):
 *   +0x00  parent     (u32, byte offset in dir table)
 *   +0x04  sibling    (u32, byte offset of next dir at same level)
 *   +0x08  child_dir  (u32, byte offset of first child dir)
 *   +0x0C  child_file (u32, byte offset of first child file in file table)
 *   +0x10  reserved   (u32)
 *   +0x14  name_len   (u32)
 *   +0x18  name       (name_len bytes)
 *
 * File entry (min 0x20 bytes):
 *   +0x00  parent     (u32, byte offset of parent dir in dir table)
 *   +0x04  sibling    (u32, byte offset of next file at same level)
 *   +0x08  data_off   (u64, offset of file data relative to data_off)
 *   +0x10  data_size  (u64)
 *   +0x18  reserved   (u32)
 *   +0x1C  name_len   (u32)
 *   +0x20  name       (name_len bytes)
 */

#define NX_ROMFS_NONE 0xFFFFFFFFu

int nx_romfs_open(const uint8_t *data, uint64_t data_len, nx_romfs *out) {
    memset(out, 0, sizeof(*out));

    if (data_len < 0x50) return NX_CNT_ERR_RANGE;

    out->dir_meta_off  = nx_rd_u64le(data + 0x18);
    out->dir_meta_size = nx_rd_u64le(data + 0x20);
    out->file_meta_off = nx_rd_u64le(data + 0x38);
    out->file_meta_size = nx_rd_u64le(data + 0x40);
    out->data_off      = nx_rd_u64le(data + 0x48);

    if (out->dir_meta_size > 0x10000000 || out->file_meta_size > 0x10000000)
        return NX_CNT_ERR_DATA;

    if (out->dir_meta_off > data_len ||
        out->dir_meta_off + out->dir_meta_size > data_len)
        return NX_CNT_ERR_RANGE;
    if (out->file_meta_off > data_len ||
        out->file_meta_off + out->file_meta_size > data_len)
        return NX_CNT_ERR_RANGE;

    out->valid = 1;
    return NX_CNT_OK;
}

/* Read the name from a RomFS entry at byte offset `off` in `table`.
 * `len_off` is the offset of the name-length field within the entry. */
static int romfs_read_name(const uint8_t *table, uint64_t table_size,
                           uint64_t off, uint32_t len_off,
                           char *out_name, size_t out_cap) {
    if (off + len_off + 4 > table_size) return -1;

    uint32_t name_len = nx_rd_u32le(table + off + len_off);
    uint64_t name_start = off + len_off + 4;

    if (name_len == 0 || name_len >= out_cap) return -1;
    if (name_start + name_len > table_size) return -1;

    memcpy(out_name, table + name_start, name_len);
    out_name[name_len] = '\0';
    return (int)name_len;
}

/* Find a sibling by name, walking the sibling chain from `start`.
 * `start` is a byte offset in the table.  Returns the byte offset of
 * the matching entry, or NX_ROMFS_NONE. */
static uint32_t romfs_find_sibling(const uint8_t *table, uint64_t table_size,
                                   uint32_t start, const char *name,
                                   int is_dir) {
    uint32_t len_off = is_dir ? 0x14 : 0x1C;
    uint32_t cur = start;
    char name_buf[256];

    while (cur != NX_ROMFS_NONE) {
        uint64_t off = cur;
        if (off >= table_size) break;

        if (romfs_read_name(table, table_size, off, len_off,
                            name_buf, sizeof(name_buf)) < 0)
            break;

        if (strcmp(name_buf, name) == 0)
            return cur;

        cur = nx_rd_u32le(table + off + 0x04);
    }
    return NX_ROMFS_NONE;
}

int nx_romfs_read_file(const nx_romfs *rfs, const uint8_t *data, uint64_t data_len,
                       const char *path, uint8_t **out_data, uint64_t *out_size) {
    *out_data = NULL;
    *out_size = 0;

    if (!rfs->valid) return NX_CNT_ERR_DATA;

    const uint8_t *dir_table = data + rfs->dir_meta_off;
    const uint8_t *file_table = data + rfs->file_meta_off;

    /* Split the path into components */
    const char *p = path;
    uint32_t cur_dir = 0; /* root is at byte offset 0 */

    while (*p) {
        while (*p == '/') p++;
        if (*p == '\0') break;

        char comp[256];
        size_t ci = 0;
        while (*p && *p != '/' && ci < sizeof(comp) - 1) {
            comp[ci++] = *p++;
        }
        comp[ci] = '\0';

        const char *rest = p;
        while (*rest == '/') rest++;
        int is_last = (*rest == '\0');

        if (is_last) {
            /* Look up as a file: first child file is at dir_entry + 0x0C */
            uint32_t first_file = nx_rd_u32le(dir_table + cur_dir + 0x0C);
            uint32_t file_off = romfs_find_sibling(
                file_table, rfs->file_meta_size,
                first_file, comp, 0);

            if (file_off == NX_ROMFS_NONE) return NX_CNT_ERR_DATA;

            /* File entry: data_off at +0x08, data_size at +0x10 */
            uint64_t rel_off = nx_rd_u64le(file_table + file_off + 0x08);
            uint64_t f_size  = nx_rd_u64le(file_table + file_off + 0x10);
            uint64_t f_abs   = rfs->data_off + rel_off;

            if (f_abs > data_len || f_abs + f_size > data_len)
                return NX_CNT_ERR_RANGE;

            uint8_t *buf = malloc(f_size > 0 ? f_size : 1);
            if (!buf) return NX_CNT_ERR_NOMEM;
            memcpy(buf, data + f_abs, f_size);
            *out_data = buf;
            *out_size = f_size;
            return NX_CNT_OK;
        } else {
            /* Look up as a directory: first child dir is at dir_entry + 0x08 */
            uint32_t first_dir = nx_rd_u32le(dir_table + cur_dir + 0x08);
            uint32_t dir_off = romfs_find_sibling(
                dir_table, rfs->dir_meta_size,
                first_dir, comp, 1);

            if (dir_off == NX_ROMFS_NONE) return NX_CNT_ERR_DATA;
            cur_dir = dir_off;
        }
    }

    return NX_CNT_ERR_DATA;
}
