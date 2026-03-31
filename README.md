# DNS-CLAW

**C Language Agentic Wireformat** — An agentic AI CLI tunneled through DNS, written in pure C.

Talk to Gemini through DNS queries. The client encodes your messages as Base32 TXT record lookups, the server decodes them, calls the Gemini API, and sends responses back as chunked TXT records. Supports tool use — the model can run commands on your machine (with approval), read/write files, and list directories.

## Example
<img width="784" height="447" alt="Screenshot 2026-03-30 at 10 59 37 PM" src="https://github.com/user-attachments/assets/9e8b62b3-c633-474d-852c-9ad3fd08cd63" />



## Quick Start

```bash
# Clone
git clone https://github.com/0Mattias/DNS-CLAW.git
cd DNS-CLAW

# Configure
cp .env.example .env
# Edit .env:
#   1. Paste your Gemini API key
#   2. Generate and set a TUNNEL_PSK (see below)

# Build
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Generate TLS certs (only needed for DoT/DoH)
./generate_certs.sh

# Run server (terminal 1 — port 53 requires sudo)
sudo ./build/server_bin

# Run client (terminal 2)
./build/client_bin
```

## How It Works

```
┌─────────────────┐     DNS TXT queries     ┌─────────────────┐     HTTPS     ┌─────────┐
│  Client (CLI)   │ ◄──────────────────────► │  Server (DNS)   │ ◄───────────► │ Gemini  │
│                 │   UDP / DoT / DoH        │                 │   REST API    │   API   │
└─────────────────┘                          └─────────────────┘               └─────────┘
```

1. Client encrypts your message (AES-256-GCM) → Base32 encodes → DNS TXT query labels
2. Server receives the DNS query, Base32 decodes, decrypts, calls Gemini
3. Gemini response → AES-256-GCM encrypted → Base64 encoded → chunked TXT records
4. Client polls for chunks, Base64 decodes, decrypts, renders markdown to terminal

The protocol is stateful — sessions, multi-turn conversations, and tool call chains all work across the DNS tunnel.

## Security: Payload-Level Encryption

DNS-CLAW uses **AES-256-GCM** payload encryption with a pre-shared key (PSK) to defeat deep packet inspection (DPI). Instead of encrypting the connection (which changes packet shape and alerts firewalls), we encrypt the data itself before it touches the DNS protocol.

```
┌──────────────────────────────────────────────────────────────────┐
│                                                                  │
│  Your prompt: "What is my IP?"                                   │
│       ↓ AES-256-GCM encrypt with TUNNEL_PSK                     │
│  Ciphertext: [magic:0xCE01][nonce:12B][encrypted][tag:16B]      │
│       ↓ Base32 encode                                            │
│  DNS query: gizqwerty4abc.0.up.3.a1b2c3.llm.local.              │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │  Firewall sees: standard plaintext DNS query ✓             │  │
│  │  Snooper sees:  random noise in labels and TXT records ✓   │  │
│  │  Actual data:   AES-256 encrypted — completely unreadable ✓│  │
│  └────────────────────────────────────────────────────────────┘  │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

### Setting Up Encryption

```bash
# 1. Generate a strong random key
openssl rand -base64 32

# 2. Add to your .env file (same key on client AND server)
TUNNEL_PSK="your-generated-key-here"
```

Both the client and server read `TUNNEL_PSK` from `.env`. If the keys don't match, you'll get a clear `Decryption failed — PSK mismatch` error.

### Crypto Details

| Property | Value |
|---|---|
| Cipher | AES-256-GCM (authenticated encryption) |
| Key derivation | HKDF-SHA256 (salt: `dns-claw-v1`, info: `tunnel-key`) |
| Nonce | 12 random bytes per message (`RAND_bytes`) |
| Auth tag | 16 bytes (GCM) |
| Wire overhead | 30 bytes per message |
| Magic header | `0xCE 0x01` — identifies encrypted payloads |

Encryption is **optional** — omit `TUNNEL_PSK` from `.env` and the system works unencrypted (backward compatible). When enabled, it applies to **all transport modes** (UDP, DoT, DoH) as an additional layer.

## Features

- **Payload encryption** — AES-256-GCM with PSK defeats DPI on captive portals and public WiFi
- **Three transports** — Plain UDP (port 53), DNS-over-TLS (port 853), DNS-over-HTTPS (port 443)
- **Agentic tools** — `client_execute_bash`, `client_read_file`, `client_write_file`, `client_list_directory` — all with user approval prompts
- **Rich terminal UI** — Gradient ASCII banner, async spinners, full ANSI markdown rendering (bold, italic, code, headers, lists, code blocks with syntax labels)
- **One-shot mode** — `./build/client_bin -m "what is 2+2"` for scripting
- **Pipe support** — `echo "explain this" | ./build/client_bin`
- **Session management** — `/clear`, `/compact`, `/status` commands; auto-reaping of idle sessions after 30 minutes
- **Zero SDK** — No Go, no Python, no Node — just C, libcurl, OpenSSL, and cJSON

## Dependencies

| Dependency | Purpose | Install |
|---|---|---|
| C compiler | clang or gcc | Xcode / `build-essential` |
| CMake ≥ 3.14 | Build system | `brew install cmake` |
| Ninja | Build tool | `brew install ninja` |
| OpenSSL 3.x | TLS + AES-256-GCM encryption | `brew install openssl@3` |
| libcurl | HTTPS transport (DoH + Gemini API) | `brew install curl` |
| cJSON | JSON serialization | Auto-fetched by CMake |

## Configuration

All configuration lives in a `.env` file (copy from `.env.example`):

```bash
# Required — your Gemini API key
GEMINI_API_KEY="your-api-key"
GEMINI_MODEL="gemini-3.1-pro-preview"

