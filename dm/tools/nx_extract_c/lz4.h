// LZ4 block decompressor for the on-device NSP/XCI/NCA/RomFS extractor.
// Implements the LZ4 block format (https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md).
// Self-contained, no external dependencies. Used to decompress NCA section
// data that uses LZ4 compression (compression kind 2 in the BKTR bucket tree).
#ifndef LZ4_H
#define LZ4_H

#include <stddef.h>
#include <stdint.h>

// Decompress a single LZ4 block from `src` (size `src_len`) into `dst`
// (capacity `dst_cap`). Returns the number of bytes written on success,
// or a negative value on error:
//   -1  invalid input (null pointer, zero length)
//   -2  source buffer too small (truncated token/length)
//   -3  destination buffer too small (output would overflow)
//   -4  invalid offset (offset == 0 or beyond current output)
//   -5  malformed block (last literal run exceeds source)
long lz4_decompress_block(const uint8_t *src, size_t src_len,
                          uint8_t *dst, size_t dst_cap);

#endif // LZ4_H
