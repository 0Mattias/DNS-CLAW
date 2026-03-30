/*
 * base64.h — RFC 4648 Base64 encoding/decoding (standard alphabet, with padding).
 *
 * Mirrors Go's base64.StdEncoding.
 */
#ifndef BASE64_H
#define BASE64_H

#include <stddef.h>
#include <stdint.h>

int   base64_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_len);
int   base64_decode(const char *src, uint8_t *dst, size_t dst_len);
size_t base64_encoded_len(size_t n);
size_t base64_decoded_len(size_t n);

#endif /* BASE64_H */
