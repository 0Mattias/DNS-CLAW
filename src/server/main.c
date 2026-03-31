/*
 * server/main.c — DNS-CLAW Server Entry Point
 *
 * A DNS server that tunnels LLM interactions via TXT records.
 * Supports plain UDP, DNS-over-TLS (DoT), and DNS-over-HTTPS (DoH).
 * Multi-provider: Gemini, OpenAI, Anthropic (Claude), OpenRouter.
 */
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <curl/curl.h>

#include "config.h"
#include "crypto.h"
#include "protocol.h"
#include "server/llm.h"
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

    /* ── Multi-Provider Detection ─────────────────────────────────────────── */
    {
        static const char *KEY_ENVS[] = {
            "GEMINI_API_KEY", "OPENAI_API_KEY",
            "ANTHROPIC_API_KEY", "OPENROUTER_API_KEY"
        };
        static const char *MODEL_ENVS[] = {
            "GEMINI_MODEL", "OPENAI_MODEL",
            "ANTHROPIC_MODEL", "OPENROUTER_MODEL"
        };

        int provider_found = 0;

        /* Check explicit LLM_PROVIDER override */
        const char *explicit_prov = getenv("LLM_PROVIDER");
        if (explicit_prov && explicit_prov[0]) {
            static const char *slugs[] = {
                "gemini", "openai", "anthropic", "openrouter"
            };
            for (int i = 0; i < NUM_PROVIDERS; i++) {
                if (strcasecmp(explicit_prov, slugs[i]) == 0) {
                    char *k = load_env(KEY_ENVS[i], NULL);
                    if (k && k[0]) {
                        g_config.provider = (llm_provider_t)i;
                        strncpy(g_config.api_key, k,
                                sizeof(g_config.api_key) - 1);
                        free(k);
                        char *m = load_env(MODEL_ENVS[i],
                                           PROVIDER_DEFAULT_MODELS[i]);
                        strncpy(g_config.model, m,
                                sizeof(g_config.model) - 1);
                        free(m);
                        provider_found = 1;
                    } else {
                        log_err("FATAL", "LLM_PROVIDER=%s but %s is not set",
                                explicit_prov, KEY_ENVS[i]);
                        free(k);
                        curl_global_cleanup();
                        return 1;
                    }
                    break;
                }
            }
            if (!provider_found) {
                log_err("FATAL",
                    "Unknown LLM_PROVIDER '%s' "
                    "(use gemini/openai/anthropic/openrouter)",
                    explicit_prov);
                curl_global_cleanup();
                return 1;
            }
        }

        /* Auto-detect: check each key in priority order */
        if (!provider_found) {
            for (int i = 0; i < NUM_PROVIDERS; i++) {
                char *k = load_env(KEY_ENVS[i], NULL);
                if (k && k[0]) {
                    g_config.provider = (llm_provider_t)i;
                    strncpy(g_config.api_key, k,
                            sizeof(g_config.api_key) - 1);
                    free(k);
                    char *m = load_env(MODEL_ENVS[i],
                                       PROVIDER_DEFAULT_MODELS[i]);
                    strncpy(g_config.model, m, sizeof(g_config.model) - 1);
                    free(m);
                    provider_found = 1;
                    break;
                }
                free(k);
            }
        }

        /* Interactive setup wizard if no key found */
        if (!provider_found) {
            fprintf(stderr, "\n");
            fprintf(stderr, CLR_R2 "  \xe2\x94\x8c\xe2\x94\x80 No API key found " CLR_DIM
                "────────────────────────────────────" CLR_RESET "\n");
            fprintf(stderr, CLR_R2 "  │" CLR_RESET "\n");
            fprintf(stderr, CLR_R2 "  │" CLR_RESET
                "  Select a provider:\n");
            fprintf(stderr, CLR_R2 "  │" CLR_RESET "    " CLR_R3
                "1)" CLR_RESET " Gemini\n");
            fprintf(stderr, CLR_R2 "  │" CLR_RESET "    " CLR_R3
                "2)" CLR_RESET " OpenAI\n");
            fprintf(stderr, CLR_R2 "  │" CLR_RESET "    " CLR_R3
                "3)" CLR_RESET " Claude (Anthropic)\n");
            fprintf(stderr, CLR_R2 "  │" CLR_RESET "    " CLR_R3
                "4)" CLR_RESET " OpenRouter\n");
            fprintf(stderr, CLR_R2 "  │" CLR_RESET "\n");
            fprintf(stderr, CLR_R2 "  │" CLR_RESET "  Choice [1-4]: ");
            fflush(stderr);

            char choice_buf[16];
            if (!fgets(choice_buf, sizeof(choice_buf), stdin)) {
                log_err("FATAL", "No input received");
                curl_global_cleanup();
                return 1;
            }
            int choice = atoi(choice_buf);
            if (choice < 1 || choice > 4) {
                log_err("FATAL", "Invalid choice");
                curl_global_cleanup();
                return 1;
            }
            int idx = choice - 1;
            g_config.provider = (llm_provider_t)idx;

            fprintf(stderr, CLR_R2 "  │" CLR_RESET "  Paste your API key: ");
            fflush(stderr);
            char key_buf[512];
            if (!fgets(key_buf, sizeof(key_buf), stdin) ||
                strlen(key_buf) < 2) {
                log_err("FATAL", "No API key provided");
                curl_global_cleanup();
                return 1;
            }
            size_t klen = strlen(key_buf);
            while (klen > 0 && (key_buf[klen - 1] == '\n' ||
                   key_buf[klen - 1] == '\r'))
                key_buf[--klen] = '\0';
            strncpy(g_config.api_key, key_buf,
                    sizeof(g_config.api_key) - 1);

            fprintf(stderr, CLR_R2 "  │" CLR_RESET "  Model (enter for %s): ",
                    PROVIDER_DEFAULT_MODELS[idx]);
            fflush(stderr);
            char model_buf[128];
            if (fgets(model_buf, sizeof(model_buf), stdin)) {
                size_t mlen = strlen(model_buf);
                while (mlen > 0 && (model_buf[mlen - 1] == '\n' ||
                       model_buf[mlen - 1] == '\r'))
                    model_buf[--mlen] = '\0';
                if (mlen > 0)
                    strncpy(g_config.model, model_buf,
                            sizeof(g_config.model) - 1);
                else
                    strncpy(g_config.model, PROVIDER_DEFAULT_MODELS[idx],
                            sizeof(g_config.model) - 1);
            } else {
                strncpy(g_config.model, PROVIDER_DEFAULT_MODELS[idx],
                        sizeof(g_config.model) - 1);
            }

            /* Save to .env */
            FILE *envf = fopen(".env", "a");
            if (envf) {
                fprintf(envf, "\n# Added by DNS-CLAW setup wizard\n");
                fprintf(envf, "%s=\"%s\"\n",
                        KEY_ENVS[idx], g_config.api_key);
                fprintf(envf, "%s=\"%s\"\n",
                        MODEL_ENVS[idx], g_config.model);
                fclose(envf);
                fprintf(stderr, CLR_R2 "  │" CLR_RESET "\n");
                fprintf(stderr, CLR_R2 "  │  " CLR_R3
                    "\xe2\x9c\x93" CLR_RESET " Saved to .env\n");
            }
            fprintf(stderr, CLR_R2 "  \xe2\x94\x94" CLR_DIM
                "───────────────────────────────────────────────"
                CLR_RESET "\n\n");
        }

        strncpy(g_config.provider_name,
                PROVIDER_NAMES[g_config.provider],
                sizeof(g_config.provider_name) - 1);
    }

    /* ── Standard config ─────────────────────────────────────────────────── */
    g_config.use_dot = getenv("USE_DOT") &&
                       strcmp(getenv("USE_DOT"), "true") == 0;
    g_config.use_doh = getenv("USE_DOH") &&
                       strcmp(getenv("USE_DOH"), "true") == 0;

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
    fprintf(stderr, CLR_R4 "  DNS-CLAW Server" CLR_DIM
            "  v" DNS_CLAW_VERSION CLR_RESET "\n\n");

    log_info("config", "Provider:  %s", g_config.provider_name);
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
