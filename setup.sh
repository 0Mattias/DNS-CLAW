#!/bin/bash
# DNS-CLAW — First-run setup script
# Checks deps, configures provider, generates keys, and builds.
# Re-run safely: skips provider setup if a key exists (use --reconfigure to force).
set -e

# ── Colors ──────────────────────────────────────────────────────────────────
R1='\033[38;2;255;60;50m'
R2='\033[38;2;255;100;80m'
R3='\033[38;2;255;140;110m'
R4='\033[38;2;255;195;180m'
DIM='\033[38;2;100;80;80m'
RST='\033[0m'
BOLD='\033[1m'

info()  { echo -e "  ${R2}│${RST}  $1"; }
ok()    { echo -e "  ${R2}│  ${R3}✓${RST} $1"; }
err()   { echo -e "  ${R1}│  ✗${RST} $1"; }
prompt(){ echo -ne "  ${R2}│${RST}  $1"; }

# ── Argument parsing ───────────────────────────────────────────────────────
FORCE_RECONFIG=0
ADVANCED=0

for arg in "$@"; do
    case "$arg" in
        --reconfigure|-r) FORCE_RECONFIG=1 ;;
        --advanced|-a)    ADVANCED=1 ;;
        --help|-h)
            echo "Usage: ./setup.sh [options]"
            echo ""
            echo "Options:"
            echo "  --reconfigure, -r   Force re-entry of API key and provider"
            echo "  --advanced, -a      Show transport selection (UDP/DoT/DoH)"
            echo "  --help, -h          Show this help"
            echo ""
            echo "On first run, the script checks dependencies, asks for your LLM"
            echo "provider and API key, generates an encryption key, and builds."
            echo "Re-run safely anytime — it skips provider setup if a key exists."
            exit 0
            ;;
    esac
done

# ── OS detection ───────────────────────────────────────────────────────────
OS="$(uname -s)"
IS_MAC=0
IS_LINUX=0
HAS_APT=0

case "$OS" in
    Darwin) IS_MAC=1 ;;
    Linux)  IS_LINUX=1; command -v apt-get &>/dev/null && HAS_APT=1 ;;
esac

# ── Banner ──────────────────────────────────────────────────────────────────
echo ""
echo -e "\033[38;2;255;20;20m▓█████▄  ███▄    █   ██████     ▄████▄   ██▓    ▄▄▄       █     █░${RST}"
echo -e "\033[38;2;255;60;50m▒██▀ ██▌ ██ ▀█   █ ▒██    ▒    ▒██▀ ▀█  ▓██▒   ▒████▄    ▓█░ █ ░█░${RST}"
echo -e "\033[38;2;255;100;80m░██   █▌▓██  ▀█ ██▒░ ▓██▄      ▒▓█    ▄ ▒██░   ▒██  ▀█▄  ▒█░ █ ░█ ${RST}"
echo -e "\033[38;2;255;140;110m░▓█▄   ▌▓██▒  ▐▌██▒  ▒   ██▒   ▒▓▓▄ ▄██▒▒██░   ░██▄▄▄▄██ ░█░ █ ░█ ${RST}"
echo -e "\033[38;2;255;170;150m░▒████▓ ▒██░   ▓██░▒██████▒▒   ▒ ▓███▀ ░░██████▒▓█   ▓██▒░░██▒██▓ ${RST}"
echo -e "\033[38;2;255;195;180m ▒▒▓  ▒ ░ ▒░   ▒ ▒ ▒ ▒▓▒ ▒ ░   ░ ░▒ ▒  ░░ ▒░▓  ░▒▒   ▓▒█░░ ▓░▒ ▒  ${RST}"
echo -e "\033[38;2;255;215;210m ░ ▒  ▒ ░ ░░   ░ ▒░░ ░▒  ░ ░     ░  ▒   ░ ░ ▒  ░ ▒   ▒▒ ░  ▒ ░ ░  ${RST}"
echo -e "\033[38;2;255;230;225m ░ ░  ░    ░   ░ ░ ░  ░  ░     ░          ░ ░    ░   ▒     ░   ░  ${RST}"
echo -e "\033[38;2;255;245;242m   ░             ░       ░     ░ ░          ░  ░     ░  ░    ░    ${RST}"
echo -e "\033[38;2;255;255;255m ░                             ░                                  ${RST}"
echo ""
echo -e "  ${R2}┌─ Setup ${DIM}──────────────────────────────────${RST}"
echo -e "  ${R2}│${RST}"

