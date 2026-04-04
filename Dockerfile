# ── Builder stage ────────────────────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git libssl-dev libcurl4-openssl-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build

# ── Runtime stage ────────────────────────────────────────────────────────────
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3t64 libcurl4t64 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /src/build/dnsclaw-server /usr/local/bin/dnsclaw-server
COPY --from=builder /src/build/dnsclaw /usr/local/bin/dnsclaw

# Sessions persist here — mount a volume to keep them across restarts
RUN mkdir -p /root/.config/dnsclaw/sessions

EXPOSE 53/udp
EXPOSE 853/tcp
EXPOSE 443/tcp

ENTRYPOINT ["dnsclaw-server"]
