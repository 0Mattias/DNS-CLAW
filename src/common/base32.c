/*
 * base32.c — RFC 4648 Base32 encoding/decoding (no padding).
 */
#include "base32.h"
#include <string.h>
#include <ctype.h>

static const char B32_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

static int b32_val(char c)
{
    c = (char)toupper((unsigned char)c);
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= '2' && c <= '7')
        return c - '2' + 26;
    return -1;
}

size_t base32_encoded_len(size_t n)
{
    /* Each 5 bytes → 8 chars. Without padding, the last group is partial. */
    return (n * 8 + 4) / 5;
}

size_t base32_decoded_len(size_t n)
{
    return (n * 5) / 8;
}

int base32_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_len)
{
    size_t needed = base32_encoded_len(src_len) + 1; /* +1 for NUL */
    if (dst_len < needed)
        return -1;

    size_t di = 0;
    size_t si = 0;

    /* Process full 5-byte blocks */
    while (si + 5 <= src_len) {
        uint64_t block = ((uint64_t)src[si] << 32) | ((uint64_t)src[si + 1] << 24) |
                         ((uint64_t)src[si + 2] << 16) | ((uint64_t)src[si + 3] << 8) |
                         ((uint64_t)src[si + 4]);

        dst[di++] = B32_ALPHABET[(block >> 35) & 0x1F];
        dst[di++] = B32_ALPHABET[(block >> 30) & 0x1F];
        dst[di++] = B32_ALPHABET[(block >> 25) & 0x1F];
        dst[di++] = B32_ALPHABET[(block >> 20) & 0x1F];
        dst[di++] = B32_ALPHABET[(block >> 15) & 0x1F];
        dst[di++] = B32_ALPHABET[(block >> 10) & 0x1F];
        dst[di++] = B32_ALPHABET[(block >> 5) & 0x1F];
        dst[di++] = B32_ALPHABET[(block) & 0x1F];

        si += 5;
    }

    /* Handle remaining bytes (no padding) */
    size_t remaining = src_len - si;
    if (remaining > 0) {
        uint64_t block = 0;
        for (size_t i = 0; i < remaining; i++)
            block |= (uint64_t)src[si + i] << (32 - i * 8);

        size_t out_chars;
        switch (remaining) {
        case 1:
            out_chars = 2;
            break;
        case 2:
            out_chars = 4;
            break;
        case 3:
            out_chars = 5;
            break;
        case 4:
            out_chars = 7;
            break;
        default:
            out_chars = 0;
            break;
        }
        for (size_t i = 0; i < out_chars; i++)
            dst[di++] = B32_ALPHABET[(block >> (35 - i * 5)) & 0x1F];
    }

    dst[di] = '\0';
    return (int)di;
}

int base32_decode(const char *src, uint8_t *dst, size_t dst_len)
{
    size_t slen = strlen(src);
    size_t max_out = base32_decoded_len(slen);
    if (dst_len < max_out)
        return -1;

    size_t di = 0;
    size_t si = 0;

    /* Process full 8-char blocks */
    while (si + 8 <= slen) {
        uint64_t block = 0;
        for (int i = 0; i < 8; i++) {
            int v = b32_val(src[si + i]);
            if (v < 0)
                return -1;
            block = (block << 5) | (uint64_t)v;
        }
        dst[di++] = (uint8_t)(block >> 32);
        dst[di++] = (uint8_t)(block >> 24);
        dst[di++] = (uint8_t)(block >> 16);
        dst[di++] = (uint8_t)(block >> 8);
        dst[di++] = (uint8_t)(block);
        si += 8;
    }

    /* Handle remaining chars */
    size_t remaining = slen - si;
    if (remaining > 0) {
        uint64_t block = 0;
        for (size_t i = 0; i < remaining; i++) {
            int v = b32_val(src[si + i]);
            if (v < 0)
                return -1;
            block = (block << 5) | (uint64_t)v;
        }
        /* Left-align the bits */
        block <<= (8 - remaining) * 5;

        size_t out_bytes;
        switch (remaining) {
        case 2:
            out_bytes = 1;
            break;
        case 4:
            out_bytes = 2;
            break;
        case 5:
            out_bytes = 3;
            break;
        case 7:
            out_bytes = 4;
            break;
        default:
            return -1; /* invalid length */
        }
        for (size_t i = 0; i < out_bytes; i++)
            dst[di++] = (uint8_t)(block >> (32 - i * 8));
    }

    return (int)di;
}
