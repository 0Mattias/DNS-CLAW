/**
 * protocol.ts — DNS-CLAW session protocol client.
 *
 * Mirrors src/client/protocol.c. Implements the full tunneling flow:
 * encrypt → base32 → DNS chunk upload → finalize → poll download → base64 decode → decrypt
 */

import { dnsQuery } from "./transport";
import { encode as b32encode } from "./base32";
import { deriveKey, encrypt, decrypt, CRYPTO_OVERHEAD } from "./crypto";
import type { ConnectionConfig, LLMResponse, StatusEvent } from "./types";

/** Protocol constants — must match server (protocol.h) */
const UPLOAD_CHUNK_SZ = 35; // bytes per upload chunk → base32(35) = 56 chars
const MAX_MSG_IDS = 256;

/** Build domain suffix with optional auth token. */
function domainSuffix(authToken?: string): string {
  if (authToken) return `.${authToken}.llm.local.`;
  return ".llm.local.";
}

function domainPrefix(prefix: string, authToken?: string): string {
  if (authToken) return `${prefix}.${authToken}.llm.local.`;
  return `${prefix}.llm.local.`;
}

/** Initialize a new session. Returns session ID. */
export async function initSession(config: ConnectionConfig): Promise<string> {
  const txt = await dnsQuery(config, domainPrefix("init", config.authToken));
  return txt.trim();
}

/** Resume a saved session. Returns new session ID. */
export async function resumeSession(
  config: ConnectionConfig,
  oldSid: string,
): Promise<string> {
  const txt = await dnsQuery(
    config,
    domainPrefix(`resume.${oldSid}`, config.authToken),
  );
  return txt.trim();
}

/** List saved sessions. */
export async function listSessions(
  config: ConnectionConfig,
): Promise<Array<{ id: string; date: number }>> {
  const txt = await dnsQuery(
    config,
    domainPrefix("list.sessions", config.authToken),
  );
  try {
    return JSON.parse(txt);
  } catch {
    return [];
  }
}

/**
 * Send a message through the DNS tunnel and receive the LLM response.
 *
 * Calls `onStatus` with progress events for the UI to display.
 * Returns the parsed LLM response.
 */
export async function sendMessage(
  config: ConnectionConfig,
  sessionId: string,
  msgId: number,
  content: string,
  onStatus: (event: StatusEvent) => void,
  options?: { type?: string; toolName?: string },
): Promise<LLMResponse> {
  const suffix = domainSuffix(config.authToken);
  const key = deriveKey(config.psk);
  const type = options?.type || "user";

  onStatus({ status: "connecting" });

  // 1. Build JSON payload
  const payload: Record<string, string> = { type, content };
  if (options?.toolName) payload.tool_name = options.toolName;
  const payloadStr = JSON.stringify(payload);

  // 2. Encrypt if PSK configured
  let uploadData: Buffer;
  if (key) {
    uploadData = encrypt(Buffer.from(payloadStr, "utf-8"), key);
  } else {
    uploadData = Buffer.from(payloadStr, "utf-8");
  }

  // 3. Base32 encode → lowercase → split into chunks
  const b32 = b32encode(uploadData).toLowerCase();
  const chunks: string[] = [];
  // Split the raw binary into UPLOAD_CHUNK_SZ byte chunks, then base32 each
  for (let i = 0; i < uploadData.length; i += UPLOAD_CHUNK_SZ) {
    const chunkBytes = uploadData.subarray(i, i + UPLOAD_CHUNK_SZ);
    chunks.push(b32encode(chunkBytes).toLowerCase());
  }

  // 4. Upload chunks via DNS
  for (let seq = 0; seq < chunks.length; seq++) {
    onStatus({ status: "uploading", chunk: seq + 1, total: chunks.length });

    const qname = `${chunks[seq]}.${seq}.up.${msgId}.${sessionId}${suffix}`;
    const txt = await dnsQuery(config, qname);
    if (txt !== "ACK") {
      throw new Error(`Upload failed at chunk ${seq}: ${txt}`);
    }
  }

  // 5. Finalize — trigger LLM processing
  onStatus({ status: "processing" });
  const finQname = `fin.${msgId}.${sessionId}${suffix}`;
  const finTxt = await dnsQuery(config, finQname);
  if (finTxt !== "ACK") {
    throw new Error(`Server did not ACK finalize: ${finTxt}`);
  }

  // 6. Poll download
  const responseChunks: Buffer[] = [];
  let downSeq = 0;
  let pollCount = 0;

  while (true) {
    const downQname = `${downSeq}.${msgId}.down.${sessionId}${suffix}`;
    const txt = await dnsQuery(config, downQname);

    if (txt === "PENDING") {
      pollCount++;
      // Adaptive backoff: 100ms → 200ms → 400ms → ... → 3s max
      const delay = Math.min(100 * Math.pow(2, Math.min(pollCount, 5)), 3000);
      await new Promise((r) => setTimeout(r, delay));
      continue;
    }

    if (txt === "EOF") break;

    if (txt.startsWith("ERR:")) {
      throw new Error(`Server error: ${txt}`);
    }

    // Base64 decode the chunk
    onStatus({ status: "downloading", chunk: downSeq + 1 });
    const decoded = Buffer.from(txt, "base64");
    responseChunks.push(decoded);
    downSeq++;
  }

  // 7. Reassemble and decrypt
  let responseBytes = Buffer.concat(responseChunks);

  if (key && responseBytes.length > CRYPTO_OVERHEAD) {
    responseBytes = Buffer.from(decrypt(responseBytes, key));
  }

  // 8. Parse JSON response
  const responseStr = responseBytes.toString("utf-8");
  const response: LLMResponse = JSON.parse(responseStr);

  onStatus({ status: "done", response });
  return response;
}

/** Get next message ID (wraps around MAX_MSG_IDS). */
export function nextMsgId(current: number): number {
  return (current + 1) % MAX_MSG_IDS;
}
