// LZ4 block decompressor for the on-device NSP/XCI/NCA/RomFS extractor.
// Implements the LZ4 block format (https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md).
// Self-contained, no external dependencies. Used to decompress NCA section
// data that uses LZ4 compression (compression kind 2 in the BKTR bucket tree).
#include "lz4.h"

// LZ4 block format:
//   - Repeated sequence: token (1 byte) + literals + match descriptor
//   - Token: upper nibble = literal length code (LLC), lower nibble = match length code (MLC)
//   - Literal length: if LLC == 15, read extra bytes (255 = continue, <255 = final)
//   - Match length: MLC + 4. If MLC == 15, read extra bytes (255 = continue, <255 = final)
//   - Offset: if MLC == 0, offset = 1 (implicit). If 1<=MLC<=14, 1 byte (1-255).
//     If MLC == 15, 2 bytes big-endian (1-65535).
//   - Last 5 bytes are always literals; no token follows the final literal run.

long lz4_decompress_block(const uint8_t *src, size_t src_len,
                          uint8_t *dst, size_t dst_cap) {
    if (!src || !dst || src_len == 0) return -1;

    const uint8_t *ip = src;
    const uint8_t *src_end = src + src_len;
    uint8_t *op = dst;
    uint8_t *dst_end = dst + dst_cap;

    while (ip < src_end) {
        uint8_t token = *ip++;

        // --- Literal length ---
        uint32_t ll = token >> 4;
        if (ll == 15) {
            do {
                if (ip >= src_end) return -2;  // truncated literal length
                ll += *ip++;
            } while (*(ip - 1) == 255);
        }

        // Copy literals
        if ((size_t)(src_end - ip) < ll) return -2;  // truncated literals
        if ((size_t)(dst_end - op) < ll) return -3;  // output overflow
        for (uint32_t i = 0; i < ll; i++)
            *op++ = *ip++;

        // If we've consumed all input, the literals are the last run.
        if (ip >= src_end) break;

        // --- Match descriptor ---
        uint8_t mlc = token & 0x0F;
        uint32_t ml;       // match length
        uint32_t offset;   // copy offset

        if (mlc == 0) {
            offset = 1;
            ml = 4;
        } else {
            // Read offset first (1 byte for MLC 1..14, 2 bytes LE for MLC 15)
            if (mlc == 15) {
                if ((size_t)(src_end - ip) < 2) return -2;
                offset = ((uint32_t)ip[1] << 8) | ip[0];  // little-endian
                ip += 2;
            } else {
                if (ip >= src_end) return -2;
                offset = *ip++;
            }
            // Then match length
            ml = mlc + 4;
            if (mlc == 15) {
                do {
                    if (ip >= src_end) return -2;
                    ml += *ip++;
                } while (*(ip - 1) == 255);
            }
        }

        // Validate offset
        if (offset == 0 || offset > (uint32_t)(op - dst)) return -4;

        // Copy match (byte-by-byte for overlap support)
        if ((size_t)(dst_end - op) < ml) return -3;
        for (uint32_t i = 0; i < ml; i++) {
            *op = *(op - offset);
            op++;
        }
    }

    return (long)(op - dst);
}
