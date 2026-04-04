/**
 * POST /api/chat — Tunnel a message through DNS-CLAW.
 *
 * Streams status events back to the browser as newline-delimited JSON.
 * The browser reads these with fetch() + ReadableStream.
 */

import { initSession, sendMessage, nextMsgId } from "@/lib/protocol";
import type { ConnectionConfig, StatusEvent } from "@/lib/types";

interface ChatRequest {
  config: ConnectionConfig;
  sessionId?: string;
  msgId?: number;
  message: string;
  type?: string;
  toolName?: string;
}

export async function POST(req: Request) {
  const body: ChatRequest = await req.json();
  const { config, message } = body;
  let { sessionId, msgId } = body;

  const encoder = new TextEncoder();

  const stream = new ReadableStream({
    async start(controller) {
      const send = (event: StatusEvent) => {
        controller.enqueue(encoder.encode(JSON.stringify(event) + "\n"));
      };

      try {
        // Create session if needed
        if (!sessionId) {
          send({ status: "connecting" });
          sessionId = await initSession(config);
          // Send session ID as a special event
          controller.enqueue(
            encoder.encode(
              JSON.stringify({ status: "connecting", sessionId }) + "\n",
            ),
          );
          msgId = 0;
        }

        // Send message through DNS tunnel
        const currentMsgId = nextMsgId(msgId ?? 0);
        const response = await sendMessage(
          config,
          sessionId,
          currentMsgId,
          message,
          send,
          { type: body.type, toolName: body.toolName },
        );

        // Final event with response + updated msgId
        controller.enqueue(
          encoder.encode(
            JSON.stringify({
              status: "done",
              response,
              sessionId,
              msgId: currentMsgId,
            }) + "\n",
          ),
        );
      } catch (err) {
        send({
          status: "error",
          error: err instanceof Error ? err.message : "Unknown error",
        });
      } finally {
        controller.close();
      }
    },
  });

  return new Response(stream, {
    headers: {
      "Content-Type": "text/plain; charset=utf-8",
      "Transfer-Encoding": "chunked",
      "Cache-Control": "no-cache",
    },
  });
}
