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
    printf("  ALL PASSED\n");
    return 0;
}
