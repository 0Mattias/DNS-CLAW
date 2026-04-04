/**
 * base32.ts — RFC 4648 Base32 encoding/decoding (no padding).
 *
 * Mirrors src/common/base32.c exactly.
 */

const ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

function b32val(c: string): number {
  const ch = c.toUpperCase();
  if (ch >= "A" && ch <= "Z") return ch.charCodeAt(0) - 65;
  if (ch >= "2" && ch <= "7") return ch.charCodeAt(0) - 50 + 26;
  return -1;
}

export function encode(data: Buffer): string {
  let out = "";
  let i = 0;

  // Process full 5-byte blocks
  while (i + 5 <= data.length) {
    const block =
      (BigInt(data[i]) << 32n) |
      (BigInt(data[i + 1]) << 24n) |
      (BigInt(data[i + 2]) << 16n) |
      (BigInt(data[i + 3]) << 8n) |
      BigInt(data[i + 4]);

    out += ALPHABET[Number((block >> 35n) & 0x1fn)];
    out += ALPHABET[Number((block >> 30n) & 0x1fn)];
    out += ALPHABET[Number((block >> 25n) & 0x1fn)];
    out += ALPHABET[Number((block >> 20n) & 0x1fn)];
    out += ALPHABET[Number((block >> 15n) & 0x1fn)];
    out += ALPHABET[Number((block >> 10n) & 0x1fn)];
    out += ALPHABET[Number((block >> 5n) & 0x1fn)];
    out += ALPHABET[Number(block & 0x1fn)];

    i += 5;
  }

  // Handle remaining bytes (no padding)
  const remaining = data.length - i;
  if (remaining > 0) {
    let block = 0n;
    for (let j = 0; j < remaining; j++) {
      block |= BigInt(data[i + j]) << BigInt(32 - j * 8);
    }

    const outChars = [0, 2, 4, 5, 7][remaining]!;
    for (let j = 0; j < outChars; j++) {
      out += ALPHABET[Number((block >> BigInt(35 - j * 5)) & 0x1fn)];
    }
  }

  return out;
}

export function decode(str: string): Buffer {
  const out: number[] = [];
  let i = 0;

  // Process full 8-char blocks
  while (i + 8 <= str.length) {
    let block = 0n;
    for (let j = 0; j < 8; j++) {
      const v = b32val(str[i + j]);
      if (v < 0) throw new Error(`Invalid base32 char: ${str[i + j]}`);
      block = (block << 5n) | BigInt(v);
    }
    out.push(Number((block >> 32n) & 0xffn));
    out.push(Number((block >> 24n) & 0xffn));
    out.push(Number((block >> 16n) & 0xffn));
    out.push(Number((block >> 8n) & 0xffn));
    out.push(Number(block & 0xffn));
    i += 8;
  }

  // Handle remaining chars
  const remaining = str.length - i;
  if (remaining > 0) {
    let block = 0n;
    for (let j = 0; j < remaining; j++) {
      const v = b32val(str[i + j]);
      if (v < 0) throw new Error(`Invalid base32 char: ${str[i + j]}`);
      block = (block << 5n) | BigInt(v);
    }
    block <<= BigInt((8 - remaining) * 5);

    const outBytes: Record<number, number> = { 2: 1, 4: 2, 5: 3, 7: 4 };
    const n = outBytes[remaining];
    if (n === undefined) throw new Error(`Invalid base32 length remainder: ${remaining}`);
    for (let j = 0; j < n; j++) {
      out.push(Number((block >> BigInt(32 - j * 8)) & 0xffn));
    }
  }

  return Buffer.from(out);
}
