/*
 * base64.c — RFC 4648 Base64 (standard, with '=' padding).
 */
#include "base64.h"
#include <string.h>

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const int B64_DEC[256] = {
    ['A']=0,  ['B']=1,  ['C']=2,  ['D']=3,  ['E']=4,  ['F']=5,
    ['G']=6,  ['H']=7,  ['I']=8,  ['J']=9,  ['K']=10, ['L']=11,
    ['M']=12, ['N']=13, ['O']=14, ['P']=15, ['Q']=16, ['R']=17,
    ['S']=18, ['T']=19, ['U']=20, ['V']=21, ['W']=22, ['X']=23,
    ['Y']=24, ['Z']=25,
    ['a']=26, ['b']=27, ['c']=28, ['d']=29, ['e']=30, ['f']=31,
    ['g']=32, ['h']=33, ['i']=34, ['j']=35, ['k']=36, ['l']=37,
    ['m']=38, ['n']=39, ['o']=40, ['p']=41, ['q']=42, ['r']=43,
    ['s']=44, ['t']=45, ['u']=46, ['v']=47, ['w']=48, ['x']=49,
    ['y']=50, ['z']=51,
    ['0']=52, ['1']=53, ['2']=54, ['3']=55, ['4']=56, ['5']=57,
    ['6']=58, ['7']=59, ['8']=60, ['9']=61,
    ['+']=62, ['/']=63,
};

size_t base64_encoded_len(size_t n)
{
    return ((n + 2) / 3) * 4;
}

size_t base64_decoded_len(size_t n)
{
    return (n / 4) * 3;
}

int base64_encode(const uint8_t *src, size_t src_len,
                  char *dst, size_t dst_len)
{
    size_t needed = base64_encoded_len(src_len) + 1;
    if (dst_len < needed) return -1;

    size_t di = 0;
    size_t si = 0;

    while (si + 3 <= src_len) {
        uint32_t n = ((uint32_t)src[si] << 16) |
                     ((uint32_t)src[si+1] << 8) |
                     ((uint32_t)src[si+2]);
        dst[di++] = B64[(n >> 18) & 0x3F];
        dst[di++] = B64[(n >> 12) & 0x3F];
        dst[di++] = B64[(n >>  6) & 0x3F];
        dst[di++] = B64[(n      ) & 0x3F];
        si += 3;
    }

    size_t rem = src_len - si;
    if (rem == 1) {
        uint32_t n = (uint32_t)src[si] << 16;
        dst[di++] = B64[(n >> 18) & 0x3F];
        dst[di++] = B64[(n >> 12) & 0x3F];
        dst[di++] = '=';
        dst[di++] = '=';
    } else if (rem == 2) {
        uint32_t n = ((uint32_t)src[si] << 16) | ((uint32_t)src[si+1] << 8);
        dst[di++] = B64[(n >> 18) & 0x3F];
        dst[di++] = B64[(n >> 12) & 0x3F];
        dst[di++] = B64[(n >>  6) & 0x3F];
        dst[di++] = '=';
    }

    dst[di] = '\0';
    return (int)di;
}

int base64_decode(const char *src, uint8_t *dst, size_t dst_len)
{
    if (!src || !src[0]) return 0;  /* empty input → 0 bytes decoded */
    size_t slen = strlen(src);
    /* Strip trailing '=' */
    while (slen > 0 && src[slen - 1] == '=') slen--;

    size_t max_out = (slen * 3) / 4;
    if (dst_len < max_out) return -1;

    size_t di = 0;
    size_t si = 0;

    while (si + 4 <= slen) {
        uint32_t n = ((uint32_t)B64_DEC[(unsigned char)src[si]]   << 18) |
                     ((uint32_t)B64_DEC[(unsigned char)src[si+1]] << 12) |
                     ((uint32_t)B64_DEC[(unsigned char)src[si+2]] <<  6) |
                     ((uint32_t)B64_DEC[(unsigned char)src[si+3]]);
        dst[di++] = (uint8_t)(n >> 16);
        dst[di++] = (uint8_t)(n >>  8);
        dst[di++] = (uint8_t)(n);
        si += 4;
    }

    size_t rem = slen - si;
    if (rem == 2) {
        uint32_t n = ((uint32_t)B64_DEC[(unsigned char)src[si]]   << 18) |
                     ((uint32_t)B64_DEC[(unsigned char)src[si+1]] << 12);
        dst[di++] = (uint8_t)(n >> 16);
    } else if (rem == 3) {
        uint32_t n = ((uint32_t)B64_DEC[(unsigned char)src[si]]   << 18) |
                     ((uint32_t)B64_DEC[(unsigned char)src[si+1]] << 12) |
                     ((uint32_t)B64_DEC[(unsigned char)src[si+2]] <<  6);
        dst[di++] = (uint8_t)(n >> 16);
        dst[di++] = (uint8_t)(n >>  8);
    }

    return (int)di;
}
