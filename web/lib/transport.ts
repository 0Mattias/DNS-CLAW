/**
 * transport.ts — UDP / DoH / DoT DNS transport.
 *
 * Mirrors the transport functions in src/common/dns_proto.c.
 * Each transport sends a DNS wire-format query and returns the TXT response.
 */

import * as dgram from "dgram";
import * as tls from "tls";
import * as net from "net";
import { buildQuery, parseTxtResponse } from "./dns";
import type { ConnectionConfig } from "./types";

const DNS_MAX_MSG = 65535;
const BACKOFF_MS = [0, 300, 800];
const MAX_RETRIES = 3;

/** Send DNS query over UDP, return TXT response string. */
async function queryUdp(
  server: string,
  port: number,
  qname: string,
): Promise<string> {
  const qid = (Math.random() * 0xffff) | 0;
  const queryBuf = buildQuery(qid, qname);

  return new Promise((resolve, reject) => {
    const socket = dgram.createSocket("udp4");
    const timeout = setTimeout(() => {
      socket.close();
      reject(new Error("UDP timeout"));
    }, 5000);

    socket.on("message", (msg) => {
      clearTimeout(timeout);
      socket.close();

      // Verify response ID matches query ID (anti-spoofing)
      if (msg.length >= 2) {
        const respId = (msg[0] << 8) | msg[1];
        if (respId !== qid) {
          reject(new Error("DNS response ID mismatch"));
          return;
        }
      }

      try {
        const { txt } = parseTxtResponse(msg);
        resolve(txt);
      } catch (e) {
        reject(e);
      }
    });

    socket.on("error", (err) => {
      clearTimeout(timeout);
      socket.close();
      reject(err);
    });

    socket.send(queryBuf, port, server);
  });
}

/** Send DNS query over DoH (HTTPS POST), return TXT response string. */
async function queryDoh(
  url: string,
  qname: string,
  insecure?: boolean,
): Promise<string> {
  const qid = (Math.random() * 0xffff) | 0;
  const queryBuf = buildQuery(qid, qname);

  // Node.js fetch with custom agent for insecure mode
  const fetchOpts: RequestInit & { dispatcher?: unknown } = {
    method: "POST",
    headers: {
      "Content-Type": "application/dns-message",
      Accept: "application/dns-message",
    },
    body: new Uint8Array(queryBuf),
    signal: AbortSignal.timeout(10000),
  };

  if (insecure) {
    // Use undici agent for TLS skip — available in Node 18+
    process.env.NODE_TLS_REJECT_UNAUTHORIZED = "0";
  }

  const resp = await fetch(url, fetchOpts);

  if (insecure) {
    process.env.NODE_TLS_REJECT_UNAUTHORIZED = "1";
  }

  if (!resp.ok) throw new Error(`DoH HTTP ${resp.status}`);

  const respBuf = Buffer.from(await resp.arrayBuffer());
  const { txt } = parseTxtResponse(respBuf);
  return txt;
}

/** Send DNS query over DoT (TLS), return TXT response string. */
async function queryDot(
  server: string,
  port: number,
  qname: string,
  insecure?: boolean,
): Promise<string> {
  const qid = (Math.random() * 0xffff) | 0;
  const queryBuf = buildQuery(qid, qname);

  return new Promise((resolve, reject) => {
    const socket = tls.connect(
      {
        host: server,
        port,
        rejectUnauthorized: !insecure,
      },
      () => {
        // DNS over TCP: 2-byte length prefix + message
        const lenBuf = Buffer.alloc(2);
        lenBuf.writeUInt16BE(queryBuf.length, 0);
        socket.write(lenBuf);
        socket.write(queryBuf);
      },
    );

    const timeout = setTimeout(() => {
      socket.destroy();
      reject(new Error("DoT timeout"));
    }, 10000);

    const chunks: Buffer[] = [];

    socket.on("data", (data) => {
      chunks.push(data);

      // Check if we have the full response
      const full = Buffer.concat(chunks);
      if (full.length >= 2) {
        const respLen = full.readUInt16BE(0);
        if (full.length >= 2 + respLen) {
          clearTimeout(timeout);
          socket.destroy();

          const respBuf = full.subarray(2, 2 + respLen);
          try {
            const { txt } = parseTxtResponse(respBuf);
            resolve(txt);
          } catch (e) {
            reject(e);
          }
        }
      }
    });

    socket.on("error", (err) => {
      clearTimeout(timeout);
      reject(err);
    });
  });
}

/**
 * Send a DNS query using the configured transport, with retry logic.
 * Returns the TXT record string from the response.
 */
export async function dnsQuery(
  config: ConnectionConfig,
  qname: string,
): Promise<string> {
  for (let attempt = 0; attempt < MAX_RETRIES; attempt++) {
    if (attempt > 0) {
      await new Promise((r) => setTimeout(r, BACKOFF_MS[attempt]));
    }

    try {
      if (config.transport === "doh") {
        // DoH URL: the server address is the full URL
        const url = config.server.startsWith("http")
          ? config.server
          : `https://${config.server}:${config.port}/dns-query`;
        return await queryDoh(url, qname, config.insecure);
      } else if (config.transport === "dot") {
        return await queryDot(config.server, config.port, qname, config.insecure);
      } else {
        return await queryUdp(config.server, config.port, qname);
      }
    } catch {
      if (attempt === MAX_RETRIES - 1) throw new Error(`DNS query failed after ${MAX_RETRIES} attempts: ${qname.substring(0, 60)}...`);
    }
  }

  throw new Error("Unreachable");
}
