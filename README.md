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
- [Troubleshooting](#troubleshooting)
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

| Problem | Fix |
|---|---|
| `Failed to initialize session` | Check server is running, ports match, `DNS_SERVER_ADDR` is correct |
| `Decryption failed — PSK mismatch` | Ensure client and server use the same `TUNNEL_PSK` |
| `FATAL: No API key configured` | Run `./setup.sh` or `dnsclaw config --provider` |
| `bind: Permission denied` | `sudo -E` on Linux, or set `SERVER_PORT` to a high port |
| DoH connection refused | Use full URL: `https://127.0.0.1/dns-query` |
| `base32 decode failed` | Rebuild both binaries from same source |
| Setup script fails | See [Prerequisites](#prerequisites) for your platform |

## License

MIT — see [LICENSE](LICENSE).

Built by [@0Mattias](https://github.com/0Mattias). C port of [DNS-LLM](https://github.com/0Mattias/DNS-LLM) (Go).
