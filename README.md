# DNS-CLAW

**C Language Agentic Wireformat** — An agentic AI CLI tunneled through DNS, written in pure C.

Talk to Gemini through DNS queries. The client encodes your messages as Base32 TXT record lookups, the server decodes them, calls the Gemini API, and sends responses back as chunked TXT records. Supports tool use — the model can run commands on your machine (with approval), read/write files, and list directories.

## Quick Start

```bash
# Clone
git clone https://github.com/0Mattias/DNS-CLAW.git
cd DNS-CLAW

# Configure
cp .env.example .env
# Edit .env → paste your Gemini API key

# Build
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run server (terminal 1)
./build/server_bin

# Run client (terminal 2)
./build/client_bin
```

That's it. The client connects to the server over UDP on port 53535 by default — no sudo, no DNS config, no TLS setup needed for local use.

## How It Works

```
┌─────────────────┐     DNS TXT queries     ┌─────────────────┐     HTTPS     ┌─────────┐
│  Client (CLI)   │ ◄──────────────────────► │  Server (DNS)   │ ◄───────────► │ Gemini  │
│                 │   UDP / DoT / DoH        │                 │   REST API    │   API   │
└─────────────────┘                          └─────────────────┘               └─────────┘
```

1. Client encodes your message → Base32 → DNS TXT query labels
2. Server receives the DNS query, decodes the message, calls Gemini
3. Gemini response → Base64 → chunked TXT record answers
4. Client polls for response chunks, decodes, and renders with markdown

The protocol is stateful — sessions, multi-turn conversations, and tool call chains all work across the DNS tunnel.

## Features

- **Three transports**: Plain UDP, DNS-over-TLS (DoT), DNS-over-HTTPS (DoH)
- **Agentic tools**: `client_execute_bash`, `client_read_file`, `client_write_file`, `client_list_directory` — all with user approval prompts
- **Rich terminal UI**: Gradient ASCII banner, async spinners, full ANSI markdown rendering
- **One-shot mode**: `./build/client_bin -m "what is 2+2"` for scripting
- **Pipe support**: `echo "explain this" | ./build/client_bin`
- **Zero SDK**: No Go, no Python, no Node — just C, libcurl, OpenSSL, and cJSON

## Dependencies

| Dependency | Purpose | Install |
|---|---|---|
| C compiler | clang or gcc | Xcode / build-essential |
| CMake ≥ 3.14 | Build system | `brew install cmake` |
| Ninja | Build tool | `brew install ninja` |
| OpenSSL | TLS (DoT/DoH) | `brew install openssl@3` |
| libcurl | HTTP transport | `brew install curl` |
| cJSON | JSON parsing | Auto-fetched by CMake |

## Transport Modes

Edit `.env` to switch transports:

| Mode | Config | Port | Notes |
|---|---|---|---|
| **UDP** (default) | `USE_DOT=false`, `USE_DOH=false` | 53535 | No TLS, no sudo |
| **DoT** | `USE_DOT=true` | 853 | Needs certs + sudo |
| **DoH** | `USE_DOH=true` | 443 | Needs certs + sudo |

For DoT/DoH, generate self-signed certs first:
```bash
./generate_certs.sh
```

## Client Usage

**Interactive mode** (default):
```bash
./build/client_bin
```

**One-shot**:
```bash
./build/client_bin -m "list files in my home directory"
```

**Pipe**:
```bash
cat error.log | ./build/client_bin -m "explain this error"
```

**Flags**:
| Flag | Description |
|---|---|
| `-m <msg>` | One-shot message |
| `-s <addr>` | Server address |
| `-p <port>` | Server port |
| `--no-color` | Disable ANSI colors |
| `--no-typewriter` | Disable typewriter text effect |

**In-session commands**:
| Command | Description |
|---|---|
| `/help` | Show commands |
| `/clear` | New chat session |
| `/compact [focus]` | Compact conversation context |
| `/status` | Show session info |
| `/exit` | Quit |

## Project Structure

```
├── src/
│   ├── client/main.c      # CLI, markdown renderer, tool executor
│   ├── server/main.c      # DNS server, Gemini API client, session manager
│   └── common/
│       ├── dns_proto.c     # DNS wire format + UDP/DoT/DoH transports
│       ├── base32.c        # RFC 4648 Base32 (DNS label encoding)
│       └── base64.c        # RFC 4648 Base64 (response encoding)
├── include/                # Headers
├── CMakeLists.txt          # Build config
├── generate_certs.sh       # Self-signed TLS cert generator
└── .env.example            # Config template
```

## License

MIT — see [LICENSE](LICENSE).

Built by [@0Mattias](https://github.com/0Mattias). Port of [DNS-LLM](https://github.com/0Mattias/DNS-LLM) (Go) to C.
