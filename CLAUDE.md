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

47 tests across 4 suites cover the common library (base64, base32, crypto, dns_proto). CI runs on push/PR via GitHub Actions (`.github/workflows/ci.yml`), including a `clang-format` check.

## Running

```bash
# Server — on macOS port 53 works without root; on Linux it requires sudo
./build/dnsclaw-server              # try without sudo first
sudo -E ./build/dnsclaw-server      # if bind fails (-E preserves your .env config)

# Client
./build/dnsclaw                  # interactive REPL
./build/dnsclaw -m "prompt"      # one-shot
echo "prompt" | ./build/dnsclaw  # pipe mode
```

## Web UI

A Next.js web frontend in `web/` that tunnels all traffic through the DNS-CLAW server — same protocol as the CLI client, reimplemented in TypeScript. No SDKs, no direct LLM API calls.

```bash
cd web && npm install && npm run dev    # http://localhost:3000
```

### Web Architecture

The `web/lib/` directory is a TypeScript port of the C client protocol:
- **`dns.ts`** — DNS wire format builder/parser (mirrors `dns_proto.c`)
- **`base32.ts`** — RFC 4648 Base32 no-padding (mirrors `base32.c`)
- **`crypto.ts`** — AES-256-GCM + HKDF-SHA256 (mirrors `crypto.c`)
- **`transport.ts`** — UDP (`dgram`), DoH (`fetch`), DoT (`tls`) with 3-retry backoff
- **`protocol.ts`** — Full session flow: init → encrypt → base32 chunk → upload → finalize → poll → base64 decode → decrypt → JSON parse

API routes (`app/api/chat/route.ts`, `app/api/sessions/route.ts`) call `protocol.ts` and stream status events to the browser via `ReadableStream`.

Frontend uses `react-markdown` + `remark-gfm` + `shiki` for rendering. Terminal-inspired dark theme with the DNS-CLAW gradient palette.

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
2. `$SUDO_USER`'s `~/.config/dnsclaw/.env` (when running under sudo)
3. `../.env`
4. `./.env`

Environment variables override all `.env` files. Key settings: `*_API_KEY`, `TUNNEL_PSK`, `USE_DOT`/`USE_DOH`, `DNS_SERVER_ADDR`. See `.env.example` for the full reference.

When no API key is found, the server prints diagnostic output showing which `.env` paths were searched and the likely cause (missing file, sudo without `-E`, empty config). Bind failures on privileged ports suggest `sudo -E` or `SERVER_PORT` override.

## Code Style

All source files are formatted with `clang-format` (LLVM-based, 4-space indent, 100-column limit). CI enforces this — PRs with formatting violations will fail. To format locally:

```bash
find src include tests -name '*.c' -o -name '*.h' | xargs clang-format -i
```

## Security Patterns

When modifying tool execution code in `client/protocol.c`:
- **Never pass LLM-supplied strings directly to `popen()`/`system()`**. Use `shell_escape()` (defined at top of file) to wrap all parameters in safely-escaped single quotes. Use `fork()`/`execlp()` instead of `system()` when running a known program with arguments.
- **All `strncpy` calls must be followed by explicit null termination**: `buf[sizeof(buf) - 1] = '\0';`
- **Validate arithmetic on `size_t` before subtraction** to prevent underflow (e.g., check `a + b <= total` before computing `total - a - b`).
- **Guard OpenSSL EVP length casts**: `size_t` → `int` truncation on inputs > `INT_MAX` causes undefined behavior. Add `if (len > (size_t)INT_MAX) return -1;` before casting.
- **Use `OPENSSL_cleanse()` for key material**, never `memset()` (compiler can optimize it away).
- **`client_read_file` prompts for confirmation** on absolute paths and paths containing `..` to prevent silent exfiltration of sensitive files.
- **`client_fetch_url` only allows `http://` and `https://` schemes** — `file://`, `ftp://`, etc. are rejected to prevent local data exfiltration.
- **Base64/Base32 decoders validate input characters** before decoding. Invalid bytes are rejected, not silently decoded.
- **DNS UDP responses are verified** — the response query ID must match the sent query ID to prevent spoofing.
- **Session `busy` refcount**: must be incremented under `g_lock` *before* spawning the LLM thread (in `handler.c`), not inside the thread itself, to prevent the reaper from destroying the session in the gap.
- **Pending prompt chunks are cleared** after LLM processing to prevent stale data from polluting future messages.
- **LLM thread buffers are heap-allocated** (not stack) to avoid stack overflow under concurrent load.
- **DoT/DoH connections are capped** at `MAX_TLS_CLIENTS` (128) to prevent thread-exhaustion DoS.

## Dependencies

C11 compiler, CMake >= 3.14, OpenSSL 3.x, libcurl, pthreads. cJSON is fetched automatically. On macOS, CMakeLists.txt assumes Homebrew paths (`/opt/homebrew/opt/`). `mise.toml` pins cmake, ninja, and node (for the web UI).
