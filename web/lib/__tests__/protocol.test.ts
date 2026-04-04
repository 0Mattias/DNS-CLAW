/**
 * protocol.test.ts — Unit tests for DNS-CLAW session protocol.
 *
 * Mocks the transport layer to test protocol logic in isolation.
 */

import { describe, it, expect, vi, beforeEach } from "vitest";
import type { ConnectionConfig, StatusEvent } from "../types";

vi.mock("../transport", () => ({
  dnsQuery: vi.fn(),
}));

import { dnsQuery } from "../transport";
import {
  initSession,
  resumeSession,
  listSessions,
  sendMessage,
  nextMsgId,
} from "../protocol";

const mockedDnsQuery = vi.mocked(dnsQuery);

const baseConfig: ConnectionConfig = {
  server: "127.0.0.1",
  port: 53,
  transport: "udp",
};

beforeEach(() => {
  mockedDnsQuery.mockReset();
});

describe("nextMsgId", () => {
  it("increments by 1", () => {
    expect(nextMsgId(0)).toBe(1);
    expect(nextMsgId(42)).toBe(43);
  });

  it("wraps at 256", () => {
    expect(nextMsgId(255)).toBe(0);
  });
});

describe("initSession", () => {
  it("queries init domain and returns session ID", async () => {
    mockedDnsQuery.mockResolvedValueOnce("abc123def456");
    const sid = await initSession(baseConfig);
    expect(sid).toBe("abc123def456");
    expect(mockedDnsQuery).toHaveBeenCalledWith(baseConfig, "init.llm.local.");
  });

  it("includes auth token in domain", async () => {
    mockedDnsQuery.mockResolvedValueOnce("sid123");
    const config = { ...baseConfig, authToken: "mytoken" };
    await initSession(config);
    expect(mockedDnsQuery).toHaveBeenCalledWith(
      config,
      "init.mytoken.llm.local.",
    );
  });

  it("trims whitespace from session ID", async () => {
    mockedDnsQuery.mockResolvedValueOnce("  sid123  \n");
    const sid = await initSession(baseConfig);
    expect(sid).toBe("sid123");
  });
});

describe("resumeSession", () => {
  it("queries resume domain with old session ID", async () => {
    mockedDnsQuery.mockResolvedValueOnce("newsid");
    const sid = await resumeSession(baseConfig, "oldsid");
    expect(sid).toBe("newsid");
    expect(mockedDnsQuery).toHaveBeenCalledWith(
      baseConfig,
      "resume.oldsid.llm.local.",
    );
  });
});

describe("listSessions", () => {
  it("parses JSON session list", async () => {
    const sessions = [{ id: "s1", date: 1000 }];
    mockedDnsQuery.mockResolvedValueOnce(JSON.stringify(sessions));
    const result = await listSessions(baseConfig);
    expect(result).toEqual(sessions);
  });

  it("returns empty array on invalid JSON", async () => {
    mockedDnsQuery.mockResolvedValueOnce("not json");
    const result = await listSessions(baseConfig);
    expect(result).toEqual([]);
  });
});

describe("sendMessage", () => {
  it("completes full upload/download flow", async () => {
    const responseJson = JSON.stringify({
      type: "text",
      content: "Hello from LLM",
    });
    const responseB64 = Buffer.from(responseJson).toString("base64");

    // Upload chunk(s) → ACK, finalize → ACK, download → data then EOF
    mockedDnsQuery.mockImplementation(async (_config, qname: string) => {
      if (qname.includes(".up.")) return "ACK";
      if (qname.startsWith("fin.")) return "ACK";
      if (qname.includes(".down.")) {
        // First call returns data, second returns EOF
        const seq = parseInt(qname.split(".")[0]);
        if (seq === 0) return responseB64;
        return "EOF";
      }
      return "";
    });

    const events: StatusEvent[] = [];
    const result = await sendMessage(baseConfig, "sid1", 1, "Hi", (e) =>
      events.push(e),
    );

    expect(result.type).toBe("text");
    expect(result.content).toBe("Hello from LLM");
  });

  it("fires status events in order", async () => {
    const responseJson = JSON.stringify({ type: "text", content: "ok" });
    const responseB64 = Buffer.from(responseJson).toString("base64");

    mockedDnsQuery.mockImplementation(async (_config, qname: string) => {
      if (qname.includes(".up.")) return "ACK";
      if (qname.startsWith("fin.")) return "ACK";
      if (qname.includes(".down.")) {
        const seq = parseInt(qname.split(".")[0]);
        return seq === 0 ? responseB64 : "EOF";
      }
      return "";
    });

    const statuses: string[] = [];
    await sendMessage(baseConfig, "sid1", 1, "Hi", (e) =>
      statuses.push(e.status),
    );

    expect(statuses[0]).toBe("connecting");
    expect(statuses).toContain("uploading");
    expect(statuses).toContain("processing");
    expect(statuses).toContain("downloading");
    expect(statuses[statuses.length - 1]).toBe("done");
  });

  it("throws on upload failure", async () => {
    mockedDnsQuery.mockResolvedValue("NACK");
    await expect(
      sendMessage(baseConfig, "sid1", 1, "Hi", () => {}),
    ).rejects.toThrow(/upload failed/i);
  });

  it("throws on finalize failure", async () => {
    mockedDnsQuery.mockImplementation(async (_config, qname: string) => {
      if (qname.includes(".up.")) return "ACK";
      if (qname.startsWith("fin.")) return "ERR:busy";
      return "";
    });

    await expect(
      sendMessage(baseConfig, "sid1", 1, "Hi", () => {}),
    ).rejects.toThrow(/finalize/i);
  });

  it("throws on server ERR response during download", async () => {
    mockedDnsQuery.mockImplementation(async (_config, qname: string) => {
      if (qname.includes(".up.")) return "ACK";
      if (qname.startsWith("fin.")) return "ACK";
      if (qname.includes(".down.")) return "ERR:internal";
      return "";
    });

    await expect(
      sendMessage(baseConfig, "sid1", 1, "Hi", () => {}),
    ).rejects.toThrow(/ERR:internal/);
  });

  it("works with encryption (PSK set)", async () => {
    const config = { ...baseConfig, psk: "test-psk-123" };
    const responseJson = JSON.stringify({ type: "text", content: "encrypted" });

    // For encrypted flow, server returns encrypted+base64 response
    const { deriveKey, encrypt } = await import("../crypto");
    const key = deriveKey(config.psk)!;
    const encryptedResponse = encrypt(Buffer.from(responseJson), key);
    const responseB64 = encryptedResponse.toString("base64");

    mockedDnsQuery.mockImplementation(async (_config, qname: string) => {
      if (qname.includes(".up.")) return "ACK";
      if (qname.startsWith("fin.")) return "ACK";
      if (qname.includes(".down.")) {
        const seq = parseInt(qname.split(".")[0]);
        return seq === 0 ? responseB64 : "EOF";
      }
      return "";
    });

    const result = await sendMessage(config, "sid1", 1, "Hi", () => {});
    expect(result.content).toBe("encrypted");
  });
});
