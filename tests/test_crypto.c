/*
 * test_crypto.c — Unit tests for AES-256-GCM tunnel encryption.
 */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "crypto.h"

#define PASS(name) printf("  PASS  %s\n", name)

static void test_disabled_by_default(void)
{
    tunnel_crypto_init(NULL);
    assert(tunnel_crypto_enabled() == 0);
    PASS("disabled_by_default");
}

static void test_enabled_with_psk(void)
{
    assert(tunnel_crypto_init("test-key-1234") == 0);
    assert(tunnel_crypto_enabled() == 1);
    PASS("enabled_with_psk");
}

static void test_encrypt_decrypt_roundtrip(void)
{
    tunnel_crypto_init("roundtrip-key");

    const char *plaintext = "Hello, DNS tunnel!";
    size_t pt_len = strlen(plaintext);
    uint8_t ciphertext[256];
    size_t ct_len = 0;

    assert(tunnel_encrypt((const uint8_t *)plaintext, pt_len,
                          ciphertext, &ct_len) == 0);
    assert(ct_len == pt_len + CRYPTO_OVERHEAD);

    /* Verify magic header */
    assert(ciphertext[0] == CRYPTO_MAGIC_0);
    assert(ciphertext[1] == CRYPTO_MAGIC_1);

    /* Decrypt */
    uint8_t decrypted[256];
    size_t dec_len = 0;
    assert(tunnel_decrypt(ciphertext, ct_len, decrypted, &dec_len) == 0);
    assert(dec_len == pt_len);
    assert(memcmp(decrypted, plaintext, pt_len) == 0);
    PASS("encrypt_decrypt_roundtrip");
}

static void test_wrong_key_fails(void)
{
    tunnel_crypto_init("key-A");
    const char *msg = "secret";
    uint8_t ct[128];
    size_t ct_len = 0;
    assert(tunnel_encrypt((const uint8_t *)msg, 6, ct, &ct_len) == 0);

    /* Switch to different key */
    tunnel_crypto_init("key-B");
    uint8_t pt[128];
    size_t pt_len = 0;
    assert(tunnel_decrypt(ct, ct_len, pt, &pt_len) != 0);
    PASS("wrong_key_fails");
}

static void test_tampered_data_fails(void)
{
    tunnel_crypto_init("tamper-key");
    const char *msg = "integrity check";
    uint8_t ct[256];
    size_t ct_len = 0;
    assert(tunnel_encrypt((const uint8_t *)msg, strlen(msg), ct, &ct_len) == 0);

    /* Flip a bit in the ciphertext */
    ct[CRYPTO_OVERHEAD / 2] ^= 0x01;
    uint8_t pt[256];
    size_t pt_len = 0;
    assert(tunnel_decrypt(ct, ct_len, pt, &pt_len) != 0);
    PASS("tampered_data_fails");
}

static void test_too_short_fails(void)
{
    tunnel_crypto_init("short-key");
    uint8_t buf[10] = {CRYPTO_MAGIC_0, CRYPTO_MAGIC_1};
    uint8_t pt[64];
    size_t pt_len = 0;
    assert(tunnel_decrypt(buf, 10, pt, &pt_len) != 0);
    PASS("too_short_fails");
}

static void test_empty_psk_disables(void)
{
    tunnel_crypto_init("some-key");
    assert(tunnel_crypto_enabled() == 1);
    tunnel_crypto_init("");
    assert(tunnel_crypto_enabled() == 0);
    PASS("empty_psk_disables");
}

static void test_unique_nonces(void)
{
    tunnel_crypto_init("nonce-key");
    const char *msg = "test";
    uint8_t ct1[128], ct2[128];
    size_t len1, len2;
    assert(tunnel_encrypt((const uint8_t *)msg, 4, ct1, &len1) == 0);
    assert(tunnel_encrypt((const uint8_t *)msg, 4, ct2, &len2) == 0);
    /* Same plaintext should produce different ciphertexts (different nonces) */
    assert(memcmp(ct1, ct2, len1) != 0);
    PASS("unique_nonces");
}

