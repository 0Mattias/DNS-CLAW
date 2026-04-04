"use client";

import { useState, useRef, useEffect, useCallback } from "react";
import Message from "./Message";
import Input from "./Input";
import type {
  ConnectionConfig,
  Message as MessageType,
  StatusEvent,
} from "@/lib/types";

interface ChatProps {
  config: ConnectionConfig;
  sessionId: string | null;
  messages: MessageType[];
  onSessionCreated: (sessionId: string) => void;
  onMessagesChange: (messages: MessageType[]) => void;
}

export default function Chat({
  config,
  sessionId,
  messages,
  onSessionCreated,
  onMessagesChange,
}: ChatProps) {
  const [input, setInput] = useState("");
  const [loading, setLoading] = useState(false);
  const [statusText, setStatusText] = useState("");
  const [msgId, setMsgId] = useState(0);
  const scrollRef = useRef<HTMLDivElement>(null);

  // Auto-scroll to bottom
  useEffect(() => {
    scrollRef.current?.scrollTo({
      top: scrollRef.current.scrollHeight,
      behavior: "smooth",
    });
  }, [messages, statusText]);

  const handleSend = useCallback(async () => {
    const text = input.trim();
    if (!text || loading) return;

    setInput("");
    setLoading(true);
    setStatusText("Connecting...");

    // Add user message
    const userMsg: MessageType = {
      id: crypto.randomUUID(),
      role: "user",
      content: text,
      timestamp: Date.now(),
    };
    const updatedMessages = [...messages, userMsg];
    onMessagesChange(updatedMessages);

    try {
      const resp = await fetch("/api/chat", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          config,
          sessionId,
          msgId,
          message: text,
        }),
      });

      if (!resp.body) throw new Error("No response body");

      const reader = resp.body.getReader();
      const decoder = new TextDecoder();
      let buffer = "";

      while (true) {
        const { done, value } = await reader.read();
        if (done) break;

        buffer += decoder.decode(value, { stream: true });
        const lines = buffer.split("\n");
        buffer = lines.pop() || "";

        for (const line of lines) {
          if (!line.trim()) continue;
          try {
            const event: StatusEvent & {
              sessionId?: string;
              msgId?: number;
            } = JSON.parse(line);

            // Handle session creation
            if (event.sessionId && !sessionId) {
              onSessionCreated(event.sessionId);
            }

            // Update status text
            switch (event.status) {
              case "connecting":
                setStatusText("Initializing session...");
                break;
              case "uploading":
                setStatusText(
                  `Uploading chunk ${event.chunk}/${event.total}...`,
                );
                break;
              case "processing":
                setStatusText("Agent is thinking...");
                break;
              case "downloading":
                setStatusText(`Receiving chunk ${event.chunk}...`);
                break;
              case "done":
                if (event.response) {
                  const assistantMsg: MessageType = {
                    id: crypto.randomUUID(),
                    role: "assistant",
                    content: event.response.content || "",
                    timestamp: Date.now(),
                    toolCall:
                      event.response.type === "tool_call"
                        ? {
                            name: event.response.tool_name || "",
                            args: event.response.tool_args || {},
                          }
                        : undefined,
                  };
                  onMessagesChange([...updatedMessages, assistantMsg]);
                }
                if (event.msgId !== undefined) {
                  setMsgId(event.msgId);
                }
                setStatusText("");
                break;
              case "error":
                setStatusText("");
                const errorMsg: MessageType = {
                  id: crypto.randomUUID(),
                  role: "assistant",
                  content: `Error: ${event.error}`,
                  timestamp: Date.now(),
                };
                onMessagesChange([...updatedMessages, errorMsg]);
                break;
            }
          } catch {
            // Skip malformed JSON lines
          }
        }
      }
    } catch (err) {
      const errorMsg: MessageType = {
        id: crypto.randomUUID(),
        role: "assistant",
        content: `Connection error: ${err instanceof Error ? err.message : "Unknown error"}`,
        timestamp: Date.now(),
      };
      onMessagesChange([...updatedMessages, errorMsg]);
    } finally {
      setLoading(false);
      setStatusText("");
    }
  }, [input, loading, config, sessionId, msgId, messages, onMessagesChange, onSessionCreated]);

  return (
    <div className="flex flex-col h-full">
      {/* Messages area */}
      <div ref={scrollRef} className="flex-1 overflow-y-auto px-6 py-4">
        {messages.length === 0 ? (
          <div className="flex flex-col items-center justify-center h-full text-center">
            <pre
              className="text-xs leading-tight mb-6 select-none"
              style={{
                background:
                  "linear-gradient(180deg, rgb(255,60,50), rgb(255,195,180))",
                WebkitBackgroundClip: "text",
                WebkitTextFillColor: "transparent",
              }}
            >
              {`▓█████▄  ███▄    █   ██████
▒██▀ ██▌ ██ ▀█   █ ▒██    ▒
░██   █▌▓██  ▀█ ██▒░ ▓██▄
░▓█▄   ▌▓██▒  ▐▌██▒  ▒   ██
░▒████▓ ▒██░   ▓██░▒██████▒`}
            </pre>
            <h2 className="text-zinc-400 text-sm mb-1">
              DNS-CLAW Web Interface
            </h2>
            <p className="text-zinc-600 text-xs max-w-sm">
              All messages are tunneled through DNS queries to your DNS-CLAW
              server. Configure the connection in the sidebar, then start
              chatting.
            </p>
          </div>
        ) : (
          <>
            {messages.map((msg) => (
              <Message key={msg.id} message={msg} />
            ))}
          </>
        )}

        {/* Loading indicator */}
        {loading && (
          <div className="mb-4">
            <div className="flex items-center gap-2">
              <span className="text-[rgb(255,60,50)] font-mono text-sm font-bold">
                ~
              </span>
              <span className="text-xs text-zinc-500 font-mono animate-pulse">
                {statusText || "..."}
              </span>
            </div>
          </div>
        )}
      </div>

      {/* Input */}
      <Input
        value={input}
        onChange={setInput}
        onSubmit={handleSend}
        disabled={loading}
        placeholder={
          sessionId
            ? "Type a message..."
            : "Type a message to start a new session..."
        }
      />
    </div>
  );
}
