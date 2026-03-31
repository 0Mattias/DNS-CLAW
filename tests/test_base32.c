/*
 * test_base32.c — Unit tests for Base32 codec (no-padding variant).
 */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "base32.h"

#define PASS(name) printf("  PASS  %s\n", name)

static void test_encode_basic(void)
{
    char buf[64];
    int n = base32_encode((const uint8_t *)"Hello", 5, buf, sizeof(buf));
    assert(n > 0);
    assert(strcmp(buf, "JBSWY3DP") == 0);
    PASS("encode_basic");
}

static void test_decode_basic(void)
{
    uint8_t buf[64];
    int n = base32_decode("JBSWY3DP", buf, sizeof(buf));
    assert(n == 5);
    assert(memcmp(buf, "Hello", 5) == 0);
    PASS("decode_basic");
}

static void test_decode_case_insensitive(void)
{
    uint8_t buf[64];
    int n = base32_decode("jbswy3dp", buf, sizeof(buf));
    assert(n == 5);
    assert(memcmp(buf, "Hello", 5) == 0);
    PASS("decode_case_insensitive");
}

static void test_roundtrip(void)
{
    const uint8_t data[] = {0x00, 0xFF, 0x01, 0xFE, 0x02, 0xFD, 0x03};
    for (size_t len = 0; len <= sizeof(data); len++) {
        char enc[64];
        uint8_t dec[64];
        int elen = base32_encode(data, len, enc, sizeof(enc));
        if (len == 0) {
            assert(elen == 0);
            continue;
        }
        assert(elen > 0);
        int dlen = base32_decode(enc, dec, sizeof(dec));
        assert((size_t)dlen == len);
        assert(memcmp(dec, data, len) == 0);
    }
    PASS("roundtrip");
}

static void test_encoded_len(void)
{
    assert(base32_encoded_len(0) == 0);
    assert(base32_encoded_len(1) == 2);
    assert(base32_encoded_len(2) == 4);
    assert(base32_encoded_len(5) == 8);
    assert(base32_encoded_len(10) == 16);
    PASS("encoded_len");
}

static void test_buffer_too_small(void)
{
    char buf[4]; /* too small */
    int rc = base32_encode((const uint8_t *)"Hello", 5, buf, sizeof(buf));
    assert(rc == -1);
    PASS("buffer_too_small");
}

int main(void)
{
    printf("base32:\n");
    test_encode_basic();
    test_decode_basic();
    test_decode_case_insensitive();
    test_roundtrip();
    test_encoded_len();
    test_buffer_too_small();
    printf("  ALL PASSED\n");
    return 0;
}
