/**
 * store.ts — localStorage persistence for conversations and connection config.
 */

import type { ConnectionConfig, Message } from "./types";

const CONFIG_KEY = "dnsclaw-config";
const CONVERSATIONS_KEY = "dnsclaw-conversations";

interface StoredConversation {
  sessionId: string;
  messages: Message[];
  createdAt: number;
}

export function loadConfig(): ConnectionConfig {
  if (typeof window === "undefined") {
    return { server: "127.0.0.1", port: 53, transport: "udp" };
  }
  try {
    const raw = localStorage.getItem(CONFIG_KEY);
    if (raw) return JSON.parse(raw);
  } catch {}
  return { server: "127.0.0.1", port: 53, transport: "udp" };
}

export function saveConfig(config: ConnectionConfig): void {
  if (typeof window === "undefined") return;
  localStorage.setItem(CONFIG_KEY, JSON.stringify(config));
}

export function loadConversations(): StoredConversation[] {
  if (typeof window === "undefined") return [];
  try {
    const raw = localStorage.getItem(CONVERSATIONS_KEY);
    if (raw) return JSON.parse(raw);
  } catch {}
  return [];
}

export function saveConversation(sessionId: string, messages: Message[]): void {
  if (typeof window === "undefined") return;
  const convos = loadConversations();
  const idx = convos.findIndex((c) => c.sessionId === sessionId);
  if (idx >= 0) {
    convos[idx].messages = messages;
  } else {
    convos.unshift({ sessionId, messages, createdAt: Date.now() });
  }
  // Keep last 50 conversations
  localStorage.setItem(CONVERSATIONS_KEY, JSON.stringify(convos.slice(0, 50)));
}

export function deleteConversation(sessionId: string): void {
  if (typeof window === "undefined") return;
  const convos = loadConversations().filter((c) => c.sessionId !== sessionId);
  localStorage.setItem(CONVERSATIONS_KEY, JSON.stringify(convos));
}
