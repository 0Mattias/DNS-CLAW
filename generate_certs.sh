#!/bin/bash
# Generate self-signed TLS certs for DoT / DoH local testing
set -e

openssl req -x509 -nodes -days 365 \
    -newkey rsa:2048 \
    -keyout key.pem \
    -out cert.pem \
    -subj "/CN=llm.local" \
    -addext "subjectAltName=DNS:llm.local,DNS:localhost,IP:127.0.0.1"

echo "Generated cert.pem and key.pem"
