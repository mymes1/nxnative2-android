/*
 * nx_nca_section.h — NCA section decryption, hash verification, and
 * decompression for the on-device extractor.
 *
 * Given an opened NCA (nx_nca), this module reads a section's raw data,
 * decrypts it (AES-CTR), verifies the hash tree (SHA256 or IVFC), and
 * decompresses it (LZ4) when the section has a compression layer.
 */
#ifndef NX_NCA_SECTION_H
#define NX_NCA_SECTION_H

#include "nx_container.h"
#include <stdint.h>
#include <stddef.h>

/* Error codes (negative) */
#define NX_SECT_OK           0
#define NX_SECT_ERR_RANGE   (-1)  /* offset/size out of bounds */
#define NX_SECT_ERR_HASH    (-2)  /* hash verification failed */
#define NX_SECT_ERR_DECRYPT (-3)  /* decryption failed (missing key) */
#define NX_SECT_ERR_DECOMP  (-4)  /* LZ4 decompression failed */
#define NX_SECT_ERR_DATA    (-5)  /* malformed section data */
#define NX_SECT_ERR_NOMEM   (-6)  /* allocation failure */

/* Section hash types (from the NCA FS header, byte 3) */
#define NX_HASH_AUTO    0
#define NX_HASH_SHA256  1
#define NX_HASH_IVFC    2

/* Section FS types (from the NCA FS header, byte 2) */
#define NX_FS_PFS0      0
#define NX_FS_ROMFS     1

/* ------------------------------------------------------------------ */
/* Section reader                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    int       valid;
    int       section_index;   /* which NCA section (0-3) */
    /* Section bounds in the NCA (absolute offsets, in bytes) */
    uint64_t  start;     /* absolute offset in the NCA data */
    uint64_t  size;      /* size in bytes */
    /* Decryption key (16 bytes, AES-128) */
    uint8_t   key[16];
    /* Upper counter for AES-CTR (8 bytes, from the FS header) */
    uint8_t   upper[8];
    /* Hash type */
    int       hash_type;
    /* FS type */
    int       fs_type;
    /* Compression present */
    int       has_compression;
    /* Compression table offset and size (relative to section start) */
    uint64_t  comp_table_offset;
    uint64_t  comp_table_size;
} nx_nca_section;

/* Prepare a section reader from an opened NCA and a section index (0-3).
 * `title_key` is the 16-byte decrypted title key (or key-area key for
 * NCAs without a rights ID).  Returns NX_SECT_OK or an error code. */
int nx_nca_section_prepare(const nx_nca *nca, int section_index,
                           const uint8_t title_key[16],
                           nx_nca_section *out);

/* Read the full decrypted, verified, decompressed section data into a
 * newly allocated buffer.  Returns the size on success, or a negative
 * error code.  The caller frees the returned buffer. */
long nx_nca_section_read(const nx_nca *nca, const nx_nca_section *sec,
                         const uint8_t *nca_data, uint64_t nca_size,
                         uint8_t **out_data);

#endif /* NX_NCA_SECTION_H */
