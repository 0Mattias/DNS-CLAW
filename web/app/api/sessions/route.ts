/**
 * GET /api/sessions — List saved sessions from the DNS-CLAW server.
 * POST /api/sessions — Create or resume a session.
 */

import {
  initSession,
  resumeSession,
  listSessions,
} from "@/lib/protocol";
import type { ConnectionConfig } from "@/lib/types";

export async function GET(req: Request) {
  const { searchParams } = new URL(req.url);
  const configStr = searchParams.get("config");

  if (!configStr) {
    return Response.json({ error: "Missing config parameter" }, { status: 400 });
  }

  try {
    const config: ConnectionConfig = JSON.parse(configStr);
    const sessions = await listSessions(config);
    return Response.json({ sessions });
  } catch (err) {
    return Response.json(
      { error: err instanceof Error ? err.message : "Failed to list sessions" },
      { status: 500 },
    );
  }
}

export async function POST(req: Request) {
  const body = await req.json();
  const { config, resumeId } = body as {
    config: ConnectionConfig;
    resumeId?: string;
  };

  try {
    let sessionId: string;
    if (resumeId) {
      sessionId = await resumeSession(config, resumeId);
    } else {
      sessionId = await initSession(config);
    }
    return Response.json({ sessionId });
  } catch (err) {
    return Response.json(
      { error: err instanceof Error ? err.message : "Failed to create session" },
      { status: 500 },
    );
  }
}
