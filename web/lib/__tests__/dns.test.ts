/**
 * dns.test.ts — Unit tests for DNS wire-format builder/parser.
 *
 * Mirrors tests/test_dns_proto.c test vectors and edge cases.
 * Includes a helper to build DNS response wire format for parser tests.
 */

import { describe, it, expect } from "vitest";
import { buildQuery, parseTxtResponse } from "../dns";

/** Build a minimal DNS response with a TXT record for testing the parser. */
function buildTestResponse(
  id: number,
  qname: string,
  rcode: number,
  txt: string | null,
): Buffer {
  // Header (12 bytes)
  const header = Buffer.alloc(12);
  header.writeUInt16BE(id & 0xffff, 0);
  header.writeUInt16BE(0x8000 | (rcode & 0x0f), 2); // QR=1, RCODE
  header.writeUInt16BE(1, 4); // QDCOUNT=1
  header.writeUInt16BE(txt !== null ? 1 : 0, 6); // ANCOUNT

  // Question section: encode qname
  const qnameParts: Buffer[] = [];
  for (const label of qname.split(".")) {
    if (label.length === 0) continue;
    qnameParts.push(Buffer.from([label.length]));
    qnameParts.push(Buffer.from(label, "ascii"));
  }
  qnameParts.push(Buffer.from([0])); // root
  const qnameBytes = Buffer.concat(qnameParts);
  const qsuffix = Buffer.alloc(4);
  qsuffix.writeUInt16BE(16, 0); // TXT
  qsuffix.writeUInt16BE(1, 2); // IN

  if (txt === null) {
    return Buffer.concat([header, qnameBytes, qsuffix]);
  }

  // Answer section: pointer to qname + TXT record
  const ansName = Buffer.from([0xc0, 12]); // pointer to offset 12
  const ansHeader = Buffer.alloc(8);
  ansHeader.writeUInt16BE(16, 0); // TYPE = TXT
  ansHeader.writeUInt16BE(1, 2); // CLASS = IN
  ansHeader.writeUInt32BE(300, 4); // TTL

  // TXT RDATA: split into 255-byte character-strings
  const txtBuf = Buffer.from(txt, "utf-8");
  const rdataParts: Buffer[] = [];
  for (let i = 0; i < txtBuf.length; i += 255) {
    const chunk = txtBuf.subarray(i, i + 255);
    rdataParts.push(Buffer.from([chunk.length]));
    rdataParts.push(chunk);
  }
  if (txtBuf.length === 0) {
    rdataParts.push(Buffer.from([0]));
  }
  const rdata = Buffer.concat(rdataParts);

  const rdlen = Buffer.alloc(2);
  rdlen.writeUInt16BE(rdata.length, 0);

  return Buffer.concat([
    header,
    qnameBytes,
    qsuffix,
    ansName,
    ansHeader,
    rdlen,
    rdata,
  ]);
}

describe("dns", () => {
  describe("buildQuery", () => {
    it("produces valid DNS header", () => {
      const buf = buildQuery(0x1234, "init.llm.local.");
      expect(buf.length).toBeGreaterThan(12);
      // ID
      expect(buf.readUInt16BE(0)).toBe(0x1234);
      // Flags: RD=1
      expect(buf.readUInt16BE(2)).toBe(0x0100);
      // QDCOUNT=1
      expect(buf.readUInt16BE(4)).toBe(1);
    });

    it("encodes qname labels correctly", () => {
      const buf = buildQuery(1, "init.llm.local.");
      // After 12-byte header: label lengths and data
      expect(buf[12]).toBe(4); // "init" length
      expect(buf.subarray(13, 17).toString()).toBe("init");
      expect(buf[17]).toBe(3); // "llm" length
    });

    it("appends TXT type and IN class", () => {
      const buf = buildQuery(1, "t.llm.local.");
      // Find the end: after qname null terminator, last 4 bytes
      const typeClass = buf.subarray(buf.length - 4);
      expect(typeClass.readUInt16BE(0)).toBe(16); // TXT
      expect(typeClass.readUInt16BE(2)).toBe(1); // IN
    });

    it("handles full query ID range", () => {
      for (const id of [0x0000, 0xffff, 0x8000]) {
        const buf = buildQuery(id, "t.llm.local.");
        expect(buf.readUInt16BE(0)).toBe(id);
      }
    });

    it("throws on label too long (64 chars)", () => {
      const longLabel = "a".repeat(64) + ".llm.local.";
      expect(() => buildQuery(1, longLabel)).toThrow();
    });

    it("accepts max label length (63 chars)", () => {
      const maxLabel = "a".repeat(63) + ".llm.local.";
      const buf = buildQuery(1, maxLabel);
      expect(buf.length).toBeGreaterThan(12);
    });

    it("encodes multi-label queries", () => {
      const qname = "data.0.up.1.abcdef12.llm.local.";
      const buf = buildQuery(0x5678, qname);
      expect(buf.length).toBeGreaterThan(12);
      expect(buf.readUInt16BE(0)).toBe(0x5678);
    });
  });

  describe("parseTxtResponse", () => {
    it("extracts TXT data", () => {
      const buf = buildTestResponse(0xabcd, "test.llm.local.", 0, "Hello World");
      const { rcode, txt } = parseTxtResponse(buf);
      expect(rcode).toBe(0);
      expect(txt).toBe("Hello World");
    });

    it("preserves rcode values", () => {
      for (const rc of [0, 1, 2, 3]) {
        const buf = buildTestResponse(1, "t.llm.local.", rc, null);
        expect(parseTxtResponse(buf).rcode).toBe(rc);
      }
    });

    it("handles empty TXT", () => {
      const buf = buildTestResponse(1, "t.llm.local.", 0, "");
      const { rcode, txt } = parseTxtResponse(buf);
      expect(rcode).toBe(0);
      expect(txt).toBe("");
    });

    it("handles NXDOMAIN with no answer", () => {
      const buf = buildTestResponse(1, "x.llm.local.", 3, null);
      const { rcode, txt } = parseTxtResponse(buf);
      expect(rcode).toBe(3);
      expect(txt).toBe("");
    });

    it("throws on truncated message", () => {
      const buf = Buffer.alloc(8);
      expect(() => parseTxtResponse(buf)).toThrow();
    });

    it("roundtrips session ID", () => {
      const sid = "a1b2c3d4e5f67890";
      const buf = buildTestResponse(3, "init.llm.local.", 0, sid);
      expect(parseTxtResponse(buf).txt).toBe(sid);
    });

    it("handles long TXT (>255 bytes, multi-part)", () => {
      const longTxt = "A".repeat(599);
      const buf = buildTestResponse(2, "t.llm.local.", 0, longTxt);
      expect(parseTxtResponse(buf).txt).toBe(longTxt);
    });
  });
});
