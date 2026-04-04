# DNS-CLAW

[![CI](https://github.com/0Mattias/DNS-CLAW/actions/workflows/ci.yml/badge.svg)](https://github.com/0Mattias/DNS-CLAW/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**C Language Agentic Wireformat** — an agentic AI CLI tunneled through DNS, written in pure C.

Talk to LLMs through DNS queries. The client encodes your messages as Base32 TXT record lookups, the server decodes them, calls your LLM provider, and sends responses back as chunked TXT records. Fully agentic — the model can run commands, read/write/edit files, search codebases, and fetch URLs on your machine.

Supports **Gemini**, **OpenAI**, **Claude (Anthropic)**, and **OpenRouter**.

<img width="798" height="514" alt="dnsclaw" src="https://github.com/user-attachments/assets/b62223c1-07f8-4500-b54d-4b284feecee4" />

## Table of Contents

- [Why DNS?](#why-dns)
- [Quick Start](#quick-start)
- [Web UI](#web-ui)
- [How It Works](#how-it-works)
- [Usage](#usage)
  - [Client](#client)
  - [Server](#server)
  - [Config Subcommand](#config-subcommand)
  - [In-Session Commands](#in-session-commands)
- [Agent Tools](#agent-tools)
- [Configuration](#configuration)
- [Security](#security)
  - [Payload Encryption](#payload-encryption)
  - [Transport Modes](#transport-modes)
  - [Client Authentication](#client-authentication)
- [Session Persistence](#session-persistence)
- [Docker](#docker)
- [Development](#development)
  - [Prerequisites](#prerequisites)
  - [Building](#building)
  - [Testing](#testing)
  - [CI](#ci)
  - [Code Style](#code-style)
- [Project Structure](#project-structure)
- [Troubleshooting](#troubleshooting) — [Server](#server-wont-start) | [Client](#client-cant-connect) | [Encryption](#encryption-errors) | [LLM API](#llm-api-errors) | [Sessions](#session-issues) | [Build](#build--setup-issues) | [Web UI](#web-ui-issues)
- [License](#license)

## Why DNS?

DNS is the one protocol that works almost everywhere — through captive portals, hotel WiFi, corporate firewalls, and restricted networks where direct HTTPS to AI APIs is blocked.

- **Works on restricted networks** — DNS traffic (port 53) is almost never blocked
- **Invisible to DPI** — AES-256-GCM payload encryption makes queries look like random noise
- **Zero cloud dependencies** — run your own server, keep your data

## Quick Start

```bash
git clone https://github.com/0Mattias/DNS-CLAW.git
cd DNS-CLAW
./setup.sh
```

The setup script checks dependencies, asks for your LLM provider and API key, generates encryption and auth keys, and builds everything. Re-run safely anytime — it skips provider setup if a key already exists (use `--reconfigure` to force).

Then start the server and client in two terminals:

```bash
# Terminal 1 — server
./build/dnsclaw-server              # macOS (no root needed)
sudo -E ./build/dnsclaw-server      # Linux (port 53 needs root; -E preserves .env)

# Terminal 2 — chat
./build/dnsclaw
```

Type a message. You should see the LLM response rendered in your terminal within a few seconds.

> **Install system-wide (optional):** `sudo cmake --install build` — then use `dnsclaw-server` and `dnsclaw` from anywhere.

## Web UI

<img width="579" height="338" alt="web-ui" src="https://github.com/user-attachments/assets/eef614aa-e07d-4c30-a744-3611622cbe4b" />

A Next.js frontend that tunnels all traffic through DNS — same protocol, same encryption as the CLI.

```bash
cd web && npm install && npm run dev    # http://localhost:3000
```

Point it at your running server from the sidebar (address, port, transport, PSK, auth token). Every message goes through DNS queries — the web UI is a TypeScript reimplementation of the C client protocol, not a direct API wrapper.

Or run both with Docker:

```bash
docker compose up    # server on port 53, web UI on port 3000
```

## How It Works

```
┌─────────────────┐                          ┌─────────────────┐               ┌─────────┐
│  dnsclaw (CLI)  │───DNS TXT queries───────>│  dnsclaw-server │──── HTTPS ───>│ LLM API │
└─────────────────┘   UDP / DoT / DoH        │                 │   REST API    │         │
┌─────────────────┐                          │                 │               │         │
│  Web UI (Next)  │───DNS TXT queries───────>│                 │               │         │
└─────────────────┘   via Node.js backend    └─────────────────┘               └─────────┘
```

1. Client encrypts your message (AES-256-GCM) and Base32 encodes it into DNS TXT query labels
2. Server receives the query, decodes, decrypts, and calls your LLM provider
3. LLM response is encrypted, Base64 encoded, and returned as chunked TXT records
4. Client polls for chunks, decodes, decrypts, and renders markdown to the terminal

The protocol is stateful — sessions, multi-turn conversations, and tool call chains all persist across DNS queries.

## Usage

### Client

```bash
dnsclaw                          # interactive REPL
dnsclaw -m "what is 2+2"         # one-shot mode (print and exit)
echo "explain this" | dnsclaw    # pipe mode
```

| Flag | Description |
|---|---|
| `-m <msg>` | One-shot message |
| `-s <addr>` | Server address (overrides .env) |
| `-p <port>` | Server port (overrides .env) |
| `--no-color` | Disable ANSI colors |
| `--no-typewriter` | Disable typewriter effect |
| `-h, --help` | Show usage |
| `-v` | Print version |

### Server

```bash
./build/dnsclaw-server              # macOS (no root needed)
sudo -E ./build/dnsclaw-server      # Linux (port 53 needs root)
```

If no API key is configured, the server prints diagnostic output showing which `.env` paths were searched and the likely cause. If a privileged port bind fails, it suggests `sudo -E` or setting `SERVER_PORT`.

### Config Subcommand

```bash
dnsclaw config                                     # show current config
dnsclaw config --edit                              # open in $EDITOR
dnsclaw config --set ANTHROPIC_MODEL=claude-sonnet-4-6  # set a value
dnsclaw config --provider                          # interactive provider setup
```

### In-Session Commands

| Command | Description |
|---|---|
| `/help` | Show available commands |
| `/clear` | Start a new session |
| `/compact [focus]` | Compress conversation context |
| `/export [file]` | Export conversation to markdown |
| `/sessions` | List saved sessions |
| `/resume <id>` | Resume a saved session |
| `/config` | Show current configuration |
| `/status` | Show session, transport, encryption info |
| `/exit` | Quit |

## Agent Tools

The LLM has access to 7 tools for interacting with the client's machine:

| Tool | Description | Approval |
|---|---|---|
| `client_execute_bash` | Run any shell command | Requires approval |
| `client_read_file` | Read file contents | Prompts for absolute/`..` paths |
| `client_write_file` | Create/overwrite a file | Requires approval |
| `client_edit_file` | Surgical find-and-replace | Requires approval |
| `client_list_directory` | List directory contents | Auto-approved |
| `client_search_files` | Recursive grep | Auto-approved |
| `client_fetch_url` | HTTP GET (http/https only) | Auto-approved |

Safe read-only tools run automatically. Destructive tools prompt for `[Y/n]` confirmation.

## Configuration

Config is loaded from (first match wins per variable):

1. `~/.config/dnsclaw/.env` — user config (created by `setup.sh`)
2. `$SUDO_USER`'s `~/.config/dnsclaw/.env` — when running under sudo
3. `../.env` — parent directory (legacy)
4. `./.env` — project-local (for development)

Environment variables override all `.env` files. The easiest way to manage settings is `dnsclaw config`.

<details>
<summary><b>Full config reference</b></summary>

```bash
# ── LLM Provider ────────────────────────────────────────────
# Set one API key — the server auto-detects the provider.

GEMINI_API_KEY="..."         # https://aistudio.google.com/apikey
GEMINI_MODEL="gemini-3.1-pro-preview"

OPENAI_API_KEY="sk-..."     # https://platform.openai.com/api-keys
OPENAI_MODEL="gpt-5.4"

ANTHROPIC_API_KEY="sk-ant-..." # https://console.anthropic.com/settings/keys
ANTHROPIC_MODEL="claude-sonnet-4-6"

OPENROUTER_API_KEY="sk-or-..." # https://openrouter.ai/keys
OPENROUTER_MODEL="openrouter/auto"

# ── Encryption ──────────────────────────────────────────────
TUNNEL_PSK="..."             # openssl rand -base64 32

# ── Transport ───────────────────────────────────────────────
USE_DOT=false                # DNS-over-TLS (port 853)
USE_DOH=false                # DNS-over-HTTPS (port 443)
DNS_SERVER_ADDR=127.0.0.1
# SERVER_PORT=53             # auto-detected from transport

# ── TLS (DoT/DoH only) ─────────────────────────────────────
TLS_CERT=cert.pem
TLS_KEY=key.pem
INSECURE_SKIP_VERIFY=true

# ── Authentication ──────────────────────────────────────────
AUTH_TOKEN="..."             # openssl rand -hex 16

# ── Session Persistence ────────────────────────────────────
SESSION_PERSIST=true

# ── Custom System Prompt ───────────────────────────────────
# SYSTEM_PROMPT="You are a helpful assistant."
```

</details>

## Security

### Payload Encryption

DNS-CLAW uses **AES-256-GCM** with a pre-shared key. Instead of encrypting the connection (which changes packet shape and alerts firewalls), we encrypt the data itself before it touches DNS.

```
Your prompt: "What is my IP?"
      | AES-256-GCM encrypt (TUNNEL_PSK)
Ciphertext: [magic:0xCE01][nonce:12B][encrypted][tag:16B]
      | Base32 encode
DNS query: gizqwerty4abc.0.up.3.a1b2c3.llm.local.

Firewall sees: standard DNS query
Snooper sees:  random noise in labels
Actual data:   AES-256 encrypted
```

| Property | Value |
|---|---|
| Cipher | AES-256-GCM (authenticated encryption) |
| Key derivation | HKDF-SHA256 (salt: `dns-claw-v1`, info: `tunnel-key`) |
| Nonce | 12 random bytes per message |
| Auth tag | 16 bytes (GCM) |
| Wire overhead | 30 bytes per message |
| Magic header | `0xCE 0x01` |

Encryption is optional — omit `TUNNEL_PSK` to run unencrypted. When enabled, it applies to all transport modes as an additional layer.

### Transport Modes

| Mode | Config | Default Port | Encryption |
|---|---|---|---|
| **UDP** (default) | Both `false` | 53 | PSK only |
| **DoT** | `USE_DOT=true` | 853 | TLS + PSK |
| **DoH** | `USE_DOH=true` | 443 | HTTPS + PSK |

For DoH, set `DNS_SERVER_ADDR=https://127.0.0.1/dns-query` (full URL). For UDP/DoT, use `127.0.0.1`.

TLS certs are generated automatically by `./setup.sh --advanced` when you select a TLS transport.

> **Note:** Port 53/443/853 require root on Linux. Use `sudo -E` or set `SERVER_PORT` to a high port. macOS does not require root.

### Client Authentication

When `AUTH_TOKEN` is set, the server rejects clients without the matching token. `setup.sh` generates one automatically.

```bash
# Generate manually
openssl rand -hex 16
# Add to ~/.config/dnsclaw/.env (same on client AND server)
AUTH_TOKEN="your-token-here"
```

## Session Persistence

Conversations save to `~/.config/dnsclaw/sessions/` automatically. Resume from the client:

```bash
/sessions           # list saved sessions
/resume a3f7b2e9    # resume by ID (prefix match works)
```

Disable with `SESSION_PERSIST=false`.

## Docker

```bash
# Standalone
docker build -t dnsclaw .
docker run -p 53:53/udp \
  -e ANTHROPIC_API_KEY=sk-ant-... \
  -e TUNNEL_PSK=... \
  -e AUTH_TOKEN=... \
  dnsclaw

# With web UI
docker compose up
```

The compose file mounts a volume for persistent sessions. For DoT/DoH, uncomment the certificate volume mounts in `docker-compose.yml`.

## Development

### Prerequisites

<details>
<summary><b>macOS</b></summary>

```bash
xcode-select --install            # C compiler
brew install cmake openssl curl   # build deps
```

libedit (for command history) is pre-installed.
</details>

<details>
<summary><b>Ubuntu / Debian</b></summary>

```bash
sudo apt-get update
sudo apt-get install build-essential cmake libssl-dev libcurl4-openssl-dev
```

Optional: `sudo apt-get install libedit-dev` for command history.
</details>

### Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Build types: `Release` (hardened), `Debug` (ASan + UBSan), `TSan` (ThreadSanitizer).

### Testing

**C tests** (56 tests across 5 suites):

```bash
ctest --test-dir build --output-on-failure
```

Covers base64, base32, AES-256-GCM crypto, DNS wire format, and integration tests (session init, upload/finalize/download, encrypted payloads, auth tokens, malformed queries).

**Web tests** (54 tests across 5 suites):

```bash
cd web && npm test
```

Covers base32, crypto, dns, protocol, and transport — mirroring the C test vectors to ensure the TypeScript port stays compatible.

### CI

GitHub Actions runs on every push and PR ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)):

| Job | What it checks |
|---|---|
| `format-check` | `clang-format` on all C source |
| `build-and-test` | CMake build + CTest (Ubuntu + macOS x Release + Debug) |
| `thread-sanitizer` | TSan build + tests |
| `web-typecheck` | TypeScript strict type checking |
| `web-lint` | ESLint (Next.js + TypeScript rules) |
| `web-test` | Vitest unit tests |
| `web-build` | Next.js production build |

### Code Style

`.clang-format` config: LLVM-based, 4-space indent, 100-column limit. CI enforces formatting:

```bash
find src include tests -name '*.c' -o -name '*.h' | xargs clang-format -i
```

## Project Structure

```
src/
  client/              CLI client (REPL, one-shot, pipe modes)
    main.c             Entry point, arg parsing, REPL loop
    protocol.c         DNS query wrapper, message processing, 7-tool executor
    render.c           ANSI markdown renderer
    spinner.c          Async activity spinner
    ui.c               Banner, help text, config subcommand
  server/              DNS server + LLM integration
    main.c             Entry point, provider detection
    handler.c          DNS query dispatcher (init/upload/fin/download)
    llm.c              Multi-provider API calls + response parsing
    transport.c        UDP, DoT, DoH listeners
    session.c          Session lifecycle + reaper thread
    log.c              Colored logging
  common/              Shared library (client + server)
    crypto.c           AES-256-GCM + HKDF key derivation
    dns_proto.c        DNS wire format + client transport
    base32.c           RFC 4648 Base32 (client -> server via labels)
    base64.c           RFC 4648 Base64 (server -> client via TXT)
    config.c           .env file loader
tests/                 C unit + integration tests (CTest)
web/                   Next.js web frontend
  lib/                 TypeScript DNS-CLAW client protocol
    dns.ts             DNS wire format builder/parser
    base32.ts          RFC 4648 Base32 codec
    crypto.ts          AES-256-GCM + HKDF-SHA256
    transport.ts       UDP/DoH/DoT transport
    protocol.ts        Session protocol (init/upload/poll/decrypt)
  lib/__tests__/       Vitest unit tests (mirrors C test vectors)
  app/                 Next.js pages and API routes
  components/          React components (Chat, Markdown, CodeBlock)
include/               C header files
.github/workflows/     CI (GitHub Actions)
CMakeLists.txt         Build system (auto-fetches cJSON)
setup.sh               First-run setup script
.env.example           Configuration template
```

## Troubleshooting

### Server won't start

| Error | Cause | Fix |
|---|---|---|
| `FATAL: No API key configured` | No LLM provider key in `.env` or environment | Run `./setup.sh` or `dnsclaw config --provider`. The error output lists every `.env` path that was searched |
| `FATAL: LLM_PROVIDER=X but KEY is not set` | `LLM_PROVIDER` is set but the matching API key isn't | Set the correct key (e.g., `ANTHROPIC_API_KEY` for `anthropic`) |
| `FATAL: Unknown LLM_PROVIDER` | Typo in `LLM_PROVIDER` | Use one of: `gemini`, `openai`, `anthropic`, `openrouter` |
| `bind: Permission denied` | Port 53/443/853 requires root on Linux | `sudo -E ./build/dnsclaw-server` (the `-E` preserves your `.env`), or set `SERVER_PORT=8053` for an unprivileged port |
| `bind: Address already in use` | Another process (e.g., systemd-resolved) is on the port | Find it with `sudo lsof -i :53`, then stop it or use a different port |
| Hint: "Running as root without `sudo -E`" | Your `.env` is in your user home but root can't see it | Always use `sudo -E` so environment variables carry through |
| Hint: ".env found but contains no API key" | Config file exists but keys are commented out or empty | Edit `~/.config/dnsclaw/.env` and uncomment/fill in one provider key |
| `Failed to load TLS certs` | Certificate or key file missing/invalid for DoT/DoH | Check `TLS_CERT` and `TLS_KEY` paths. Generate with: `openssl req -x509 -nodes -days 365 -newkey rsa:2048 -keyout key.pem -out cert.pem -subj "/CN=llm.local"` |
| `SSL_CTX_new failed` | OpenSSL initialization failure | Verify OpenSSL 3.x is installed: `openssl version` |

### Client can't connect

| Error | Cause | Fix |
|---|---|---|
| `Failed to initialize session` | Client can't reach the server | 1) Check server is running. 2) Verify `DNS_SERVER_ADDR` and port match. 3) Check firewall allows DNS traffic |
| `UDP timeout` / query hangs | No response from server within 5 seconds | Server may not be listening, port mismatch, or firewall blocking UDP |
| `DNS response ID mismatch` | Response doesn't match the query (anti-spoofing check) | Network issue or middlebox interference; retry or switch transport |
| DoH connection refused | Wrong address format | Use full URL with path: `DNS_SERVER_ADDR=https://127.0.0.1/dns-query` |
| `DoT timeout` | TLS handshake or response timeout | Check server is listening on DoT port (default 853), certs are valid, and `INSECURE_SKIP_VERIFY=true` if using self-signed certs |

### Encryption errors

| Error | Cause | Fix |
|---|---|---|
| `Decryption failed — PSK mismatch or corrupted data` | `TUNNEL_PSK` differs between client and server | Copy the exact same key to both. Regenerate with `openssl rand -base64 32` |
| `Invalid magic bytes` | One side has encryption enabled, the other doesn't | Either set `TUNNEL_PSK` on both, or remove it from both |
| `Encryption failed` | Crypto initialization issue | Check `TUNNEL_PSK` is set and OpenSSL is working |

### LLM API errors

| Server log | Cause | Fix |
|---|---|---|
| `HTTP 401` / `invalid_api_key` | API key is wrong or revoked | Verify your key at the provider's dashboard; run `dnsclaw config --provider` to re-enter |
| `HTTP 429` (rate limit) | Too many requests to the provider | Wait and retry; the server retries automatically with backoff |
| `HTTP 5xx` (server error) | Provider is having issues | Check provider status page; the server retries up to 3 times |
| `All N attempts failed` | Every retry failed | Check network, provider status, and API key validity |
| `API call failed` | Generic failure after retries | Check the preceding `HTTP` or `curl error` line in logs for details |
| `curl error: ...` | Network/DNS/SSL issue in the outbound HTTPS call | Check internet connectivity from the server; verify DNS resolution works |
| `JSON parse failed` | LLM returned non-JSON response | Unusual; check provider status or if the model name is valid |

### Session issues

| Error | Cause | Fix |
|---|---|---|
| `ERR:NOSESSION` | Session expired or doesn't exist | Sessions are reaped after 30 min idle. Start a new one with `/clear` |
| `ERR:OVERFLOW` | Message too large for the tunnel | Split into smaller messages |
| `/resume` fails | Session ID doesn't exist on server | List available sessions with `/sessions`; old sessions may have been reaped |

### Build & setup issues

| Problem | Fix |
|---|---|
| `setup.sh` says "Missing: cmake" (or other tool) | Install prerequisites — see [Prerequisites](#prerequisites) |
| Build fails with OpenSSL errors | macOS: `brew install openssl@3`. Ubuntu: `sudo apt-get install libssl-dev` |
| Build fails with curl errors | macOS: `brew install curl`. Ubuntu: `sudo apt-get install libcurl4-openssl-dev` |
| `binaries not found in build/` | Clean rebuild: `rm -rf build && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build` |
| `base32 decode failed` at runtime | Client/server version mismatch; rebuild both from the same source |
| `npm install` fails in `web/` | Ensure Node.js is installed (v20+). If using mise: `mise install` |

### Web UI issues

| Problem | Fix |
|---|---|
| Web UI can't connect to server | Check server address/port in the sidebar. The web UI talks to the server via its Node.js backend, so `127.0.0.1` means the machine running `npm run dev`, not your browser |
| `DNS query failed after 3 attempts` | Server unreachable from the web backend; same fixes as [Client can't connect](#client-cant-connect) |
| `Upload failed at chunk N` | Network instability between web backend and DNS server; retry |
| `Server error: ERR:...` | Server-side issue; check server logs for details |

### Quick diagnostics

```bash
# Is the server running?
pgrep -f dnsclaw-server

# What's on port 53?
sudo lsof -i :53

# Can you reach the server?
dig @127.0.0.1 init.llm.local TXT    # should return a session ID

# Check your config
dnsclaw config

# Server logs (run in foreground)
./build/dnsclaw-server    # watch stderr for [config], [init], [llm] messages
```

## License

MIT — see [LICENSE](LICENSE).

Built by [@0Mattias](https://github.com/0Mattias). C port of [DNS-LLM](https://github.com/0Mattias/DNS-LLM) (Go).
