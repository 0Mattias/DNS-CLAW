/*
 * crypto.h — Payload-level AES-256-GCM encryption for DNS tunnel.
 *
 * Encrypts/decrypts payloads using a pre-shared key (PSK) so that
 * DNS queries and TXT responses contain only random noise, defeating
 * deep packet inspection on captive portals and network monitoring.
 *
 * Wire format of encrypted payload:
 *   [0xCE][0x01][12-byte nonce][ciphertext][16-byte GCM auth tag]
 *
 * Key derivation:
 *   HKDF-SHA256(salt="dns-claw-v1", ikm=PSK) → 32-byte AES key
 */
#ifndef DNS_CLAW_CRYPTO_H
#define DNS_CLAW_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

/* Magic header identifying encrypted payloads */
#define CRYPTO_MAGIC_0 0xCE
#define CRYPTO_MAGIC_1 0x01

/* Overhead: 2 (magic) + 12 (nonce) + 16 (tag) = 30 bytes */
#define CRYPTO_OVERHEAD  30
#define CRYPTO_NONCE_LEN 12
#define CRYPTO_TAG_LEN   16

/*
 * Initialize the tunnel encryption from a PSK string.
 * Derives a 32-byte AES-256 key via HKDF-SHA256.
 * Pass NULL or empty string to disable encryption.
 * Returns 0 on success, -1 on error.
 */
int tunnel_crypto_init(const char *psk);

/*
 * Returns 1 if encryption is enabled (PSK was set), 0 otherwise.
 */
int tunnel_crypto_enabled(void);

/*
 * Encrypt `in_len` bytes of plaintext `in` into `out`.
 * `out` must have room for `in_len + CRYPTO_OVERHEAD` bytes.
 * On success, sets `*out_len` and returns 0.
 * Returns -1 on error.
 */
int tunnel_encrypt(const uint8_t *in, size_t in_len, uint8_t *out, size_t *out_len);

/*
 * Decrypt `in_len` bytes of ciphertext `in` into `out`.
 * `out` must have room for `in_len` bytes (actual output is smaller).
 * Verifies the magic header and GCM authentication tag.
 * On success, sets `*out_len` and returns 0.
 * Returns -1 on error (wrong key, tampered data, or not encrypted).
 */
int tunnel_decrypt(const uint8_t *in, size_t in_len, uint8_t *out, size_t *out_len);

#endif /* DNS_CLAW_CRYPTO_H */
