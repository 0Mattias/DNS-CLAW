# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

DNS-CLAW (C Language Agentic Wireformat) — an agentic AI CLI tunneled through DNS, written in pure C11. The client sends LLM prompts as encrypted Base32-encoded DNS TXT queries; the server decodes, calls the LLM API, and returns chunked encrypted TXT responses. Supports Gemini, OpenAI, Claude (Anthropic), and OpenRouter.

## Build Commands

```bash
# Configure + build (Release)
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build

# Debug build (ASan + UBSan)
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build

# Clean rebuild
cmake --build build --target clean && cmake --build build

# Install system-wide
sudo cmake --install build
```

## Testing

### C tests (56 tests, 5 suites)

```bash
ctest --test-dir build --output-on-failure

# Single suite
./build/test_base64
./build/test_base32
./build/test_crypto
./build/test_dns_proto
./build/test_integration

# ThreadSanitizer build
cmake -B build -DCMAKE_BUILD_TYPE=TSan
cmake --build build && ctest --test-dir build --output-on-failure
```

Suites: base64, base32, crypto (AES-256-GCM), dns_proto (wire format), integration (full handler pipeline with session/upload/download/auth).

### Web tests (54 tests, 5 suites)

```bash
cd web && npm test

# Watch mode
npm run test:watch
```

Suites mirror the C tests: base32, crypto, dns, protocol, transport. Same test vectors ensure TypeScript port compatibility.

### Web checks

```bash
cd web
npm run typecheck    # tsc --noEmit
npm run lint         # eslint
npm run build        # next build
```

## Running

```bash
# Server — macOS port 53 works without root; Linux requires sudo
./build/dnsclaw-server              # try without sudo first
sudo -E ./build/dnsclaw-server      # if bind fails (-E preserves .env config)

# Client
./build/dnsclaw                  # interactive REPL
./build/dnsclaw -m "prompt"      # one-shot
echo "prompt" | ./build/dnsclaw  # pipe mode

# Web UI
cd web && npm install && npm run dev    # http://localhost:3000
```

## Architecture

Three CMake targets:

- **`dnsclaw_common`** (static lib) — AES-256-GCM crypto, DNS wire format, Base32/Base64 codecs, .env config loader.
- **`dnsclaw-server`** (executable) — DNS server (UDP/DoT/DoH), reassembles chunked messages, calls LLM provider API, returns TXT records. Session reaper thread.
- **`dnsclaw`** (executable) — Terminal client with REPL, ANSI markdown rendering, spinner, 7-tool agent executor.

A fourth target, **`dnsclaw_server_lib`**, is an internal static library containing the server source files (handler, llm, session, transport, log). It exists so integration tests can link against server internals without building a full executable.

### Data Flow

Client encrypts (AES-256-GCM) -> Base32 encodes -> DNS TXT query labels -> Server decodes -> decrypts -> calls LLM API -> encrypts response -> Base64 encodes -> chunked TXT records -> Client polls/decodes/decrypts -> renders markdown.

### Key Design Points

- **Protocol is stateful**: sessions, multi-turn conversations, and tool call chains persist across DNS queries. Session state lives server-side (`session.c`), reaped after 30 min idle.
- **Tool execution is client-side**: the LLM returns tool calls in its response, the server passes them through, and `client/protocol.c` executes them locally. Safe tools auto-approve; destructive ones prompt the user.
- **Multi-provider LLM**: `server/llm.c` handles all 4 providers with provider-specific JSON formatting. Provider is auto-detected from which API key is set.
- **Transport abstraction**: `server/transport.c` handles UDP, DoT, and DoH listeners. `common/dns_proto.c` handles DNS wire format and client-side transport.
- **No external SDK deps**: only libc, libcurl, OpenSSL, cJSON (auto-fetched), and pthreads.

### Web Architecture

The `web/lib/` directory is a TypeScript port of the C client protocol:
- **`dns.ts`** — DNS wire format builder/parser (mirrors `dns_proto.c`)
- **`base32.ts`** — RFC 4648 Base32 no-padding (mirrors `base32.c`)
- **`crypto.ts`** — AES-256-GCM + HKDF-SHA256 (mirrors `crypto.c`)
- **`transport.ts`** — UDP (`dgram`), DoH (`fetch`), DoT (`tls`) with 3-retry backoff
- **`protocol.ts`** — Full session flow: init -> encrypt -> base32 chunk -> upload -> finalize -> poll -> base64 decode -> decrypt -> JSON parse

