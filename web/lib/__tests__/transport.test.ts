/**
 * transport.test.ts — Unit tests for DNS transport retry/routing logic.
 *
 * Mocks dgram, tls, and fetch to test dnsQuery behavior without network I/O.
 */

import { describe, it, expect, vi, beforeEach } from "vitest";
import type { ConnectionConfig } from "../types";

// Mock dgram
const mockSocket = {
  on: vi.fn(),
  send: vi.fn(),
  close: vi.fn(),
};
vi.mock("dgram", () => ({
  createSocket: vi.fn(() => mockSocket),
}));

// Mock tls
const mockTlsSocket = {
  on: vi.fn(),
  write: vi.fn(),
  destroy: vi.fn(),
};
vi.mock("tls", () => ({
  connect: vi.fn((_opts: unknown, cb: () => void) => {
    setTimeout(cb, 0);
    return mockTlsSocket;
  }),
}));

// Mock dns module — buildQuery must embed the query ID in bytes 0-1
// so queryUdp's anti-spoofing check passes.
vi.mock("../dns", () => ({
  buildQuery: vi.fn((id: number) => {
    const buf = Buffer.alloc(32);
    buf.writeUInt16BE(id & 0xffff, 0);
    return buf;
  }),
  parseTxtResponse: vi.fn(() => ({ rcode: 0, txt: "RESPONSE" })),
}));

import { dnsQuery } from "../transport";
import * as dgram from "dgram";
import * as tls from "tls";

const baseConfig: ConnectionConfig = {
  server: "127.0.0.1",
  port: 53,
  transport: "udp",
};

beforeEach(() => {
  vi.clearAllMocks();
  vi.restoreAllMocks();

  // Reset socket mocks
  mockSocket.on.mockReset();
  mockSocket.send.mockReset();
  mockSocket.close.mockReset();
  mockTlsSocket.on.mockReset();
  mockTlsSocket.write.mockReset();
  mockTlsSocket.destroy.mockReset();
});

describe("dnsQuery", () => {
  it("uses UDP by default", async () => {
    // Capture the message callback and trigger it when send is called
    let messageCallback: ((...args: unknown[]) => void) | null = null;

    mockSocket.on.mockImplementation((event: string, cb: (...args: unknown[]) => void) => {
      if (event === "message") messageCallback = cb;
      return mockSocket;
    });

    mockSocket.send.mockImplementation((buf: Buffer) => {
      // Echo back query ID (first 2 bytes) in a mock response
      const resp = Buffer.alloc(32);
      resp[0] = buf[0];
      resp[1] = buf[1];
      setTimeout(() => messageCallback?.(resp), 5);
    });

    const result = await dnsQuery(baseConfig, "test.llm.local.");
    expect(dgram.createSocket).toHaveBeenCalledWith("udp4");
    expect(result).toBe("RESPONSE");
  });

  it("uses DoH when transport is 'doh'", async () => {
    const config: ConnectionConfig = {
      ...baseConfig,
      transport: "doh",
      server: "https://dns.example.com/dns-query",
    };

    const mockFetch = vi.fn().mockResolvedValue({
      ok: true,
      arrayBuffer: () => Promise.resolve(new ArrayBuffer(32)),
    });
    vi.stubGlobal("fetch", mockFetch);

    const result = await dnsQuery(config, "test.llm.local.");
    expect(mockFetch).toHaveBeenCalled();
    expect(result).toBe("RESPONSE");

    vi.unstubAllGlobals();
  });

  it("uses DoT when transport is 'dot'", async () => {
    const config: ConnectionConfig = {
      ...baseConfig,
      transport: "dot",
      port: 853,
    };

    // Simulate TLS data response with 2-byte length prefix
    const responseData = Buffer.alloc(32);
    const lenBuf = Buffer.alloc(2);
    lenBuf.writeUInt16BE(responseData.length, 0);
    const fullResponse = Buffer.concat([lenBuf, responseData]);

    mockTlsSocket.on.mockImplementation((event: string, cb: (...args: unknown[]) => void) => {
      if (event === "data") {
        setTimeout(() => cb(fullResponse), 10);
      }
      return mockTlsSocket;
    });

    const result = await dnsQuery(config, "test.llm.local.");
    expect(tls.connect).toHaveBeenCalled();
    expect(result).toBe("RESPONSE");
  });

  it("retries on failure and eventually throws", async () => {
    mockSocket.on.mockImplementation((event: string, cb: (...args: unknown[]) => void) => {
      if (event === "error") {
        setTimeout(() => cb(new Error("network error")), 5);
      }
      return mockSocket;
    });
    mockSocket.send.mockImplementation(() => {});

    await expect(
      dnsQuery(baseConfig, "test.llm.local."),
    ).rejects.toThrow(/failed after 3 attempts/);

    // Should have created 3 sockets (3 attempts)
    expect(dgram.createSocket).toHaveBeenCalledTimes(3);
  });
});
