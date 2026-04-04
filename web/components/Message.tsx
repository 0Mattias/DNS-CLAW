"use client";

import Markdown from "./Markdown";
import type { Message as MessageType } from "@/lib/types";

interface MessageProps {
  message: MessageType;
}

export default function Message({ message }: MessageProps) {
  if (message.role === "user") {
    return (
      <div className="mb-4">
        <div className="flex items-center gap-2 mb-1">
          <span className="text-[rgb(255,100,80)] font-mono text-sm font-bold">
            {">"}{" "}
          </span>
          <span className="text-xs text-zinc-600 font-mono">you</span>
        </div>
        <div className="pl-5 text-zinc-200 font-mono text-sm whitespace-pre-wrap">
          {message.content}
        </div>
      </div>
    );
  }

  // Tool call display
  if (message.toolCall) {
    return (
      <div className="mb-4">
        <div className="flex items-center gap-2 mb-1">
          <span className="text-[rgb(255,60,50)] font-mono text-sm font-bold">
            $
          </span>
          <span className="text-xs text-zinc-600 font-mono">tool call</span>
        </div>
        <div className="ml-5 rounded-lg border border-zinc-800 bg-zinc-900/50 p-3">
          <div className="flex items-center gap-2 mb-2">
            <span className="px-2 py-0.5 rounded bg-[rgb(255,60,50)]/10 text-[rgb(255,100,80)] text-xs font-mono font-bold">
              {message.toolCall.name}
            </span>
          </div>
          <pre className="text-xs text-zinc-400 font-mono overflow-x-auto">
            {JSON.stringify(message.toolCall.args, null, 2)}
          </pre>
          <p className="text-xs text-zinc-600 mt-2 italic">
            Tool execution is available in the CLI client only.
          </p>
        </div>
      </div>
    );
  }

  // Assistant text response
  return (
    <div className="mb-4">
      <div className="flex items-center gap-2 mb-1">
        <span className="text-[rgb(255,60,50)] font-mono text-sm font-bold">
          ~
        </span>
        <span className="text-xs text-zinc-600 font-mono">dns-claw</span>
      </div>
      <div className="pl-5 text-zinc-300 text-sm leading-relaxed">
        <Markdown content={message.content} />
      </div>
    </div>
  );
}