# Recommended — payload encryption key
# Generate with: openssl rand -base64 32
TUNNEL_PSK="your-psk-here"

# Transport mode (pick one or leave both false for plain UDP)
USE_DOT=false
USE_DOH=false

# Server address
#   UDP/DoT: 127.0.0.1
#   DoH:     https://127.0.0.1/dns-query  ← must include https:// and path
DNS_SERVER_ADDR=127.0.0.1

# Skip TLS cert verification (for self-signed certs)
INSECURE_SKIP_VERIFY=true

# Override default port (optional — defaults: UDP=53, DoT=853, DoH=443)
# SERVER_PORT=53

# TLS certificates (DoT/DoH only)
TLS_CERT=cert.pem
TLS_KEY=key.pem
```

## Transport Modes

| Mode | Config | Default Port | Encryption Layer(s) | Notes |
|---|---|---|---|---|
| **UDP** (default) | Both `false` | 53 | PSK only | Standard DNS, requires `sudo` |
| **DoT** | `USE_DOT=true` | 853 | TLS + PSK | Requires `sudo`, needs certs |
| **DoH** | `USE_DOH=true` | 443 | HTTPS + PSK | Requires `sudo`, needs certs |

> **Important**: For DoH, set `DNS_SERVER_ADDR=https://127.0.0.1/dns-query` (the full URL). For UDP and DoT, use just `127.0.0.1`.

All modes support payload-level AES-256-GCM on top. For UDP, the PSK is your **only** encryption — but it's military-grade. For DoT/DoH, it adds a second layer.

Generate self-signed certs for DoT/DoH:
```bash
./generate_certs.sh
```

## Client Usage

### Interactive mode (default)
```bash
./build/client_bin
```

### One-shot
```bash
./build/client_bin -m "list files in my home directory"
```

### Pipe
```bash
cat error.log | ./build/client_bin -m "explain this error"
```

### Flags
| Flag | Description |
|---|---|
| `-m <msg>` | One-shot message |
| `-s <addr>` | Server address |
| `-p <port>` | Server port |
| `--no-color` | Disable ANSI colors |
| `--no-typewriter` | Disable typewriter text effect |

### In-Session Commands
| Command | Description |
|---|---|
| `/help` | Show available commands |
| `/clear` | Start a new chat session |
| `/compact [focus]` | Compress conversation context |
| `/status` | Show session ID, transport, encryption, connection info |
| `/exit` | Quit |

## Troubleshooting

| Problem | Cause | Fix |
|---|---|---|
| `Failed to initialize session` | Client can't reach server | Check server is running, ports match, and `DNS_SERVER_ADDR` is correct |
| `Decryption failed — PSK mismatch` | Client and server have different `TUNNEL_PSK` values | Ensure both use the exact same key in `.env` |
| `FATAL: GEMINI_API_KEY not set` | Missing or empty API key | Add a valid Gemini API key to `.env` |
| `bind: Permission denied` | Port 53/443/853 requires root | Run server with `sudo` |
| DoH connection refused | Wrong `DNS_SERVER_ADDR` format | Use `https://127.0.0.1/dns-query` (full URL with path) |
| `base32 decode failed` | Corrupted packet or version mismatch | Rebuild both client and server from same source |

## Project Structure

```
├── src/
│   ├── client/main.c        # CLI, markdown renderer, tool executor (1300+ lines)
│   ├── server/main.c        # DNS server, Gemini API client, session manager (1400+ lines)
│   └── common/
│       ├── crypto.c          # AES-256-GCM payload encryption + HKDF key derivation
│       ├── dns_proto.c       # DNS wire format + UDP/DoT/DoH transport implementations
│       ├── base32.c          # RFC 4648 Base32 encoding (client→server via DNS labels)
│       ├── base64.c          # RFC 4648 Base64 encoding (server→client via TXT records)
│       └── config.c          # .env file loader
├── include/
│   ├── crypto.h              # Encryption API
│   ├── dns_proto.h           # DNS protocol API
│   ├── base32.h / base64.h   # Encoding APIs
│   └── config.h              # Shared config, version, theme colors
├── CMakeLists.txt            # Build configuration (fetches cJSON automatically)
├── generate_certs.sh         # Self-signed TLS certificate generator
├── .env.example              # Configuration template
└── LICENSE                   # MIT
```

## License

MIT — see [LICENSE](LICENSE).

Built by [@0Mattias](https://github.com/0Mattias). C port of [DNS-LLM](https://github.com/0Mattias/DNS-LLM) (Go).
