export interface ConnectionConfig {
  server: string;
  port: number;
  transport: "udp" | "doh" | "dot";
  psk?: string;
  authToken?: string;
  insecure?: boolean;
}

export interface Message {
  id: string;
  role: "user" | "assistant";
  content: string;
  toolCall?: {
    name: string;
    args: Record<string, string>;
  };
  timestamp: number;
}

export interface Session {
  id: string;
  date: number;
  active?: boolean;
  messages: Message[];
}

export interface StatusEvent {
  status: "connecting" | "uploading" | "processing" | "downloading" | "done" | "error";
  chunk?: number;
  total?: number;
  response?: LLMResponse;
  error?: string;
}

export interface LLMResponse {
  type: "text" | "tool_call";
  content?: string;
  tool_name?: string;
  tool_args?: Record<string, string>;
  provider?: string;
}
