"use client";

import { useState, type ChangeEvent } from "react";
import type { ConnectionConfig } from "@/lib/types";

interface ConversationEntry {
  sessionId: string;
  messages: { role: string; content: string }[];
  createdAt: number;
}

interface SidebarProps {
  config: ConnectionConfig;
  onConfigChange: (config: ConnectionConfig) => void;
  conversations: ConversationEntry[];
  currentSession: string | null;
  onNewChat: () => void;
  onSelectSession: (sessionId: string) => void;
  onDeleteSession: (sessionId: string) => void;
  connected: boolean;
}

export default function Sidebar({
  config,
  onConfigChange,
  conversations,
  currentSession,
  onNewChat,
  onSelectSession,
  onDeleteSession,
  connected,
}: SidebarProps) {
  const [showConfig, setShowConfig] = useState(false);

  const update = (field: keyof ConnectionConfig, value: string | number | boolean) => {
    onConfigChange({ ...config, [field]: value });
  };

  return (
    <div className="w-72 flex flex-col border-r border-zinc-800 bg-zinc-950 h-full">
      {/* Header */}
      <div className="p-4 border-b border-zinc-800">
        <div className="flex items-center gap-2 mb-3">
          <div
            className="text-lg font-bold tracking-tight"
            style={{
              background: "linear-gradient(135deg, rgb(255,60,50), rgb(255,195,180))",
              WebkitBackgroundClip: "text",
              WebkitTextFillColor: "transparent",
            }}
          >
            DNS-CLAW
          </div>
          <span className="text-xs text-zinc-600 font-mono">web</span>
        </div>

        <button
          onClick={onNewChat}
          className="w-full py-2 rounded-lg border border-zinc-700 text-zinc-300 text-sm hover:border-[rgb(255,60,50)]/50 hover:text-zinc-100 transition-colors"
        >
          + New Chat
        </button>
      </div>

      {/* Connection config */}
      <div className="border-b border-zinc-800">
        <button
          onClick={() => setShowConfig(!showConfig)}
          className="w-full flex items-center justify-between px-4 py-2 text-xs text-zinc-500 hover:text-zinc-300 transition-colors"
        >
          <span className="flex items-center gap-2">
            <span
              className={`w-2 h-2 rounded-full ${connected ? "bg-green-500" : "bg-zinc-600"}`}
            />
            {config.server}:{config.port} ({config.transport})
          </span>
          <span>{showConfig ? "\u25B2" : "\u25BC"}</span>
        </button>

        {showConfig && (
          <div className="px-4 pb-3 space-y-2">
            <div>
              <label className="text-xs text-zinc-600 block mb-0.5">
                Server
              </label>
              <input
                type="text"
                value={config.server}
                onChange={(e: ChangeEvent<HTMLInputElement>) =>
                  update("server", e.target.value)
                }
                className="w-full bg-zinc-900 border border-zinc-800 rounded px-2 py-1 text-xs text-zinc-300 font-mono focus:border-[rgb(255,60,50)]/50 outline-none"
              />
            </div>
            <div className="flex gap-2">
              <div className="flex-1">
                <label className="text-xs text-zinc-600 block mb-0.5">
                  Port
                </label>
                <input
                  type="number"
                  value={config.port}
                  onChange={(e: ChangeEvent<HTMLInputElement>) =>
                    update("port", parseInt(e.target.value) || 53)
                  }
                  className="w-full bg-zinc-900 border border-zinc-800 rounded px-2 py-1 text-xs text-zinc-300 font-mono focus:border-[rgb(255,60,50)]/50 outline-none"
                />
              </div>
              <div className="flex-1">
                <label className="text-xs text-zinc-600 block mb-0.5">
                  Transport
                </label>
                <select
                  value={config.transport}
                  onChange={(e: ChangeEvent<HTMLSelectElement>) =>
                    update("transport", e.target.value)
                  }
                  className="w-full bg-zinc-900 border border-zinc-800 rounded px-2 py-1 text-xs text-zinc-300 font-mono focus:border-[rgb(255,60,50)]/50 outline-none"
                >
                  <option value="udp">UDP</option>
                  <option value="doh">DoH</option>
                  <option value="dot">DoT</option>
                </select>
              </div>
            </div>
            <div>
              <label className="text-xs text-zinc-600 block mb-0.5">
                PSK (encryption key)
              </label>
              <input
                type="password"
                value={config.psk || ""}
                onChange={(e: ChangeEvent<HTMLInputElement>) =>
                  update("psk", e.target.value)
                }
                placeholder="optional"
                className="w-full bg-zinc-900 border border-zinc-800 rounded px-2 py-1 text-xs text-zinc-300 font-mono focus:border-[rgb(255,60,50)]/50 outline-none"
              />
            </div>
            <div>
              <label className="text-xs text-zinc-600 block mb-0.5">
                Auth Token
              </label>
              <input
                type="password"
                value={config.authToken || ""}
                onChange={(e: ChangeEvent<HTMLInputElement>) =>
                  update("authToken", e.target.value)
                }
                placeholder="optional"
                className="w-full bg-zinc-900 border border-zinc-800 rounded px-2 py-1 text-xs text-zinc-300 font-mono focus:border-[rgb(255,60,50)]/50 outline-none"
              />
            </div>
            {(config.transport === "doh" || config.transport === "dot") && (
              <label className="flex items-center gap-2 text-xs text-zinc-500">
                <input
                  type="checkbox"
                  checked={config.insecure || false}
                  onChange={(e: ChangeEvent<HTMLInputElement>) =>
                    update("insecure", e.target.checked)
                  }
                  className="accent-[rgb(255,60,50)]"
                />
                Skip TLS verify (self-signed)
              </label>
            )}
          </div>
        )}
      </div>

      {/* Conversation list */}
      <div className="flex-1 overflow-y-auto">
        {conversations.length === 0 ? (
          <p className="px-4 py-6 text-xs text-zinc-600 text-center">
            No conversations yet
          </p>
        ) : (
          conversations.map((conv) => (
            <div
              key={conv.sessionId}
              className={`group flex items-center px-4 py-2.5 cursor-pointer border-b border-zinc-900 hover:bg-zinc-900/50 transition-colors ${
                currentSession === conv.sessionId
                  ? "bg-zinc-900 border-l-2 border-l-[rgb(255,60,50)]"
                  : ""
              }`}
              onClick={() => onSelectSession(conv.sessionId)}
            >
              <div className="flex-1 min-w-0">
                <p className="text-xs text-zinc-400 font-mono truncate">
                  {conv.messages[0]?.content.substring(0, 40) || "Empty chat"}
                </p>
                <p className="text-[10px] text-zinc-600 mt-0.5">
                  {new Date(conv.createdAt).toLocaleDateString()} &middot;{" "}
                  {conv.sessionId.substring(0, 8)}
                </p>
              </div>
              <button
                onClick={(e) => {
                  e.stopPropagation();
                  onDeleteSession(conv.sessionId);
                }}
                className="opacity-0 group-hover:opacity-100 text-zinc-600 hover:text-red-400 text-xs ml-2 transition-opacity"
              >
                x
              </button>
            </div>
          ))
        )}
      </div>

      {/* Footer */}
      <div className="px-4 py-2 border-t border-zinc-800">
        <p className="text-[10px] text-zinc-700 text-center">
          Tunneled through DNS
        </p>
      </div>
    </div>
  );
}
