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
#include <time.h>
#include <unistd.h>

#ifdef HAVE_LIBEDIT
#include <histedit.h>
#endif

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

#ifdef HAVE_LIBEDIT
static const char *el_prompt_null(EditLine *e)
{
    (void)e;
    return "";
}
#endif

/* ── Conversation export ─────────────────────────────────────────────────── */

#define MAX_EXPORT_ENTRIES 4096
static struct {
    const char *role;
    char *text;
} g_export_log[MAX_EXPORT_ENTRIES];
static int g_export_count = 0;

void export_log_add(const char *role, const char *text)
{
    if (g_export_count >= MAX_EXPORT_ENTRIES)
        return;
    g_export_log[g_export_count].role = role;
    g_export_log[g_export_count].text = strdup(text);
    g_export_count++;
}

static int export_conversation(const char *path)
{
    char filepath[512];
    if (path && path[0]) {
        strncpy(filepath, path, sizeof(filepath) - 1);
        filepath[sizeof(filepath) - 1] = '\0';
    } else {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        snprintf(filepath, sizeof(filepath), "dnsclaw-chat-%04d%02d%02d-%02d%02d%02d.md",
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min,
                 tm->tm_sec);
    }

    FILE *fp = fopen(filepath, "w");
    if (!fp)
        return -1;

    fprintf(fp, "# DNS-CLAW Conversation Export\n\n");
    fprintf(fp, "**Session:** `%s`  \n", g_session_id);
    fprintf(fp, "**Turns:** %d  \n\n---\n\n", g_turn);

    for (int i = 0; i < g_export_count; i++) {
        if (strcmp(g_export_log[i].role, "user") == 0) {
            fprintf(fp, "## You\n\n%s\n\n", g_export_log[i].text);
        } else {
            fprintf(fp, "## Agent\n\n%s\n\n", g_export_log[i].text);
        }
    }
    fclose(fp);

    set_fg_rgb(THEME_R2);
    printf("\n  ✓ Exported %d messages to %s\n" ANSI_RESET, g_export_count, filepath);
    return 0;
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

    /* Load .env from multiple locations (first match wins per variable) */
    const char *home = getenv("HOME");
    if (home) {
        char config_env[512];
        snprintf(config_env, sizeof(config_env), "%s/.config/dnsclaw/.env", home);
        load_dotenv(config_env);
    }
    load_dotenv(".env");
    load_dotenv("../.env");

    /* ── Config subcommand (before option parsing) ────────────── */
    if (argc >= 2 && strcmp(argv[1], "config") == 0) {
        /* Initialize crypto so config_show can report encryption status */
        const char *psk = getenv("TUNNEL_PSK");
        tunnel_crypto_init(psk);

        if (argc == 2) {
            config_show();
            return 0;
        }
        if (strcmp(argv[2], "--edit") == 0) {
            config_edit();
            return 0;
        }
        if (strcmp(argv[2], "--set") == 0 && argc >= 4) {
            return config_set(argv[3]);
        }
        if (strcmp(argv[2], "--provider") == 0) {
            return config_provider_interactive();
        }
        fprintf(stderr, "Unknown config option: %s\n", argv[2]);
        print_usage(argv[0]);
        return 1;
    }

    /* Defaults */
    g_cfg.typewriter = 1;
    char *oneshot_msg = NULL;

    /* Parse CLI args */
    static struct option long_opts[] = {{"help", no_argument, NULL, 'h'},
                                        {"version", no_argument, NULL, 'v'},
                                        {"no-color", no_argument, NULL, 'C'},
                                        {"no-typewriter", no_argument, NULL, 'T'},
                                        {0, 0, 0, 0}};

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
            snprintf(g_cfg.server_addr, sizeof(g_cfg.server_addr), "https://127.0.0.1/dns-query");
        } else {
            snprintf(g_cfg.server_addr, sizeof(g_cfg.server_addr), "127.0.0.1");
        }
    }

    if (!g_cfg.port) {
        if (g_cfg.use_dot)
            g_cfg.port = 853;
        else if (g_cfg.use_doh)
            g_cfg.port = 443;
        else
            g_cfg.port = 53;

        if ((env = getenv("SERVER_PORT")) && env[0])
            g_cfg.port = (int)strtol(env, NULL, 10);
    }

    /* Non-interactive: disable typewriter */
    if (!g_is_tty)
        g_cfg.typewriter = 0;

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
            size_t n = fread(piped_input + total, 1, sizeof(piped_input) - 1 - total, stdin);
            if (n == 0)
                break;
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