# ── Check dependencies ─────────────────────────────────────────────────────
info "Checking dependencies..."

MISSING=""
MISSING_PKGS=""

# Check for a C compiler
if ! command -v cc &>/dev/null && ! command -v gcc &>/dev/null; then
    MISSING="$MISSING cc"
    if [ "$IS_MAC" -eq 1 ]; then
        MISSING_PKGS="$MISSING_PKGS (install Xcode CLI tools: xcode-select --install)"
    elif [ "$HAS_APT" -eq 1 ]; then
        MISSING_PKGS="$MISSING_PKGS build-essential"
    fi
fi

for cmd in cmake openssl curl; do
    if ! command -v "$cmd" &>/dev/null; then
        MISSING="$MISSING $cmd"
    fi
done

if [ -n "$MISSING" ]; then
    err "Missing:${MISSING}"
    if [ "$IS_MAC" -eq 1 ]; then
        info "Install with: ${BOLD}brew install${MISSING}${RST}"
    elif [ "$HAS_APT" -eq 1 ]; then
        # Map commands to package names
        APT_PKGS="$MISSING_PKGS"
        for cmd in $MISSING; do
            case "$cmd" in
                cmake)   APT_PKGS="$APT_PKGS cmake" ;;
                openssl) APT_PKGS="$APT_PKGS libssl-dev" ;;
                curl)    APT_PKGS="$APT_PKGS libcurl4-openssl-dev" ;;
            esac
        done
        info "Install with: ${BOLD}sudo apt-get install${APT_PKGS}${RST}"
    else
        info "Please install:${MISSING}"
    fi
    echo -e "  ${R2}└${DIM}──────────────────────────────────────────${RST}"
    exit 1
fi
ok "All dependencies found"
echo -e "  ${R2}│${RST}"

# ── Config directory ────────────────────────────────────────────────────────
CONFIG_DIR="$HOME/.config/dnsclaw"
ENV_FILE="$CONFIG_DIR/.env"
mkdir -p "$CONFIG_DIR"

# ── Provider setup ──────────────────────────────────────────────────────────
HAS_KEY=0
if [ "$FORCE_RECONFIG" -eq 0 ] && [ -f "$ENV_FILE" ]; then
    for key in GEMINI_API_KEY OPENAI_API_KEY ANTHROPIC_API_KEY OPENROUTER_API_KEY; do
        if grep -q "^${key}=" "$ENV_FILE" 2>/dev/null; then
            VAL=$(grep "^${key}=" "$ENV_FILE" | head -1 | cut -d= -f2 | tr -d '"' | tr -d "'")
            if [ -n "$VAL" ] && [ "$VAL" != "your-api-key-here" ]; then
                HAS_KEY=1
                ok "Found existing API key in $ENV_FILE"
                info "To change provider: ${BOLD}./setup.sh --reconfigure${RST}"
                break
            fi
        fi
    done
fi

