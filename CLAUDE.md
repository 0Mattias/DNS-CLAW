# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

DNS-CLAW (C Language Agentic Wireformat) — an agentic AI CLI tunneled through DNS, written in pure C11. The client sends LLM prompts as encrypted Base32-encoded DNS TXT queries; the server decodes, calls the LLM API, and returns chunked encrypted TXT responses. Supports Gemini, OpenAI, Claude (Anthropic), and OpenRouter.

## Build Commands

```bash
# Configure (first time, or after CMakeLists.txt changes)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build

# Debug build (enables ASan + UBSan)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Clean rebuild
cmake --build build --target clean && cmake --build build

# Install system-wide
sudo cmake --install build
```

## Testing

```bash
# Run all tests
ctest --test-dir build --output-on-failure

# Run a single test
./build/test_base64
./build/test_base32
./build/test_crypto
./build/test_dns_proto

# ThreadSanitizer build
cmake -B build -DCMAKE_BUILD_TYPE=TSan
cmake --build build && ctest --test-dir build --output-on-failure
```

Tests cover the common library (base64, base32, crypto, dns_proto). CI runs on push/PR via GitHub Actions (`.github/workflows/ci.yml`).

## Running

```bash
# Server (port 53 requires root; -E preserves env vars for API keys)
sudo -E ./build/dnsclaw-server

# Client
./build/dnsclaw                  # interactive REPL
./build/dnsclaw -m "prompt"      # one-shot
echo "prompt" | ./build/dnsclaw  # pipe mode
```

## Architecture

Three CMake targets:

- **`dnsclaw_common`** (static lib) — shared code: AES-256-GCM crypto, DNS wire format, Base32/Base64 codecs, .env config loader. Used by both client and server.
- **`dnsclaw-server`** (executable) — DNS server that listens on UDP/DoT/DoH, reassembles chunked messages, calls the configured LLM provider API, and returns responses as TXT records. Manages sessions with a reaper thread.
- **`dnsclaw`** (executable) — Terminal client with REPL, ANSI markdown rendering, spinner, and a 7-tool agent executor (bash, read/write/edit file, list dir, search files, fetch URL).

### Data Flow

Client encrypts (AES-256-GCM) → Base32 encodes → DNS TXT query labels → Server decodes → decrypts → calls LLM API → encrypts response → Base64 encodes → chunked TXT records → Client polls/decodes/decrypts → renders markdown.

### Key Design Points

- **Protocol is stateful**: sessions, multi-turn conversations, and tool call chains persist across DNS queries. Session state lives server-side (`session.c`), reaped after 30 min idle.
- **Tool execution is client-side**: the LLM returns tool calls in its response, the server passes them through, and `client/protocol.c` executes them locally. Safe tools auto-approve; destructive ones prompt the user.
- **Multi-provider LLM**: `server/llm.c` handles all 4 providers (Gemini, OpenAI, Anthropic, OpenRouter) with provider-specific JSON request/response formatting. Provider is auto-detected from which API key is set.
- **Transport abstraction**: `server/transport.c` handles UDP, DoT, and DoH listeners. `common/dns_proto.c` handles the DNS wire format and client-side transport.
- **No external SDK deps**: only libc, libcurl, OpenSSL, cJSON (auto-fetched), and pthreads.

## Configuration

Config loaded from (first match wins per variable):
1. `~/.config/dnsclaw/.env`
2. `./.env`
3. `../.env`

Environment variables override all `.env` files. Key settings: `*_API_KEY`, `TUNNEL_PSK`, `USE_DOT`/`USE_DOH`, `DNS_SERVER_ADDR`. See `.env.example` for the full reference.

## Dependencies

C11 compiler, CMake >= 3.14, OpenSSL 3.x, libcurl, pthreads. cJSON is fetched automatically. On macOS, CMakeLists.txt assumes Homebrew paths (`/opt/homebrew/opt/`). `mise.toml` pins cmake and ninja.
