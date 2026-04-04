/**
 * base32.test.ts — Unit tests for Base32 codec (no-padding variant).
 *
 * Mirrors tests/test_base32.c test vectors and edge cases.
 */

import { describe, it, expect } from "vitest";
import { encode, decode } from "../base32";

describe("base32", () => {
  it("encodes 'Hello' to JBSWY3DP", () => {
    expect(encode(Buffer.from("Hello"))).toBe("JBSWY3DP");
  });

  it("decodes JBSWY3DP to 'Hello'", () => {
    expect(decode("JBSWY3DP")).toEqual(Buffer.from("Hello"));
  });

  it("decodes case-insensitively", () => {
    expect(decode("jbswy3dp")).toEqual(Buffer.from("Hello"));
  });

  it("roundtrips variable-length data", () => {
    const data = Buffer.from([0x00, 0xff, 0x01, 0xfe, 0x02, 0xfd, 0x03]);
    for (let len = 1; len <= data.length; len++) {
      const slice = data.subarray(0, len);
      const encoded = encode(slice);
      const decoded = decode(encoded);
      expect(decoded).toEqual(slice);
    }
  });

  it("encodes empty input to empty string", () => {
    expect(encode(Buffer.alloc(0))).toBe("");
  });

  it("rejects invalid characters", () => {
    expect(() => decode("0000")).toThrow();
    expect(() => decode("1111")).toThrow();
    expect(() => decode("8888")).toThrow();
    expect(() => decode("!@#$")).toThrow();
  });

  it("roundtrips all 256 byte values", () => {
    const data = Buffer.alloc(256);
    for (let i = 0; i < 256; i++) data[i] = i;
    const encoded = encode(data);
    const decoded = decode(encoded);
    expect(decoded).toEqual(data);
  });

  it("roundtrips each single byte individually", () => {
    for (let i = 0; i < 256; i++) {
      const buf = Buffer.from([i]);
      const decoded = decode(encode(buf));
      expect(decoded).toEqual(buf);
    }
  });
});