if [ "$HAS_KEY" -eq 0 ]; then
    info "Select your LLM provider:"
    echo -e "  ${R2}│${RST}"
    echo -e "  ${R2}│${RST}    ${R3}1)${RST} Gemini         ${DIM}https://aistudio.google.com/apikey${RST}"
    echo -e "  ${R2}│${RST}    ${R3}2)${RST} OpenAI         ${DIM}https://platform.openai.com/api-keys${RST}"
    echo -e "  ${R2}│${RST}    ${R3}3)${RST} Claude         ${DIM}https://console.anthropic.com/settings/keys${RST}"
    echo -e "  ${R2}│${RST}    ${R3}4)${RST} OpenRouter     ${DIM}https://openrouter.ai/keys${RST}"
    echo -e "  ${R2}│${RST}"
    prompt "Choice [1-4]: "
    read -r CHOICE

    case "$CHOICE" in
        1) KEY_NAME="GEMINI_API_KEY";     MODEL_NAME="GEMINI_MODEL";     DEFAULT_MODEL="gemini-3.1-pro-preview" ;;
        2) KEY_NAME="OPENAI_API_KEY";     MODEL_NAME="OPENAI_MODEL";     DEFAULT_MODEL="gpt-5.4" ;;
        3) KEY_NAME="ANTHROPIC_API_KEY";  MODEL_NAME="ANTHROPIC_MODEL";  DEFAULT_MODEL="claude-sonnet-4-6" ;;
        4) KEY_NAME="OPENROUTER_API_KEY"; MODEL_NAME="OPENROUTER_MODEL"; DEFAULT_MODEL="openrouter/auto" ;;
        *) err "Invalid choice"; exit 1 ;;
    esac

    prompt "Paste your API key: "
    read -rs API_KEY
    echo ""
    if [ -z "$API_KEY" ]; then
        err "No API key provided"
        exit 1
    fi
    ok "API key received"

    prompt "Model (enter for ${DEFAULT_MODEL}): "
    read -r MODEL
    [ -z "$MODEL" ] && MODEL="$DEFAULT_MODEL"

    echo -e "  ${R2}│${RST}"

    # Generate PSK and auth token
    PSK=$(openssl rand -base64 32)
    AUTH=$(openssl rand -hex 16)

    # Write config (port auto-detected from transport mode)
    cat > "$ENV_FILE" <<ENVEOF
# DNS-CLAW Configuration
# Generated by setup.sh — edit with: dnsclaw config --edit

# LLM Provider
${KEY_NAME}="${API_KEY}"
${MODEL_NAME}="${MODEL}"

# Payload Encryption (AES-256-GCM)
TUNNEL_PSK="${PSK}"

# Client authentication (clients must present this token)
AUTH_TOKEN="${AUTH}"

# Transport (UDP is default; set ONE to true for TLS)
# Port is auto-detected: UDP=53, DoT=853, DoH=443
USE_DOT=false
USE_DOH=false

# Server address
DNS_SERVER_ADDR=127.0.0.1
INSECURE_SKIP_VERIFY=true

# TLS certs (only needed for DoT/DoH)
TLS_CERT=cert.pem
TLS_KEY=key.pem
ENVEOF

    chmod 600 "$ENV_FILE"
    ok "Config saved to $ENV_FILE"
    ok "Generated encryption key (TUNNEL_PSK)"
    ok "Generated auth token (AUTH_TOKEN)"
fi

echo -e "  ${R2}│${RST}"

# ── Transport choice ───────────────────────────────────────────────────────
if [ "$ADVANCED" -eq 1 ]; then
    prompt "Transport mode? [${BOLD}1${RST}] UDP  [2] DoT  [3] DoH: "
    read -r TRANSPORT

    case "$TRANSPORT" in
        2)
            sed -i.bak 's/USE_DOT=false/USE_DOT=true/' "$ENV_FILE"
            rm -f "$ENV_FILE.bak"
            ok "Transport: DNS-over-TLS (port 853)"
            NEED_CERTS=1
            ;;
        3)
            sed -i.bak 's/USE_DOH=false/USE_DOH=true/' "$ENV_FILE"
            sed -i.bak 's|^DNS_SERVER_ADDR=.*|DNS_SERVER_ADDR=https://127.0.0.1/dns-query|' "$ENV_FILE"
            rm -f "$ENV_FILE.bak"
            ok "Transport: DNS-over-HTTPS (port 443)"
            NEED_CERTS=1
            ;;
        *)
            ok "Transport: UDP (port 53)"
            ;;
    esac