API routes (`app/api/chat/route.ts`, `app/api/sessions/route.ts`) call `protocol.ts` and stream status events to the browser via `ReadableStream`.

Frontend uses `react-markdown` + `remark-gfm` + `shiki` for rendering.

## Configuration

Config loaded from (first match wins per variable):
1. `~/.config/dnsclaw/.env`
2. `$SUDO_USER`'s `~/.config/dnsclaw/.env` (when running under sudo)
3. `../.env`
4. `./.env`

Environment variables override all `.env` files. Key settings: `*_API_KEY`, `TUNNEL_PSK`, `USE_DOT`/`USE_DOH`, `DNS_SERVER_ADDR`, `AUTH_TOKEN`. See `.env.example` for the full reference.

When no API key is found, the server prints diagnostic output showing which `.env` paths were searched and the likely cause (missing file, sudo without `-E`, empty config). Bind failures on privileged ports suggest `sudo -E` or `SERVER_PORT` override.

## CI

GitHub Actions (`.github/workflows/ci.yml`) runs on push/PR with 7 parallel jobs:

| Job | What it checks |
|---|---|
| `format-check` | `clang-format` on C source |
| `build-and-test` | CMake + CTest (Ubuntu + macOS x Release + Debug) |
| `thread-sanitizer` | TSan build + tests |
| `web-typecheck` | `tsc --noEmit` |
| `web-lint` | ESLint (Next.js + TypeScript) |
| `web-test` | Vitest (54 unit tests) |
| `web-build` | Next.js production build |

## Code Style

**C**: All source files formatted with `clang-format` (LLVM-based, 4-space indent, 100-column limit). CI enforces this.

```bash
find src include tests -name '*.c' -o -name '*.h' | xargs clang-format -i
```

**Web**: ESLint with `eslint-config-next` (includes `@typescript-eslint`). Strict TypeScript.

```bash
cd web && npm run lint
```

## Security Patterns

When modifying tool execution code in `client/protocol.c`:
- **Never pass LLM-supplied strings directly to `popen()`/`system()`**. Use `shell_escape()` (defined at top of file) to wrap all parameters in safely-escaped single quotes. Use `fork()`/`execlp()` instead of `system()` when running a known program with arguments.
- **All `strncpy` calls must be followed by explicit null termination**: `buf[sizeof(buf) - 1] = '\0';`
- **Validate arithmetic on `size_t` before subtraction** to prevent underflow (e.g., check `a + b <= total` before computing `total - a - b`).
- **Guard OpenSSL EVP length casts**: `size_t` -> `int` truncation on inputs > `INT_MAX` causes undefined behavior. Add `if (len > (size_t)INT_MAX) return -1;` before casting.
- **Use `OPENSSL_cleanse()` for key material**, never `memset()` (compiler can optimize it away).
- **`client_read_file` prompts for confirmation** on absolute paths and paths containing `..` to prevent silent exfiltration of sensitive files.
- **`client_fetch_url` only allows `http://` and `https://` schemes** — `file://`, `ftp://`, etc. are rejected to prevent local data exfiltration.
- **Base64/Base32 decoders validate input characters** before decoding. Invalid bytes are rejected, not silently decoded.
- **DNS UDP responses are verified** — the response query ID must match the sent query ID to prevent spoofing.
- **Session `busy` refcount**: must be incremented under `g_lock` *before* spawning the LLM thread (in `handler.c`), not inside the thread itself, to prevent the reaper from destroying the session in the gap.
- **Pending prompt chunks are cleared** after LLM processing to prevent stale data from polluting future messages.
- **LLM thread buffers are heap-allocated** (not stack) to avoid stack overflow under concurrent load.
- **DoT/DoH connections are capped** at `MAX_TLS_CLIENTS` (128) to prevent thread-exhaustion DoS.
- **cJSON global hooks** must be initialized on the main thread before any concurrent usage (see `test_integration.c`) to avoid a data race on the lazy-init global.

## Dependencies

C11 compiler, CMake >= 3.14, OpenSSL 3.x, libcurl, pthreads. cJSON is fetched automatically. On macOS, CMakeLists.txt assumes Homebrew paths (`/opt/homebrew/opt/`). `mise.toml` pins cmake, ninja, and node (for the web UI).

Web UI: Node.js, npm. Dependencies installed via `npm install` in `web/`.