#ifdef HAVE_LIBEDIT
    /* ── libedit setup (readline-compatible) ────────────────── */
    HistEvent hev;
    History *hist = history_init();
    history(hist, &hev, H_SETSIZE, 500);

    /* Load history from ~/.config/dnsclaw/history */
    char hist_path[512] = {0};
    if (home) {
        snprintf(hist_path, sizeof(hist_path), "%s/.config/dnsclaw/history", home);
        history(hist, &hev, H_LOAD, hist_path);
    }

    EditLine *el = el_init(argv[0], stdin, stdout, stderr);
    el_set(el, EL_EDITOR, "emacs");
    el_set(el, EL_HIST, history, hist);
    el_set(el, EL_SIGNAL, 1);
#endif

    char input[65536];

    for (;;) {
        g_interrupted = 0;

        /* Build prompt string */
        char prompt[256];
        snprintf(prompt, sizeof(prompt),
                 "\033[38;2;100;80;80m %d "
                 "\033[48;2;255;60;50m\033[38;2;255;255;255m\033[1m You \033[0m"
                 "\033[38;2;255;100;80m ❯ \033[0m",
                 g_turn + 1);

#ifdef HAVE_LIBEDIT
        /* Print prompt ourselves (libedit prompt callbacks can't handle raw ANSI) */
        printf("%s", prompt);
        fflush(stdout);
        el_set(el, EL_PROMPT, el_prompt_null);
        int line_len = 0;
        const char *line = el_gets(el, &line_len);
        if (!line) {
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

        /* Copy to input buffer */
        size_t ilen = (size_t)line_len;
        if (ilen >= sizeof(input))
            ilen = sizeof(input) - 1;
        memcpy(input, line, ilen);
        input[ilen] = '\0';
#else
        /* Fallback: basic fgets prompt */
        printf("%s", prompt);
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
        size_t ilen = strlen(input);
#endif

        /* Trim */
        while (ilen > 0 && (input[ilen - 1] == '\n' || input[ilen - 1] == '\r'))
            input[--ilen] = '\0';
        char *text = input;
        while (*text == ' ' || *text == '\t')
            text++;
        if (text[0] == '\0')
            continue;

#ifdef HAVE_LIBEDIT
        /* Add to history (skip empty and duplicate) */
        if (text[0])
            history(hist, &hev, H_ENTER, text);
#endif

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
        if (strcmp(text, "/config") == 0) {
            config_show();
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
            printf("  Transport:   %s\n", g_cfg.use_doh   ? "DoH (HTTPS)"
                                          : g_cfg.use_dot ? "DoT (TLS)"
                                                          : "UDP");
            printf("  Encryption:  %s\n", tunnel_crypto_enabled() ? "AES-256-GCM (PSK)" : "none");
            printf("  Server:      %s:%d\n", g_cfg.server_addr, g_cfg.port);
            printf("\n");
            continue;
        }
        if (strncmp(text, "/export", 7) == 0) {
            const char *path = text + 7;
            while (*path == ' ')
                path++;
            if (g_export_count == 0) {
                set_fg_rgb(255, 200, 0);
                printf("\n  No messages to export.\n" ANSI_RESET);
            } else {
                export_conversation(*path ? path : NULL);
            }
            printf("\n");
            continue;
        }
        if (strncmp(text, "/compact", 8) == 0) {
            const char *focus = text + 8;
            while (*focus == ' ')
                focus++;

            char compact_msg[4096];
            if (*focus) {
                snprintf(compact_msg, sizeof(compact_msg),
                         "SYSTEM: Summarize our conversation, keeping all important "
                         "context. Acknowledge compaction. Focus on: %s",
                         focus);
            } else {
                snprintf(compact_msg, sizeof(compact_msg),
                         "SYSTEM: Summarize our conversation, keeping all important "
                         "facts and context, then acknowledge compaction.");
            }
            set_fg_rgb(THEME_R1);
            printf("\n  [Compacting context...]\n" ANSI_RESET);
            text = compact_msg;
        }

        g_turn++;
        g_interrupted = 0;
        export_log_add("user", text);
        process_message_loop("user", text, NULL);
        printf("\n");
    }

#ifdef HAVE_LIBEDIT
    /* Save history */
    if (hist_path[0])
        history(hist, &hev, H_SAVE, hist_path);
    history_end(hist);
    el_end(el);
#endif

    curl_global_cleanup();
    return 0;
}
