/*
 * client/main.c — DNS-CLAW Client Entry Point
 *
 * A terminal chat client that communicates with the DNS-CLAW server
 * via DNS tunneling (UDP, DoT, DoH).
 */
#define _POSIX_C_SOURCE 200809L

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>

#include "config.h"
#include "crypto.h"
#include "protocol.h"
#include "client/protocol.h"
#include "client/render.h"
#include "client/ui.h"

/* ── Global state (owned by main, declared in render.h) ───────────────────── */

client_config_t g_cfg;
int g_is_tty = 0;

/* ── Signal handling ──────────────────────────────────────────────────────── */

static void sigint_handler(int sig)
{
    (void)sig;
    if (g_interrupted) {
        const char rst[] = "\n\033[0m";
        write(STDOUT_FILENO, rst, sizeof(rst) - 1);
        _exit(130);
    }
    g_interrupted = 1;
    write(STDOUT_FILENO, "\n", 1);
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_is_tty = isatty(STDIN_FILENO);

    /* Signal handling */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    load_dotenv("../.env");
    load_dotenv(".env");

    /* Defaults */
    g_cfg.typewriter = 1;
    char *oneshot_msg = NULL;

    /* Parse CLI args */
    static struct option long_opts[] = {
        {"help",           no_argument,       NULL, 'h'},
        {"version",        no_argument,       NULL, 'v'},
        {"no-color",       no_argument,       NULL, 'C'},
        {"no-typewriter",  no_argument,       NULL, 'T'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "hvm:s:p:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'h':
            print_usage(argv[0]);
            return 0;
        case 'v':
            printf("dns-claw %s\n", DNS_CLAW_VERSION);
            return 0;
        case 'm':
            oneshot_msg = optarg;
            break;
        case 's':
            strncpy(g_cfg.server_addr, optarg, sizeof(g_cfg.server_addr) - 1);
            break;
        case 'p':
            g_cfg.port = (int)strtol(optarg, NULL, 10);
            break;
        case 'C':
            g_cfg.no_color = 1;
            break;
        case 'T':
            g_cfg.typewriter = 0;
            break;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Config from env (only if not set by CLI) */
    const char *env;
    g_cfg.use_dot = (env = getenv("USE_DOT")) && strcmp(env, "true") == 0;
    g_cfg.use_doh = (env = getenv("USE_DOH")) && strcmp(env, "true") == 0;
    g_cfg.insecure = (env = getenv("INSECURE_SKIP_VERIFY")) && strcmp(env, "true") == 0;

    if (!g_cfg.server_addr[0]) {
        if ((env = getenv("DNS_SERVER_ADDR")) && env[0]) {
            strncpy(g_cfg.server_addr, env, sizeof(g_cfg.server_addr) - 1);
        } else if (g_cfg.use_doh) {
            strcpy(g_cfg.server_addr, "https://127.0.0.1/dns-query");
        } else {
            strcpy(g_cfg.server_addr, "127.0.0.1");
        }
    }

    if (!g_cfg.port) {
        if (g_cfg.use_dot)      g_cfg.port = 853;
        else if (g_cfg.use_doh) g_cfg.port = 443;
        else                    g_cfg.port = 53;

        if ((env = getenv("SERVER_PORT")) && env[0])
            g_cfg.port = (int)strtol(env, NULL, 10);
    }

    /* Non-interactive: disable typewriter */
    if (!g_is_tty) g_cfg.typewriter = 0;

    /* Initialize payload encryption from PSK */
    const char *psk = getenv("TUNNEL_PSK");
    if (tunnel_crypto_init(psk) < 0) {
        fprintf(stderr, "Error: Failed to initialize encryption\n");
        curl_global_cleanup();
        return 1;
    }

    /* ── Banner (only in interactive mode) ──────────────────── */
    if (g_is_tty && !oneshot_msg) {
        print_banner();
    }

    /* ── Init session ────────────────────────────────────────── */
    if (init_session(g_is_tty) < 0) {
        curl_global_cleanup();
        return 1;
    }

    /* ── One-shot mode ───────────────────────────────────────── */
    if (oneshot_msg) {
        g_turn++;
        process_message_loop("user", oneshot_msg, NULL);
        printf("\n");
        curl_global_cleanup();
        return 0;
    }

    /* ── Piped mode (non-interactive) ────────────────────────── */
    if (!g_is_tty) {
        char piped_input[65536] = {0};
        size_t total = 0;
        while (total < sizeof(piped_input) - 1) {
            size_t n = fread(piped_input + total, 1,
                             sizeof(piped_input) - 1 - total, stdin);
            if (n == 0) break;
            total += n;
        }
        piped_input[total] = '\0';
        if (total > 0) {
            g_turn++;
            process_message_loop("user", piped_input, NULL);
            printf("\n");
        }
        curl_global_cleanup();
        return 0;
    }

    /* ── Interactive REPL ────────────────────────────────────── */
    printf(ANSI_DIM);
    printf("  Type a message, /help for commands, Ctrl+C to interrupt.\n");
    printf(ANSI_RESET "\n");

    char input[4096];

    for (;;) {
        g_interrupted = 0;

        /* Prompt with turn counter */
        set_fg_rgb(THEME_DIM);
        printf(" %d ", g_turn + 1);
        set_bg_rgb(THEME_R1);
        set_fg_rgb(255, 255, 255);
        printf(ANSI_BOLD " You " ANSI_RESET);
        set_fg_rgb(THEME_R2);
        printf(" ❯ " ANSI_RESET);
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) {
            if (g_interrupted) {
                set_fg_rgb(THEME_DIM);
                printf("\n  Goodbye.\n" ANSI_RESET);
                break;
            }
            break;
        }

        if (g_interrupted) {
            g_interrupted = 0;
            continue;
        }

        /* Trim */
        size_t ilen = strlen(input);
        while (ilen > 0 && (input[ilen-1] == '\n' || input[ilen-1] == '\r'))
            input[--ilen] = '\0';
        char *text = input;
        while (*text == ' ' || *text == '\t') text++;
        if (text[0] == '\0') continue;

        /* ── Commands ──────────────────────────────────────── */
        if (strcmp(text, "/exit") == 0 || strcmp(text, "/quit") == 0) {
            set_fg_rgb(THEME_DIM);
            printf("\n  Goodbye.\n" ANSI_RESET);
            break;
        }
        if (strcmp(text, "/clear") == 0 || strcmp(text, "/new") == 0 ||
            strcmp(text, "/reset") == 0) {
            printf("\n");
            init_session(1);
            continue;
        }
        if (strcmp(text, "/help") == 0) {
            print_help();
            continue;
        }
        if (strcmp(text, "/status") == 0) {
            printf("\n");
            set_fg_rgb(THEME_R1);
            printf(ANSI_BOLD "  Session Status\n" ANSI_RESET);
            set_fg_rgb(THEME_DIM);
            printf("  ──────────────────────────────────────────\n" ANSI_RESET);
            printf("  Session ID:  %s\n", g_session_id);
            printf("  Turn:        %d\n", g_turn);
            printf("  Transport:   %s\n",
                   g_cfg.use_doh ? "DoH (HTTPS)" :
                   g_cfg.use_dot ? "DoT (TLS)" : "UDP");
            printf("  Encryption:  %s\n",
                   tunnel_crypto_enabled() ? "AES-256-GCM (PSK)" : "none");
            printf("  Server:      %s:%d\n", g_cfg.server_addr, g_cfg.port);
            printf("\n");
            continue;
        }
        if (strncmp(text, "/compact", 8) == 0) {
            const char *focus = text + 8;
            while (*focus == ' ') focus++;

            char compact_msg[4096];
            if (*focus) {
                snprintf(compact_msg, sizeof(compact_msg),
                    "SYSTEM: Summarize our conversation, keeping all important "
                    "context. Acknowledge compaction. Focus on: %s", focus);
            } else {
                strcpy(compact_msg,
                    "SYSTEM: Summarize our conversation, keeping all important "
                    "facts and context, then acknowledge compaction.");
            }
            set_fg_rgb(THEME_R1);
            printf("\n  [Compacting context...]\n" ANSI_RESET);
            text = compact_msg;
        }

        g_turn++;
        g_interrupted = 0;
        process_message_loop("user", text, NULL);
        printf("\n");
    }

    curl_global_cleanup();
    return 0;
}
