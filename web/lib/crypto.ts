/**
 * crypto.ts — AES-256-GCM payload encryption for DNS tunnel.
 *
 * Mirrors src/common/crypto.c. Key derived from PSK via HKDF-SHA256.
 * Wire format: [0xCE, 0x01][nonce:12][ciphertext][tag:16]
 */

import { createHmac, createCipheriv, createDecipheriv, randomBytes } from "crypto";

const MAGIC_0 = 0xce;
const MAGIC_1 = 0x01;
const NONCE_LEN = 12;
const TAG_LEN = 16;
const OVERHEAD = 2 + NONCE_LEN + TAG_LEN; // 30 bytes

/**
 * HKDF-SHA256 (RFC 5869) — extract + expand to produce exactly 32 bytes.
 * Matches the C implementation in crypto.c.
 */
function hkdfSha256(
  salt: Buffer,
  ikm: Buffer,
  info: Buffer,
): Buffer {
  // Extract: PRK = HMAC-SHA256(salt, IKM)
  const prk = createHmac("sha256", salt).update(ikm).digest();

  // Expand: T(1) = HMAC-SHA256(PRK, info || 0x01)
  const expandInput = Buffer.concat([info, Buffer.from([0x01])]);
  const t1 = createHmac("sha256", prk).update(expandInput).digest();

  return t1; // 32 bytes — exactly what AES-256 needs
}

/** Derive AES-256 key from PSK. Returns null if no PSK. */
export function deriveKey(psk: string | undefined): Buffer | null {
  if (!psk || psk.length === 0) return null;

  return hkdfSha256(
    Buffer.from("dns-claw-v1", "utf-8"),
    Buffer.from(psk, "utf-8"),
    Buffer.from("tunnel-key", "utf-8"),
  );
}

/** Encrypt plaintext with AES-256-GCM. Returns [magic][nonce][ct][tag]. */
export function encrypt(plaintext: Buffer, key: Buffer): Buffer {
  const nonce = randomBytes(NONCE_LEN);
  const cipher = createCipheriv("aes-256-gcm", key, nonce);

  const ciphertext = Buffer.concat([cipher.update(plaintext), cipher.final()]);
  const tag = cipher.getAuthTag();

  return Buffer.concat([
    Buffer.from([MAGIC_0, MAGIC_1]),
    nonce,
    ciphertext,
    tag,
  ]);
}

/** Decrypt [magic][nonce][ct][tag] with AES-256-GCM. */
export function decrypt(data: Buffer, key: Buffer): Buffer {
  if (data.length < OVERHEAD) {
    throw new Error("Encrypted data too short");
  }
  if (data[0] !== MAGIC_0 || data[1] !== MAGIC_1) {
    throw new Error("Invalid magic bytes — not an encrypted DNS-CLAW payload");
  }

  const nonce = data.subarray(2, 2 + NONCE_LEN);
  const ciphertext = data.subarray(2 + NONCE_LEN, data.length - TAG_LEN);
  const tag = data.subarray(data.length - TAG_LEN);

  const decipher = createDecipheriv("aes-256-gcm", key, nonce);
  decipher.setAuthTag(tag);

  return Buffer.concat([decipher.update(ciphertext), decipher.final()]);
}

export { OVERHEAD as CRYPTO_OVERHEAD };
