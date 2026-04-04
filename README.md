# DNS-CLAW

[![CI](https://github.com/0Mattias/DNS-CLAW/actions/workflows/ci.yml/badge.svg)](https://github.com/0Mattias/DNS-CLAW/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux-blue)

**C Language Agentic Wireformat** — An agentic AI CLI tunneled through DNS, written in pure C.

Talk to LLMs through DNS queries. The client encodes your messages as Base32 TXT record lookups, the server decodes them, calls your LLM provider, and sends responses back as chunked TXT records. Fully agentic — the model can run commands, read/write/edit files, search codebases, and fetch URLs on your machine.

Supports **Gemini**, **OpenAI**, **Claude (Anthropic)**, and **OpenRouter**.

## Example

<img width="784" height="447" alt="DNS-CLAW terminal session" src="https://github.com/user-attachments/assets/9e8b62b3-c633-474d-852c-9ad3fd08cd63" />

## Why DNS?

DNS is the one protocol that works almost everywhere — through captive portals, hotel WiFi, corporate firewalls, and restricted networks where direct HTTPS to AI APIs is blocked. DNS-CLAW encrypts your prompts inside standard-looking DNS queries so you can reach your LLM from anywhere.

- **Works on restricted networks** — DNS traffic (port 53) is almost never blocked
- **Invisible to DPI** — AES-256-GCM payload encryption makes queries look like random noise
- **Zero cloud dependencies** — run your own server, keep your data

## Quick Start

### Prerequisites

<details>
<summary><b>macOS</b></summary>

```bash
# Xcode command-line tools (provides the C compiler)
xcode-select --install

# Build dependencies
brew install cmake openssl curl
```

libedit (for command history) is pre-installed on macOS.
</details>

<details>
<summary><b>Ubuntu / Debian</b></summary>

```bash
sudo apt-get update
sudo apt-get install build-essential cmake libssl-dev libcurl4-openssl-dev
```

Optional: `sudo apt-get install libedit-dev` for command history and line editing.
</details>

### Setup

```bash
git clone https://github.com/0Mattias/DNS-CLAW.git
cd DNS-CLAW
./setup.sh
```

The setup script checks dependencies, asks for your LLM provider and API key, generates an encryption key, and builds everything. Re-run safely anytime — it skips provider setup if a key already exists (use `--reconfigure` to force).

### Run

```bash
# Terminal 1 — start the server
./build/dnsclaw-server              # macOS (no root needed)
sudo -E ./build/dnsclaw-server      # Linux (port 53 needs root; -E preserves your .env)

# Terminal 2 — start chatting
./build/dnsclaw
```

### Verify it works

Once both are running, type a message in the client. You should see the LLM response rendered in your terminal within a few seconds. If something goes wrong, the [Troubleshooting](#troubleshooting) section has solutions for every common error.

### Install system-wide (optional)

```bash
sudo cmake --install build
# Now available anywhere:
dnsclaw-server            # terminal 1 (macOS)
sudo -E dnsclaw-server    # terminal 1 (Linux)
dnsclaw                   # terminal 2
```

## Features

- **Multi-provider** — Gemini, OpenAI, Claude (Anthropic), OpenRouter with auto-detection
- **Payload encryption** — AES-256-GCM with PSK defeats DPI on captive portals and public WiFi
- **Three transports** — Plain UDP (port 53), DNS-over-TLS (port 853), DNS-over-HTTPS (port 443)
- **7 agentic tools** — `execute_bash`, `read_file`, `write_file`, `edit_file` (surgical find-and-replace), `list_directory`, `search_files` (recursive grep), `fetch_url` — safe tools auto-approve, destructive ones prompt
- **Rich terminal UI** — Gradient ASCII banner, async spinners, full ANSI markdown rendering (bold, italic, code, headers, lists, code blocks)
- **One-shot mode** — `dnsclaw -m "what is 2+2"` for scripting
- **Pipe support** — `echo "explain this" | dnsclaw`
- **Command history** — Arrow keys, line editing, persistent history across sessions (via libedit, auto-detected)
- **Session management** — `/clear`, `/compact`, `/status`, `/export` commands; auto-reaping of idle sessions after 30 minutes
- **Automatic retry** — DNS queries retry up to 3 times with exponential backoff on transient failures
- **Zero SDK** — No Go, no Python, no Node — just C, libcurl, OpenSSL, and cJSON
- **Hardened build** — `-D_FORTIFY_SOURCE=2`, `-fstack-protector-strong`, `-fPIE`, full RELRO on Linux; ASan/UBSan/TSan in CI

## How It Works

```
┌─────────────────┐     DNS TXT queries     ┌─────────────────┐     HTTPS     ┌─────────┐
│  dnsclaw (CLI)  │ <────────────────────> │  dnsclaw-server │ <───────────> │ LLM API │
│                 │   UDP / DoT / DoH        │                 │   REST API    │         │
└─────────────────┘                          └─────────────────┘               └─────────┘
```

1. Client encrypts your message (AES-256-GCM) -> Base32 encodes -> DNS TXT query labels
2. Server receives the DNS query, Base32 decodes, decrypts, calls your LLM provider
3. LLM response -> AES-256-GCM encrypted -> Base64 encoded -> chunked TXT records
4. Client polls for chunks, Base64 decodes, decrypts, renders markdown to terminal

The protocol is stateful — sessions, multi-turn conversations, and tool call chains all work across the DNS tunnel.

## Commands

### `dnsclaw` — the client

Interactive terminal chat client that communicates with `dnsclaw-server` via DNS tunneling.

```bash
dnsclaw                          # interactive REPL
dnsclaw -m "what is 2+2"         # one-shot mode
echo "explain this" | dnsclaw    # pipe mode
```

| Flag | Description |
|---|---|
| `-m <msg>` | One-shot message (print response and exit) |
| `-s <addr>` | Server address (overrides .env) |
| `-p <port>` | Server port (overrides .env) |
| `--no-color` | Disable ANSI colors |
| `--no-typewriter` | Disable typewriter text effect |
| `-h, --help` | Show usage help |
| `-v` | Print version |

**In-session commands:**

| Command | Description |
|---|---|
| `/help` | Show available commands |
| `/clear` | Start a new chat session |
| `/compact [focus]` | Compress conversation context |
| `/export [file]` | Export conversation to markdown file |
| `/config` | Show current configuration |
| `/status` | Show session ID, transport, encryption info |
| `/exit` | Quit |

### `dnsclaw config` — manage settings

View and change settings without editing files manually.

```bash
dnsclaw config                                     # show current config
dnsclaw config --edit                              # open config in $EDITOR
dnsclaw config --set ANTHROPIC_MODEL=claude-sonnet-4-6  # set a value
dnsclaw config --provider                          # interactive provider setup
```

### `dnsclaw-server` — the server

DNS server that tunnels LLM requests. Listens for DNS queries, reassembles chunked messages, calls the configured LLM API, and returns responses as TXT records.

```bash
./build/dnsclaw-server              # start (macOS — no root needed)
sudo -E dnsclaw-server              # start (Linux — port 53 needs root)
dnsclaw-server --help               # show all options and env vars
dnsclaw-server --version            # print version
```

If no API key is configured, the server diagnoses the cause (missing `.env` file, sudo without `-E`, or empty config), lists the paths it searched, and prints setup instructions. If a privileged port bind fails, it suggests `sudo -E` or setting `SERVER_PORT`.

The server logs all activity to stderr with colored output:
```
[config] Provider:  Claude
[config] Model:     claude-sonnet-4-6
[config] Transport: UDP (plain)
[config] Encryption: AES-256-GCM (PSK)
[config] Port:      53
[init]   New session: a1b2c3d4
[llm]    Processing sid=a1b2c3d4 mid=1 type=user
```

## Agent Tools

The LLM has access to 7 tools for interacting with the client's machine:

| Tool | Description | Approval |
|---|---|---|
| `client_execute_bash` | Run any shell command | **Requires approval** |
| `client_read_file` | Read file contents | Prompts for absolute/`..` paths |
| `client_write_file` | Create/overwrite a file | **Requires approval** |
| `client_edit_file` | Surgical find-and-replace in a file | **Requires approval** |
| `client_list_directory` | List directory contents with sizes | Auto-approved |
| `client_search_files` | Recursive grep with file filtering | Auto-approved |
| `client_fetch_url` | HTTP GET a URL (http/https only) | Auto-approved |

Safe read-only tools run automatically. Destructive tools (bash, write, edit) prompt for `[Y/n]` confirmation. `read_file` prompts when the path is absolute or contains `..` to prevent exfiltration of sensitive files.

## Configuration

Config is loaded from these locations (first match wins per variable):

1. `~/.config/dnsclaw/.env` — user config (created by `setup.sh`)
2. `$SUDO_USER`'s `~/.config/dnsclaw/.env` — original user's config when running under sudo
3. `../.env` — parent directory (legacy)
4. `./.env` — project-local config (for development)

Environment variables set in your shell override all `.env` files.

The easiest way to manage settings is `dnsclaw config` — see [config subcommand](#dnsclaw-config--manage-settings) above.

### Full config reference

```bash
# ── LLM Provider ────────────────────────────────────────────────────
# Set one API key — the server auto-detects the provider.
# Or set LLM_PROVIDER explicitly: gemini | openai | anthropic | openrouter

# Gemini — https://aistudio.google.com/apikey
GEMINI_API_KEY="your-api-key"
GEMINI_MODEL="gemini-3.1-pro-preview"

# OpenAI — https://platform.openai.com/api-keys
# OPENAI_API_KEY="sk-..."
# OPENAI_MODEL="gpt-5.4"

# Anthropic (Claude) — https://console.anthropic.com/settings/keys
# ANTHROPIC_API_KEY="sk-ant-..."
# ANTHROPIC_MODEL="claude-sonnet-4-6"

# OpenRouter — https://openrouter.ai/keys
# OPENROUTER_API_KEY="sk-or-..."
# OPENROUTER_MODEL="openrouter/auto"

# Payload Encryption (recommended)
# Generate with: openssl rand -base64 32
TUNNEL_PSK="your-psk-here"

# Transport mode (pick one or leave both false for plain UDP)
# Port is auto-detected: UDP=53, DoT=853, DoH=443
USE_DOT=false
USE_DOH=false

# Server address
DNS_SERVER_ADDR=127.0.0.1
INSECURE_SKIP_VERIFY=true

# SERVER_PORT=53  (only set for non-standard ports; auto-detected from transport)

# TLS certificates (DoT/DoH only)
TLS_CERT=cert.pem
TLS_KEY=key.pem

# Custom system prompt (optional — override the default AI persona)
# SYSTEM_PROMPT="You are a helpful assistant."
```

## Security: Payload-Level Encryption

DNS-CLAW uses **AES-256-GCM** payload encryption with a pre-shared key (PSK). Instead of encrypting the connection (which changes packet shape and alerts firewalls), we encrypt the data itself before it touches the DNS protocol.

```
┌──────────────────────────────────────────────────────────────────┐
│                                                                  │
│  Your prompt: "What is my IP?"                                   │
│       | AES-256-GCM encrypt with TUNNEL_PSK                      │
│  Ciphertext: [magic:0xCE01][nonce:12B][encrypted][tag:16B]       │
│       | Base32 encode                                             │
│  DNS query: gizqwerty4abc.0.up.3.a1b2c3.llm.local.              │
│                                                                  │
│  Firewall sees: standard plaintext DNS query                     │
│  Snooper sees:  random noise in labels and TXT records           │
│  Actual data:   AES-256 encrypted — completely unreadable        │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

### Setting up encryption manually

```bash
# Generate a key
openssl rand -base64 32

# Add to ~/.config/dnsclaw/.env (same key on client AND server)
TUNNEL_PSK="your-generated-key-here"
```

`setup.sh` generates this automatically.

### Crypto details

| Property | Value |
|---|---|
| Cipher | AES-256-GCM (authenticated encryption) |
| Key derivation | HKDF-SHA256 (salt: `dns-claw-v1`, info: `tunnel-key`) |
| Nonce | 12 random bytes per message (`RAND_bytes`) |
| Auth tag | 16 bytes (GCM) |
| Wire overhead | 30 bytes per message |
| Magic header | `0xCE 0x01` — identifies encrypted payloads |

Encryption is optional — omit `TUNNEL_PSK` and the system works unencrypted. When enabled, it applies to all transport modes (UDP, DoT, DoH) as an additional layer.

## Transport Modes

| Mode | Config | Default Port | Encryption | Notes |
|---|---|---|---|---|
| **UDP** (default) | Both `false` | 53 | PSK only | Standard DNS; needs `sudo` on Linux, works without on macOS |
| **DoT** | `USE_DOT=true` | 853 | TLS + PSK | Needs `sudo` on Linux, needs certs |
| **DoH** | `USE_DOH=true` | 443 | HTTPS + PSK | Needs `sudo` on Linux, needs certs |

For DoH, set `DNS_SERVER_ADDR=https://127.0.0.1/dns-query` (the full URL). For UDP and DoT, use just `127.0.0.1`.

TLS certs for DoT/DoH are generated automatically by `./setup.sh --advanced` when you select a TLS transport.

## Troubleshooting

| Problem | Cause | Fix |
|---|---|---|
| `Failed to initialize session` | Client can't reach server | Check server is running, ports match, and `DNS_SERVER_ADDR` is correct |
| `Decryption failed — PSK mismatch` | Different `TUNNEL_PSK` values | Ensure client and server use the same key |
| `FATAL: No API key configured` | Missing API key | Run `./setup.sh` or `dnsclaw config --provider` — the error output shows which `.env` paths were searched and the likely cause |
| `bind: Permission denied` | Port 53/443/853 requires root on Linux | Run server with `sudo -E` (the `-E` preserves your env) or set `SERVER_PORT` to a high port. macOS does not require root for port 53 |
| DoH connection refused | Wrong address format | Use `https://127.0.0.1/dns-query` (full URL with path) |
| `base32 decode failed` | Corrupted packet or version mismatch | Rebuild both binaries from same source |
| Setup script fails | Missing build tools | See [Prerequisites](#prerequisites) for your platform |

## Dependencies

| Dependency | Purpose | macOS | Ubuntu / Debian |
|---|---|---|---|
| C compiler | clang or gcc | `xcode-select --install` | `build-essential` |
| CMake >= 3.14 | Build system | `brew install cmake` | `cmake` |
| OpenSSL 3.x | TLS + AES-256-GCM | `brew install openssl@3` | `libssl-dev` |
| libcurl | HTTPS transport | `brew install curl` | `libcurl4-openssl-dev` |
| cJSON | JSON serialization | Auto-fetched by CMake | Auto-fetched by CMake |
| libedit (optional) | Command history | Pre-installed | `libedit-dev` |
| Ninja (optional) | Faster builds | `brew install ninja` | `ninja-build` |

## Development

### Manual setup (without setup.sh)

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Configure (option A: interactive)
./build/dnsclaw config --provider

# Configure (option B: copy and edit template)
mkdir -p ~/.config/dnsclaw
cp .env.example ~/.config/dnsclaw/.env
# Edit: set your API key and TUNNEL_PSK

# Run
./build/dnsclaw-server           # terminal 1 (macOS)
sudo -E ./build/dnsclaw-server   # terminal 1 (Linux — port 53 needs root)
./build/dnsclaw                  # terminal 2
```

### Building from source

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Build types: `Release` (hardened, default), `Debug` (ASan + UBSan), `TSan` (ThreadSanitizer).

### Running tests

```bash
ctest --test-dir build --output-on-failure
```

47 tests across 4 suites cover the common library: base64 (encoding, decoding, invalid input rejection, RFC 4648 vectors), base32 (encoding, decoding, case insensitivity, binary roundtrip), AES-256-GCM crypto (roundtrip, wrong-key, tampering, empty payload, large payload), and DNS wire format (build/parse, truncation, edge cases).

### CI

GitHub Actions runs on every push and PR: `clang-format` check, matrix build (Ubuntu + macOS x Release + Debug), and a dedicated ThreadSanitizer job. See [`.github/workflows/ci.yml`](.github/workflows/ci.yml).

### Code style

A `.clang-format` config is included (LLVM-based, 4-space indent, 100-column limit). CI enforces formatting — run `find src include tests -name '*.c' -o -name '*.h' | xargs clang-format -i` before committing.

## Project Structure

```
├── src/
│   ├── client/          # CLI client (REPL, one-shot, pipe modes)
│   │   ├── main.c       # Entry point, arg parsing, REPL loop
│   │   ├── protocol.c   # DNS query wrapper, message processing, 7-tool executor
│   │   ├── render.c     # ANSI markdown renderer
│   │   ├── spinner.c    # Async activity spinner
│   │   └── ui.c         # Banner, help text, config subcommand, provider setup
│   ├── server/          # DNS server + LLM integration
│   │   ├── main.c       # Entry point, provider detection, setup wizard
│   │   ├── handler.c    # DNS query dispatcher (init/upload/fin/download)
│   │   ├── llm.c        # Multi-provider API calls + response parsing
│   │   ├── transport.c  # UDP, DoT, DoH listeners
│   │   ├── session.c    # Session lifecycle + reaper thread
│   │   └── log.c        # Colored logging
│   └── common/          # Shared between client and server
│       ├── crypto.c     # AES-256-GCM encryption + HKDF key derivation
│       ├── dns_proto.c  # DNS wire format + UDP/DoT/DoH transport
│       ├── base32.c     # RFC 4648 Base32 (client -> server via DNS labels)
│       ├── base64.c     # RFC 4648 Base64 (server -> client via TXT records)
│       └── config.c     # .env file loader
├── tests/               # Unit tests (CTest)
│   ├── test_base64.c    # Base64 codec tests (RFC 4648 vectors)
│   ├── test_base32.c    # Base32 codec tests
│   ├── test_crypto.c    # AES-256-GCM roundtrip, wrong-key, tampering
│   └── test_dns_proto.c # DNS wire format build/parse tests
├── include/             # Header files
├── .github/workflows/   # CI (GitHub Actions)
├── CMakeLists.txt       # Build system (auto-fetches cJSON)
├── setup.sh             # First-run setup script (also generates TLS certs)
├── .clang-format        # Code style config
├── .env.example         # Configuration template
└── LICENSE              # MIT
```

## License

MIT — see [LICENSE](LICENSE).

Built by [@0Mattias](https://github.com/0Mattias). C port of [DNS-LLM](https://github.com/0Mattias/DNS-LLM) (Go).
