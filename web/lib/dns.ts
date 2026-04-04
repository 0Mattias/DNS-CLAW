/**
 * dns.ts — DNS wire-format builder/parser.
 *
 * Mirrors src/common/dns_proto.c. Only implements TXT queries/answers
 * needed by the DNS-CLAW tunneling protocol.
 */

const DNS_TYPE_TXT = 16;
const DNS_CLASS_IN = 1;

/** Encode a dotted name into DNS wire format label sequence. */
function encodeQname(name: string): Buffer {
  const parts: Buffer[] = [];
  const labels = name.split(".");

  for (const label of labels) {
    if (label.length === 0) continue; // trailing dot
    if (label.length > 63) throw new Error(`DNS label too long: ${label.length}`);
    parts.push(Buffer.from([label.length]));
    parts.push(Buffer.from(label, "ascii"));
  }
  parts.push(Buffer.from([0])); // root label
  return Buffer.concat(parts);
}

/** Build a DNS TXT query in wire format. */
export function buildQuery(id: number, qname: string): Buffer {
  // Header: 12 bytes
  const header = Buffer.alloc(12);
  header.writeUInt16BE(id & 0xffff, 0); // ID
  header.writeUInt16BE(0x0100, 2); // Flags: RD=1
  header.writeUInt16BE(1, 4); // QDCOUNT=1
  // ANCOUNT, NSCOUNT, ARCOUNT = 0

  const qnameBytes = encodeQname(qname);

  // QTYPE + QCLASS: 4 bytes
  const suffix = Buffer.alloc(4);
  suffix.writeUInt16BE(DNS_TYPE_TXT, 0);
  suffix.writeUInt16BE(DNS_CLASS_IN, 2);

  return Buffer.concat([header, qnameBytes, suffix]);
}

/** Parse a DNS response, extract the first TXT record's data. */
export function parseTxtResponse(buf: Buffer): { rcode: number; txt: string } {
  if (buf.length < 12) throw new Error("DNS response too short");

  const rcode = buf[3] & 0x0f;
  const qdcount = buf.readUInt16BE(4);
  const ancount = buf.readUInt16BE(6);

  // Skip question section
  let pos = 12;
  for (let i = 0; i < qdcount; i++) {
    // Skip QNAME
    while (pos < buf.length) {
      if (buf[pos] === 0) {
        pos++;
        break;
      }
      if ((buf[pos] & 0xc0) === 0xc0) {
        pos += 2;
        break;
      }
      const labelLen = buf[pos];
      pos += 1 + labelLen;
    }
    pos += 4; // QTYPE + QCLASS
  }

  // Parse answer section — find first TXT record
  let txt = "";
  for (let i = 0; i < ancount; i++) {
    if (pos >= buf.length) break;

    // Skip NAME (handle pointer compression)
    while (pos < buf.length) {
      if (buf[pos] === 0) {
        pos++;
        break;
      }
      if ((buf[pos] & 0xc0) === 0xc0) {
        pos += 2;
        break;
      }
      const labelLen = buf[pos];
      pos += 1 + labelLen;
    }

    if (pos + 10 > buf.length) break;

    const rtype = buf.readUInt16BE(pos);
    pos += 2;
    pos += 2; // RCLASS
    pos += 4; // TTL
    const rdlen = buf.readUInt16BE(pos);
    pos += 2;

    if (rtype === DNS_TYPE_TXT && txt.length === 0) {
      // TXT RDATA: sequence of length-prefixed strings
      const rdataEnd = pos + rdlen;
      const chunks: Buffer[] = [];
      while (pos < rdataEnd) {
        const slen = buf[pos++];
        if (pos + slen > buf.length || pos + slen > rdataEnd) break;
        chunks.push(buf.subarray(pos, pos + slen));
        pos += slen;
      }
      txt = Buffer.concat(chunks).toString("utf-8");
    } else {
      pos += rdlen;
    }
  }

  return { rcode, txt };
}
