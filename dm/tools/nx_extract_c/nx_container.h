/*
 * nx_container.h — Nintendo container parsers for the on-device extractor.
 *
 * Parses PFS0 (NSP/NCA section), XCI (game card), NCA (Nintendo Content
 * Archive), and RomFS (read-only file system) formats in pure C, without
 * external dependencies beyond aes.h and the system SHA-256.
 *
 * All offsets are absolute within the provided buffer. All multi-byte
 * integers are little-endian unless noted.
 */
#ifndef NX_CONTAINER_H
#define NX_CONTAINER_H

#include <stdint.h>
#include <stddef.h>

#define NX_CNT_OK         0
#define NX_CNT_ERR_MAGIC  (-1)  /* wrong magic */
#define NX_CNT_ERR_RANGE  (-2)  /* offset/size out of bounds */
#define NX_CNT_ERR_DATA   (-3)  /* malformed header */
#define NX_CNT_ERR_NOMEM  (-4)  /* allocation failure */
#define NX_CNT_ERR_CRYPTO (-5)  /* decryption failed */

/* ------------------------------------------------------------------ */
/* PFS0 / HFS0                                                         */
/* ------------------------------------------------------------------ */

#define NX_PFS0_ENTRY_SIZE 0x18
#define NX_HFS0_ENTRY_SIZE 0x40
#define NX_PFS0_MAGIC      "PFS0"
#define NX_HFS0_MAGIC      "HFS0"

/* One file in a PFS0/HFS0 partition */
typedef struct {
    char     name[256];     /* NUL-terminated file name */
    uint64_t offset;        /* absolute offset of file data in the source */
    uint64_t size;          /* file size in bytes */
} nx_pfs0_entry;

/* A parsed PFS0/HFS0 partition */
typedef struct {
    int            valid;
    int            is_hfs0;       /* 1 = HFS0 (0x40 entries), 0 = PFS0 */
    uint64_t       header_offset; /* offset of the partition header */
    uint64_t       data_start;    /* absolute offset where file data begins */
    uint64_t       header_size;   /* size of the header (incl. entries + names) */
    int            count;         /* number of file entries */
    nx_pfs0_entry *entries;       /* caller frees with nx_container_free() */
} nx_pfs0;

/* Parse a PFS0 or HFS0 partition at `base` within `data` (length `data_len`). */
int nx_pfs0_parse(const uint8_t *data, uint64_t data_len, uint64_t base,
                  nx_pfs0 *out);

/* ------------------------------------------------------------------ */
/* XCI (game card image)                                              */
/* ------------------------------------------------------------------ */

#define NX_XCI_HEADER_SIZE 0x200
#define NX_XCI_KEY_AREA    0x1000

typedef struct {
    int       valid;
    uint64_t  header_offset;   /* 0 or 0x1000 (with key area) */
    uint64_t  root_offset;     /* absolute offset of the root HFS0 */
    uint64_t  root_header_size;
    nx_pfs0   root;            /* the root HFS0 partition */
} nx_xci;

/* Parse an XCI image. `header_key` (32 bytes) is not needed for parsing
 * the structure — only for decrypting NCAs inside the secure partition. */
int nx_xci_parse(const uint8_t *data, uint64_t data_len, nx_xci *out);

/* ------------------------------------------------------------------ */
/* NCA (Nintendo Content Archive)                                      */
/* ------------------------------------------------------------------ */

#define NX_NCA_HEADER_SIZE 0xC00
#define NX_NCA_SECT_SIZE   0x200