else
    ok "Transport: UDP (port 53)"
    info "${DIM}Run ${RST}${BOLD}./setup.sh --advanced${RST}${DIM} to configure DoT or DoH${RST}"
fi

# ── Generate TLS certs if needed ───────────────────────────────────────────
if [ "${NEED_CERTS:-0}" = "1" ]; then
    echo -e "  ${R2}│${RST}"
    info "Generating TLS certificates..."
    openssl req -x509 -nodes -days 365 \
        -newkey rsa:2048 \
        -keyout key.pem \
        -out cert.pem \
        -subj "/CN=llm.local" \
        -addext "subjectAltName=DNS:llm.local,DNS:localhost,IP:127.0.0.1" \
        2>/dev/null
    ok "Generated cert.pem and key.pem"
fi

echo -e "  ${R2}│${RST}"

# ── Build ──────────────────────────────────────────────────────────────────
info "Building DNS-CLAW..."

CMAKE_ARGS="-DCMAKE_BUILD_TYPE=Release"

# Detect build tool
if command -v ninja &>/dev/null; then
    cmake -B build -G Ninja $CMAKE_ARGS 2>&1 | tail -1
    cmake --build build 2>&1 | tail -1
else
    cmake -B build $CMAKE_ARGS 2>&1 | tail -1
    cmake --build build 2>&1 | tail -1
fi

# Verify the build produced both binaries
if [ ! -x build/dnsclaw-server ] || [ ! -x build/dnsclaw ]; then
    err "Build failed — binaries not found in build/"
    info "Try a clean build: ${BOLD}rm -rf build && cmake -B build && cmake --build build${RST}"
    echo -e "  ${R2}└${DIM}──────────────────────────────────────────${RST}"
    exit 1
fi

ok "Build complete"
echo -e "  ${R2}│${RST}"

# ── Done ───────────────────────────────────────────────────────────────────
echo -e "  ${R2}│${RST}  ${BOLD}Ready to go!${RST}"
echo -e "  ${R2}│${RST}"

if [ "$IS_MAC" -eq 1 ]; then
    echo -e "  ${R2}│${RST}  Start the server:"
    echo -e "  ${R2}│${RST}    ${R3}\$ ./build/dnsclaw-server${RST}"
else
    echo -e "  ${R2}│${RST}  Start the server (port 53 needs root on Linux):"
    echo -e "  ${R2}│${RST}    ${R3}\$ sudo -E ./build/dnsclaw-server${RST}"
fi

echo -e "  ${R2}│${RST}"
echo -e "  ${R2}│${RST}  Then in another terminal:"
echo -e "  ${R2}│${RST}    ${R3}\$ ./build/dnsclaw${RST}"
echo -e "  ${R2}│${RST}"
echo -e "  ${R2}│${RST}  Or install system-wide:"
echo -e "  ${R2}│${RST}    ${R3}\$ sudo cmake --install build${RST}"

if [ "$IS_MAC" -eq 1 ]; then
    echo -e "  ${R2}│${RST}    ${R3}\$ dnsclaw-server${RST}  &  ${R3}dnsclaw${RST}"
else
    echo -e "  ${R2}│${RST}    ${R3}\$ sudo -E dnsclaw-server${RST}  &  ${R3}dnsclaw${RST}"
fi

echo -e "  ${R2}│${RST}"
echo -e "  ${R2}│${RST}  Settings:  ${R3}dnsclaw config${RST}"
echo -e "  ${R2}│${RST}"
echo -e "  ${R2}└${DIM}──────────────────────────────────────────${RST}"
echo ""