static void test_encrypt_when_disabled_fails(void)
{
    tunnel_crypto_init(NULL);
    assert(tunnel_crypto_enabled() == 0);
    uint8_t ct[128];
    size_t ct_len = 0;
    assert(tunnel_encrypt((const uint8_t *)"test", 4, ct, &ct_len) == -1);
    PASS("encrypt_when_disabled_fails");
}

static void test_decrypt_when_disabled_fails(void)
{
    tunnel_crypto_init(NULL);
    uint8_t pt[128];
    size_t pt_len = 0;
    uint8_t fake[64] = {CRYPTO_MAGIC_0, CRYPTO_MAGIC_1};
    assert(tunnel_decrypt(fake, sizeof(fake), pt, &pt_len) == -1);
    PASS("decrypt_when_disabled_fails");
}

static void test_bad_magic_rejected(void)
{
    tunnel_crypto_init("magic-key");
    const char *msg = "test";
    uint8_t ct[128];
    size_t ct_len = 0;
    assert(tunnel_encrypt((const uint8_t *)msg, 4, ct, &ct_len) == 0);

    /* Corrupt magic header */
    ct[0] = 0xFF;
    ct[1] = 0xFF;
    uint8_t pt[128];
    size_t pt_len = 0;
    assert(tunnel_decrypt(ct, ct_len, pt, &pt_len) == -1);
    PASS("bad_magic_rejected");
}

static void test_empty_plaintext_roundtrip(void)
{
    tunnel_crypto_init("empty-key");
    uint8_t ct[128];
    size_t ct_len = 0;
    assert(tunnel_encrypt((const uint8_t *)"", 0, ct, &ct_len) == 0);
    assert(ct_len == CRYPTO_OVERHEAD);

    uint8_t pt[128];
    size_t pt_len = 99;
    assert(tunnel_decrypt(ct, ct_len, pt, &pt_len) == 0);
    assert(pt_len == 0);
    PASS("empty_plaintext_roundtrip");
}

static void test_large_payload_roundtrip(void)
{
    tunnel_crypto_init("large-key");
    /* 4KB payload */
    uint8_t data[4096];
    for (int i = 0; i < (int)sizeof(data); i++) data[i] = (uint8_t)(i & 0xFF);

    uint8_t ct[4096 + CRYPTO_OVERHEAD + 16];
    size_t ct_len = 0;
    assert(tunnel_encrypt(data, sizeof(data), ct, &ct_len) == 0);

    uint8_t pt[4096];
    size_t pt_len = 0;
    assert(tunnel_decrypt(ct, ct_len, pt, &pt_len) == 0);
    assert(pt_len == sizeof(data));
    assert(memcmp(pt, data, sizeof(data)) == 0);
    PASS("large_payload_roundtrip");
}

static void test_tampered_tag_fails(void)
{
    tunnel_crypto_init("tag-key");
    const char *msg = "authenticate me";
    uint8_t ct[128];
    size_t ct_len = 0;
    assert(tunnel_encrypt((const uint8_t *)msg, strlen(msg), ct, &ct_len) == 0);

    /* Flip a bit in the GCM auth tag (last 16 bytes) */
    ct[ct_len - 1] ^= 0x01;
    uint8_t pt[128];
    size_t pt_len = 0;
    assert(tunnel_decrypt(ct, ct_len, pt, &pt_len) != 0);
    PASS("tampered_tag_fails");
}

int main(void)
{
    printf("crypto:\n");
    test_disabled_by_default();
    test_enabled_with_psk();
    test_encrypt_decrypt_roundtrip();
    test_wrong_key_fails();
    test_tampered_data_fails();
    test_too_short_fails();
    test_empty_psk_disables();
    test_unique_nonces();
    test_encrypt_when_disabled_fails();
    test_decrypt_when_disabled_fails();
    test_bad_magic_rejected();
    test_empty_plaintext_roundtrip();
    test_large_payload_roundtrip();
    test_tampered_tag_fails();
    printf("  ALL PASSED\n");
    return 0;
}
