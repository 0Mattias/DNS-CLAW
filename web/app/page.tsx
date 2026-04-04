"use client";

import { useState, useEffect, useCallback } from "react";
import Sidebar from "@/components/Sidebar";
import Chat from "@/components/Chat";
import {
  loadConfig,
  saveConfig,
  loadConversations,
  saveConversation,
  deleteConversation,
} from "@/lib/store";
import type { ConnectionConfig, Message } from "@/lib/types";

interface Conversation {
  sessionId: string;
  messages: Message[];
  createdAt: number;
}

export default function Home() {
  const [config, setConfig] = useState<ConnectionConfig>({
    server: "127.0.0.1",
    port: 53,
    transport: "udp",
  });
  const [conversations, setConversations] = useState<Conversation[]>([]);
  const [currentSession, setCurrentSession] = useState<string | null>(null);
  const [messages, setMessages] = useState<Message[]>([]);
  const [connected, setConnected] = useState(false);

  // Load from localStorage on mount
  useEffect(() => {
    setConfig(loadConfig());
    setConversations(loadConversations());
  }, []);

  // Save config when it changes
  const handleConfigChange = useCallback((newConfig: ConnectionConfig) => {
    setConfig(newConfig);
    saveConfig(newConfig);
    setConnected(false);
  }, []);

  // Handle new chat
  const handleNewChat = useCallback(() => {
    setCurrentSession(null);
    setMessages([]);
  }, []);

  // Handle session creation (from first message)
  const handleSessionCreated = useCallback((sessionId: string) => {
    setCurrentSession(sessionId);
    setConnected(true);
  }, []);

  // Handle messages change
  const handleMessagesChange = useCallback(
    (newMessages: Message[]) => {
      setMessages(newMessages);
      if (currentSession) {
        saveConversation(currentSession, newMessages);
        setConversations(loadConversations());
      }
    },
    [currentSession],
  );

  // Handle select existing session
  const handleSelectSession = useCallback(
    (sessionId: string) => {
      setCurrentSession(sessionId);
      const conv = conversations.find((c) => c.sessionId === sessionId);
      setMessages(conv?.messages || []);
    },
    [conversations],
  );

  // Handle delete session
  const handleDeleteSession = useCallback(
    (sessionId: string) => {
      deleteConversation(sessionId);
      setConversations(loadConversations());
      if (currentSession === sessionId) {
        setCurrentSession(null);
        setMessages([]);
      }
    },
    [currentSession],
  );

  return (
    <div className="flex h-screen">
      <Sidebar
        config={config}
        onConfigChange={handleConfigChange}
        conversations={conversations}
        currentSession={currentSession}
        onNewChat={handleNewChat}
        onSelectSession={handleSelectSession}
        onDeleteSession={handleDeleteSession}
        connected={connected}
      />
      <main className="flex-1 flex flex-col min-w-0">
        <Chat
          config={config}
          sessionId={currentSession}
          messages={messages}
          onSessionCreated={handleSessionCreated}
          onMessagesChange={handleMessagesChange}
        />
      </main>
    </div>
  );
}
