/*
 * base32.h — RFC 4648 Base32 encoding/decoding (no padding variant).
 *
 * Mirrors Go's base32.StdEncoding.WithPadding(base32.NoPadding).
 * Alphabet: A-Z 2-7
 */
#ifndef BASE32_H
#define BASE32_H

#include <stddef.h>
#include <stdint.h>

/*
 * Encode `src_len` bytes from `src` into Base32 (no padding).
 * Writes at most `dst_len` bytes to `dst` (including NUL terminator).
 * Returns the number of characters written (excluding NUL), or -1 on error.
 */
int base32_encode(const uint8_t *src, size_t src_len,
                  char *dst, size_t dst_len);

/*
 * Decode a NUL-terminated Base32 string `src` (case-insensitive, no padding).
 * Writes at most `dst_len` bytes to `dst`.
 * Returns the number of bytes decoded, or -1 on error.
 */
int base32_decode(const char *src, uint8_t *dst, size_t dst_len);

/*
 * Returns the encoded length (without NUL) for `n` input bytes (no padding).
 */
size_t base32_encoded_len(size_t n);

/*
 * Returns the maximum decoded length for `n` Base32 characters.
 */
size_t base32_decoded_len(size_t n);

#endif /* BASE32_H */