typedef struct {
    uint8_t  magic[4];          /* "NCA3" or "NCA2" */
    uint8_t  distribution_type; /* 0 = download, 1 = game card */
    uint8_t  content_type;      /* 0=Program, 1=Meta, 2=Control, 3=Data, ... */
    uint8_t  key_generation;    /* header[0x206] */
    uint8_t  key_generation_old; /* header[0x220] */
    uint8_t  key_area_key_index; /* header[0x207] */
    uint8_t  signature_key_generation; /* header[0x221] */
    uint64_t program_id;        /* header[0x210] */
    uint8_t  rights_id[16];     /* header[0x230] */
    uint8_t  key_area_key[16];  /* decrypted AES-CTR key for the key area */
    uint8_t  titlekek[16];      /* title-key decryption key (may be zero) */
    /* Section table: 4 entries at 0x240, each 0x10 bytes (offset, size in sectors) */
    uint32_t sect_offset[4];
    uint32_t sect_size[4];
    /* Decrypted FS headers (0x200 bytes each, at 0x400 + i * 0x200) */
    uint8_t  fs_header[4][0x200];
} nx_nca;

/* Open (decrypt + parse) an NCA. `header_key` is 32 bytes (AES-256-XTS).
 * `key_area_key` is the 16-byte AES-128 key for the key area (from prod.keys
 * or the ticket). `titlekek` is the 16-byte title-key decryption key.
 * All may be zero-filled if the NCA doesn't use those key paths.
 * Returns NX_CNT_OK or an error code. */
int nx_nca_open(const uint8_t *data, uint64_t data_len,
                const uint8_t header_key[32],
                const uint8_t key_area_key[16],
                const uint8_t titlekek[16],
                nx_nca *out);

/* ------------------------------------------------------------------ */
/* NCA section FS header                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  fs_type;       /* byte 2: 0 = PFS0, 1 = RomFS */
    uint8_t  hash_type;     /* byte 3: 0 = Auto, 1 = SHA256, 2 = IVFC */
    uint8_t  encryption_type; /* byte 4: 0 = None, 1 = AES-CTR */
    uint8_t  is_patch;      /* 1 if relocation tree size != 0 */
    uint8_t  has_sparse;    /* 1 if sparse layer present */
    uint8_t  has_compression; /* 1 if compression info present */
    uint64_t upper_counter; /* bytes 0x140..0x147 as big-endian u64 */
    /* Compression info (at 0x178): table offset, table size */
    uint64_t comp_table_offset;
    uint64_t comp_table_size;
} nx_nca_fs_header;

/* Parse an FS header from the decrypted header bytes. */
void nx_nca_fs_header_parse(const uint8_t raw[0x200], nx_nca_fs_header *out);

/* ------------------------------------------------------------------ */
/* RomFS                                                              */
/*                                                                    */
/* RomFS entries are variable-sized: fixed fields followed by a u32    */
/* name length and the name bytes inline.  Pointers are byte offsets  */
/* into the table.  See nx_container.c for the exact layout.          */
/* ------------------------------------------------------------------ */

typedef struct {
    int       valid;
    uint64_t  dir_meta_off;
    uint64_t  dir_meta_size;
    uint64_t  file_meta_off;
    uint64_t  file_meta_size;
    uint64_t  data_off;
} nx_romfs;

/* Open a RomFS from its raw (decrypted, decompressed) data.
 * `data` must point to the start of the RomFS image. */
int nx_romfs_open(const uint8_t *data, uint64_t data_len, nx_romfs *out);

/* Read a file from a RomFS by path (e.g. "/path/to/file").
 * Returns the file data (malloc'd, caller frees) and its size, or error. */
int nx_romfs_read_file(const nx_romfs *rfs, const uint8_t *data, uint64_t data_len,
                       const char *path, uint8_t **out_data, uint64_t *out_size);

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Free a container structure allocated by the parsers. */
void nx_container_free(nx_pfs0 *pfs);

/* Read a little-endian u16/u32/u64 from a byte buffer. */
uint16_t nx_rd_u16le(const uint8_t *p);
uint32_t nx_rd_u32le(const uint8_t *p);
uint64_t nx_rd_u64le(const uint8_t *p);

/* Write a little-endian u16/u32/u64. */
void nx_wr_u16le(uint8_t *p, uint16_t v);
void nx_wr_u32le(uint8_t *p, uint32_t v);
void nx_wr_u64le(uint8_t *p, uint64_t v);

#endif /* NX_CONTAINER_H */
