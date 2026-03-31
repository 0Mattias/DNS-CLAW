/*
 * crypto.c — AES-256-GCM payload encryption for DNS tunnel.
 *
 * Uses OpenSSL EVP API. Key derived from PSK via HKDF-SHA256.
 * All encryption uses random nonces (safe for up to ~2^48 messages).
 */
#include "crypto.h"

#include <string.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

/* ── State ────────────────────────────────────────────────────────────────── */

static uint8_t g_key[32];      /* AES-256 key derived from PSK */
static int     g_enabled = 0;  /* 1 if PSK was set */

/* ── HKDF-SHA256 (RFC 5869) ──────────────────────────────────────────────── */

/*
 * HKDF-Extract: PRK = HMAC-SHA256(salt, ikm)
 * HKDF-Expand:  OKM = HMAC-SHA256(PRK, info || 0x01)  [first 32 bytes only]
 *
 * This produces exactly 32 bytes — perfect for AES-256.
 * Implemented manually for maximum OpenSSL version compatibility.
 */
static int hkdf_sha256(const uint8_t *salt, size_t salt_len,
                       const uint8_t *ikm,  size_t ikm_len,
                       const uint8_t *info, size_t info_len,
                       uint8_t *okm, size_t okm_len)
{
    if (okm_len > 32) return -1;  /* single HMAC output block */

    /* Extract: PRK = HMAC-SHA256(salt, IKM) */
    uint8_t prk[32];
    unsigned int prk_len = 32;
    if (!HMAC(EVP_sha256(), salt, (int)salt_len,
              ikm, ikm_len, prk, &prk_len))
        return -1;

    /* Expand: T(1) = HMAC-SHA256(PRK, info || 0x01) */
    uint8_t expand_input[256];
    if (info_len + 1 > sizeof(expand_input)) return -1;
    memcpy(expand_input, info, info_len);
    expand_input[info_len] = 0x01;

    uint8_t t1[32];
    unsigned int t1_len = 32;
    if (!HMAC(EVP_sha256(), prk, 32,
              expand_input, info_len + 1, t1, &t1_len))
        return -1;

    memcpy(okm, t1, okm_len);
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int tunnel_crypto_init(const char *psk)
{
    g_enabled = 0;
    memset(g_key, 0, sizeof(g_key));

    if (!psk || !psk[0])
        return 0;  /* No PSK = encryption disabled, not an error */

    static const uint8_t salt[] = "dns-claw-v1";
    static const uint8_t info[] = "tunnel-key";

    if (hkdf_sha256(salt, sizeof(salt) - 1,
                    (const uint8_t *)psk, strlen(psk),
                    info, sizeof(info) - 1,
                    g_key, sizeof(g_key)) < 0)
        return -1;

    g_enabled = 1;
    return 0;
}

int tunnel_crypto_enabled(void)
{
    return g_enabled;
}

int tunnel_encrypt(const uint8_t *in, size_t in_len,
                   uint8_t *out, size_t *out_len)
{
    if (!g_enabled) return -1;

    /* Layout: [magic:2][nonce:12][ciphertext:in_len][tag:16] */
    uint8_t nonce[CRYPTO_NONCE_LEN];
    if (RAND_bytes(nonce, CRYPTO_NONCE_LEN) != 1)
        return -1;

    /* Write magic header */
    out[0] = CRYPTO_MAGIC_0;
    out[1] = CRYPTO_MAGIC_1;

    /* Write nonce */
    memcpy(out + 2, nonce, CRYPTO_NONCE_LEN);

    /* Encrypt */
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int rc = -1;
    int len = 0;
    int ciphertext_len = 0;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
        goto cleanup;
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, g_key, nonce) != 1)
        goto cleanup;

    /* Encrypt plaintext → ciphertext at offset 14 (2 magic + 12 nonce) */
    if (EVP_EncryptUpdate(ctx, out + 2 + CRYPTO_NONCE_LEN, &len,
                          in, (int)in_len) != 1)
        goto cleanup;
    ciphertext_len = len;

    if (EVP_EncryptFinal_ex(ctx, out + 2 + CRYPTO_NONCE_LEN + ciphertext_len,
                            &len) != 1)
        goto cleanup;
    ciphertext_len += len;

    /* Append GCM tag */
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, CRYPTO_TAG_LEN,
                            out + 2 + CRYPTO_NONCE_LEN + ciphertext_len) != 1)
        goto cleanup;

    *out_len = 2 + CRYPTO_NONCE_LEN + (size_t)ciphertext_len + CRYPTO_TAG_LEN;
    rc = 0;

cleanup:
    EVP_CIPHER_CTX_free(ctx);
    return rc;
}

int tunnel_decrypt(const uint8_t *in, size_t in_len,
                   uint8_t *out, size_t *out_len)
{
    if (!g_enabled) return -1;

    /* Minimum size: 2 (magic) + 12 (nonce) + 0 (ciphertext) + 16 (tag) */
    if (in_len < CRYPTO_OVERHEAD) return -1;

    /* Verify magic header */
    if (in[0] != CRYPTO_MAGIC_0 || in[1] != CRYPTO_MAGIC_1)
        return -1;

    const uint8_t *nonce      = in + 2;
    const uint8_t *ciphertext = in + 2 + CRYPTO_NONCE_LEN;
    size_t ct_len = in_len - CRYPTO_OVERHEAD;
    const uint8_t *tag = in + in_len - CRYPTO_TAG_LEN;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int rc = -1;
    int len = 0;
    int plaintext_len = 0;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
        goto cleanup;
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, g_key, nonce) != 1)
        goto cleanup;

    /* Decrypt ciphertext → plaintext */
    if (EVP_DecryptUpdate(ctx, out, &len,
                          ciphertext, (int)ct_len) != 1)
        goto cleanup;
    plaintext_len = len;

    /* Set expected GCM tag for verification */
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, CRYPTO_TAG_LEN,
                            (void *)tag) != 1)
        goto cleanup;

    /* Finalize — this verifies the tag (returns 0 if tampered) */
    if (EVP_DecryptFinal_ex(ctx, out + plaintext_len, &len) != 1)
        goto cleanup;  /* Authentication failed — wrong key or tampered */
    plaintext_len += len;

    *out_len = (size_t)plaintext_len;
    rc = 0;

cleanup:
    EVP_CIPHER_CTX_free(ctx);
    return rc;
}
