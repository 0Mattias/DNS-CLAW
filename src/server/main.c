/*
 * server/main.c — DNS-CLAW Server Entry Point
 *
 * A DNS server that tunnels LLM interactions via TXT records.
 * Supports plain UDP, DNS-over-TLS (DoT), and DNS-over-HTTPS (DoH).
 */
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>

#include "config.h"
#include "crypto.h"
#include "protocol.h"
#include "server/log.h"
#include "server/session.h"
#include "server/transport.h"

/* ── Global state (owned by main, declared in transport.h) ────────────────── */

atomic_int      g_running = ATOMIC_VAR_INIT(1);
int             g_server_fd = -1;
server_config_t g_config;

/* ── Utility ──────────────────────────────────────────────────────────────── */

static char *load_env(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    if (v && v[0]) return strdup(v);
    return fallback ? strdup(fallback) : NULL;
}

static int safe_atoi(const char *s)
{
    if (!s || !*s) return -1;
    char *end;
    long val = strtol(s, &end, 10);
    if (*end != '\0' || val < 0 || val > 65535)
        return -1;
    return (int)val;
}

/* ── Signal handling ──────────────────────────────────────────────────────── */

static void sigint_main_handler(int sig)
{
    (void)sig;
    atomic_store(&g_running, 0);
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }
    const char msg[] = "\n\033[33m[server]\033[0m Shutting down...\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    sa.sa_handler = sigint_main_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Load .env files */
    load_dotenv("../.env");
    load_dotenv(".env");

    /* Read config */
    char *api_key = load_env("GEMINI_API_KEY", NULL);
    if (!api_key || !api_key[0]) {
        log_err("FATAL", "GEMINI_API_KEY not set in .env or environment");
        free(api_key);
        curl_global_cleanup();
        return 1;
    }
    strncpy(g_config.api_key, api_key, sizeof(g_config.api_key) - 1);
    free(api_key);

    char *model = load_env("GEMINI_MODEL", "gemini-3.1-pro-preview");
    strncpy(g_config.model, model, sizeof(g_config.model) - 1);
    free(model);

    g_config.use_dot = getenv("USE_DOT") && strcmp(getenv("USE_DOT"), "true") == 0;
    g_config.use_doh = getenv("USE_DOH") && strcmp(getenv("USE_DOH"), "true") == 0;

    char *port_str = load_env("SERVER_PORT", NULL);
    if (port_str) {
        g_config.port = safe_atoi(port_str);
        free(port_str);
    }

    if (g_config.port <= 0) {
        if (g_config.use_dot)      g_config.port = 853;
        else if (g_config.use_doh) g_config.port = 443;
        else                       g_config.port = 53;
    }

    char *cert = load_env("TLS_CERT", "cert.pem");
    strncpy(g_config.tls_cert, cert, sizeof(g_config.tls_cert) - 1);
    free(cert);

    char *key = load_env("TLS_KEY", "key.pem");
    strncpy(g_config.tls_key, key, sizeof(g_config.tls_key) - 1);
    free(key);

    /* Initialize payload encryption from PSK */
    char *psk = load_env("TUNNEL_PSK", NULL);
    if (tunnel_crypto_init(psk) < 0) {
        log_err("FATAL", "Failed to initialize encryption");
        free(psk);
        curl_global_cleanup();
        return 1;
    }
    free(psk);

    /* Banner */
    print_banner_to(stderr);
    fprintf(stderr, CLR_R4 "  DNS-CLAW Server" CLR_DIM "  v" DNS_CLAW_VERSION CLR_RESET "\n\n");

    log_info("config", "Model:     %s", g_config.model);
    log_info("config", "Transport: %s",
             g_config.use_doh ? "DoH (HTTPS)" :
             g_config.use_dot ? "DoT (TLS)" : "UDP (plain)");
    log_info("config", "Encryption: %s",
             tunnel_crypto_enabled() ? "AES-256-GCM (PSK)" : "none (plaintext)");
    log_info("config", "Port:      %d", g_config.port);
    fprintf(stderr, "\n");

    /* Start session reaper */
    pthread_t reaper;
    pthread_create(&reaper, NULL, session_reaper_thread, NULL);
    pthread_detach(reaper);

    if (g_config.use_doh) {
        doh_server_thread(NULL);
    } else if (g_config.use_dot) {
        dot_server_thread(NULL);
    } else {
        udp_server_thread(NULL);
    }

    /* Cleanup all sessions on exit */
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].active)
            session_destroy(&g_sessions[i]);
    }
    pthread_mutex_unlock(&g_lock);

    curl_global_cleanup();
    log_ok("server", "Shutdown complete.");
    return 0;
}
