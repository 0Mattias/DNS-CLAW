/*
 * test_base64.c — Unit tests for Base64 codec.
 */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "base64.h"

#define PASS(name) printf("  PASS  %s\n", name)

static void test_encode_basic(void)
{
    char buf[64];
    base64_encode((const uint8_t *)"Hello", 5, buf, sizeof(buf));
    assert(strcmp(buf, "SGVsbG8=") == 0);
    PASS("encode_basic");
}

static void test_decode_basic(void)
{
    uint8_t buf[64];
    int n = base64_decode("SGVsbG8=", buf, sizeof(buf));
    assert(n == 5);
    assert(memcmp(buf, "Hello", 5) == 0);
    PASS("decode_basic");
}

static void test_roundtrip(void)
{
    const char *inputs[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    for (int i = 0; i < 7; i++) {
        size_t len = strlen(inputs[i]);
        char enc[64];
        uint8_t dec[64];
        base64_encode((const uint8_t *)inputs[i], len, enc, sizeof(enc));
        int n = base64_decode(enc, dec, sizeof(dec));
        assert((size_t)n == len);
        assert(memcmp(dec, inputs[i], len) == 0);
    }
    PASS("roundtrip");
}

static void test_empty_decode(void)
{
    uint8_t buf[64];
    int n = base64_decode("", buf, sizeof(buf));
    assert(n == 0);
    PASS("empty_decode");
}

static void test_null_src_decode(void)
{
    uint8_t buf[64];
    /* NULL input should return 0, not crash */
    int n = base64_decode(NULL, buf, sizeof(buf));
    assert(n == 0);
    PASS("null_src_decode");
}

static void test_rfc4648_vectors(void)
{
    /* RFC 4648 test vectors */
    struct { const char *plain; const char *encoded; } vecs[] = {
        {"",       ""},
        {"f",      "Zg=="},
        {"fo",     "Zm8="},
        {"foo",    "Zm9v"},
        {"foob",   "Zm9vYg=="},
        {"fooba",  "Zm9vYmE="},
        {"foobar", "Zm9vYmFy"},
    };
    for (int i = 0; i < 7; i++) {
        char enc[64];
        size_t plen = strlen(vecs[i].plain);
        base64_encode((const uint8_t *)vecs[i].plain, plen, enc, sizeof(enc));
        assert(strcmp(enc, vecs[i].encoded) == 0);
    }
    PASS("rfc4648_vectors");
}

static void test_encoded_len(void)
{
    assert(base64_encoded_len(0) == 0);
    assert(base64_encoded_len(1) == 4);
    assert(base64_encoded_len(2) == 4);
    assert(base64_encoded_len(3) == 4);
    assert(base64_encoded_len(4) == 8);
    PASS("encoded_len");
}

static void test_buffer_too_small(void)
{
    char buf[4]; /* too small for "Hello" → "SGVsbG8=" (8 chars + NUL) */
    int rc = base64_encode((const uint8_t *)"Hello", 5, buf, sizeof(buf));
    assert(rc == -1);
    PASS("buffer_too_small");
}

int main(void)
{
    printf("base64:\n");
    test_encode_basic();
    test_decode_basic();
    test_roundtrip();
    test_empty_decode();
    test_null_src_decode();
    test_rfc4648_vectors();
    test_encoded_len();
    test_buffer_too_small();
    printf("  ALL PASSED\n");
    return 0;
}
