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

static void test_invalid_chars_rejected(void)
{
    uint8_t buf[64];
    /* Characters outside A-Z 2-7 must be rejected */
    assert(base32_decode("0000", buf, sizeof(buf)) == -1); /* '0' invalid */
    assert(base32_decode("1111", buf, sizeof(buf)) == -1); /* '1' invalid */
    assert(base32_decode("8888", buf, sizeof(buf)) == -1); /* '8' invalid */
    assert(base32_decode("!@#$", buf, sizeof(buf)) == -1);
    assert(base32_decode("AB\x01D", buf, sizeof(buf)) == -1);
    PASS("invalid_chars_rejected");
}

static void test_decode_dst_too_small(void)
{
    uint8_t buf[2];
    /* "JBSWY3DP" decodes to "Hello" (5 bytes), 2-byte buffer should fail */
    assert(base32_decode("JBSWY3DP", buf, sizeof(buf)) == -1);
    PASS("decode_dst_too_small");
}

static void test_binary_roundtrip(void)
{
    /* All byte values 0x00-0xFF */
    uint8_t data[256];
    for (int i = 0; i < 256; i++)
        data[i] = (uint8_t)i;

    char enc[512];
    assert(base32_encode(data, sizeof(data), enc, sizeof(enc)) > 0);

    uint8_t dec[256];
    int n = base32_decode(enc, dec, sizeof(dec));
    assert(n == 256);
    assert(memcmp(dec, data, 256) == 0);
    PASS("binary_roundtrip");
}

static void test_single_byte_values(void)
{
    /* Roundtrip each single byte individually */
    for (int i = 0; i < 256; i++) {
        uint8_t byte = (uint8_t)i;
        char enc[16];
        uint8_t dec[16];
        int elen = base32_encode(&byte, 1, enc, sizeof(enc));
        assert(elen > 0);
        int dlen = base32_decode(enc, dec, sizeof(dec));
        assert(dlen == 1);
        assert(dec[0] == byte);
    }
    PASS("single_byte_values");
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
    test_invalid_chars_rejected();
    test_decode_dst_too_small();
    test_binary_roundtrip();
    test_single_byte_values();
    printf("  ALL PASSED\n");
    return 0;
}
