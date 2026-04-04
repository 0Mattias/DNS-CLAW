/**
 * crypto.test.ts — Unit tests for AES-256-GCM tunnel encryption.
 *
 * Mirrors tests/test_crypto.c test vectors and edge cases.
 */

import { describe, it, expect } from "vitest";
import { deriveKey, encrypt, decrypt, CRYPTO_OVERHEAD } from "../crypto";

describe("crypto", () => {
  it("deriveKey returns null for undefined PSK", () => {
    expect(deriveKey(undefined)).toBeNull();
  });

  it("deriveKey returns null for empty PSK", () => {
    expect(deriveKey("")).toBeNull();
  });

  it("deriveKey returns 32-byte key for valid PSK", () => {
    const key = deriveKey("test-key-1234");
    expect(key).not.toBeNull();
    expect(key!.length).toBe(32);
  });

  it("encrypt/decrypt roundtrip", () => {
    const key = deriveKey("roundtrip-key")!;
    const plaintext = Buffer.from("Hello, DNS tunnel!");
    const ciphertext = encrypt(plaintext, key);
    const decrypted = decrypt(ciphertext, key);
    expect(decrypted).toEqual(plaintext);
  });

  it("ciphertext has magic header 0xCE 0x01", () => {
    const key = deriveKey("magic-key")!;
    const ct = encrypt(Buffer.from("test"), key);
    expect(ct[0]).toBe(0xce);
    expect(ct[1]).toBe(0x01);
  });

  it("ciphertext length = plaintext + CRYPTO_OVERHEAD", () => {
    const key = deriveKey("len-key")!;
    const plaintext = Buffer.from("Hello, DNS tunnel!");
    const ct = encrypt(plaintext, key);
    expect(ct.length).toBe(plaintext.length + CRYPTO_OVERHEAD);
  });

  it("wrong key fails to decrypt", () => {
    const keyA = deriveKey("key-A")!;
    const keyB = deriveKey("key-B")!;
    const ct = encrypt(Buffer.from("secret"), keyA);
    expect(() => decrypt(ct, keyB)).toThrow();
  });

  it("tampered ciphertext fails", () => {
    const key = deriveKey("tamper-key")!;
    const ct = encrypt(Buffer.from("integrity check"), key);
    ct[Math.floor(CRYPTO_OVERHEAD / 2)] ^= 0x01;
    expect(() => decrypt(ct, key)).toThrow();
  });

  it("tampered GCM tag fails", () => {
    const key = deriveKey("tag-key")!;
    const ct = encrypt(Buffer.from("authenticate me"), key);
    ct[ct.length - 1] ^= 0x01;
    expect(() => decrypt(ct, key)).toThrow();
  });

  it("too-short data throws", () => {
    const key = deriveKey("short-key")!;
    const buf = Buffer.from([0xce, 0x01, 0, 0, 0, 0, 0, 0, 0, 0]);
    expect(() => decrypt(buf, key)).toThrow();
  });

  it("bad magic bytes throw", () => {
    const key = deriveKey("magic-key")!;
    const ct = encrypt(Buffer.from("test"), key);
    ct[0] = 0xff;
    ct[1] = 0xff;
    expect(() => decrypt(ct, key)).toThrow(/magic/i);
  });

  it("same plaintext produces different ciphertexts (unique nonces)", () => {
    const key = deriveKey("nonce-key")!;
    const msg = Buffer.from("test");
    const ct1 = encrypt(msg, key);
    const ct2 = encrypt(msg, key);
    expect(ct1.equals(ct2)).toBe(false);
  });

  it("empty plaintext roundtrip", () => {
    const key = deriveKey("empty-key")!;
    const ct = encrypt(Buffer.alloc(0), key);
    expect(ct.length).toBe(CRYPTO_OVERHEAD);
    const pt = decrypt(ct, key);
    expect(pt.length).toBe(0);
  });

  it("large payload (4KB) roundtrip", () => {
    const key = deriveKey("large-key")!;
    const data = Buffer.alloc(4096);
    for (let i = 0; i < 4096; i++) data[i] = i & 0xff;
    const ct = encrypt(data, key);
    const pt = decrypt(ct, key);
    expect(pt).toEqual(data);
  });
});
