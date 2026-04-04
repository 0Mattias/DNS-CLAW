/*
 * client/ui.c — Banner, help, usage, configuration display
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "config.h"
#include "crypto.h"
#include "client/render.h"
#include "client/ui.h"

void print_banner(void)
{
    print_banner_to(stdout);

    set_fg_rgb(THEME_R4);
    printf("  C Language Agentic Wireformat");
    set_fg_rgb(THEME_DIM);
    printf("  v%s\n", DNS_CLAW_VERSION);
    printf(ANSI_RESET);

    set_fg_rgb(THEME_DIM);
    printf("  ──────────────────────────────────────────────────────────────────\n");
    printf(ANSI_RESET);
}

void print_help(void)
{
    printf("\n");
    set_fg_rgb(THEME_R1);
    printf(ANSI_BOLD "  Commands\n" ANSI_RESET);
    set_fg_rgb(THEME_DIM);
    printf("  ──────────────────────────────────────────\n" ANSI_RESET);

    const char *cmds[][2] = {
        {"/help", "Show this help"},
        {"/clear", "Start a new chat session"},
        {"/compact [focus]", "Compact conversation context"},
        {"/export [file]", "Export conversation to markdown"},
        {"/config", "Show current configuration"},
        {"/status", "Show session info"},
        {"/exit", "Exit the application"},
    };
    for (int i = 0; i < 7; i++) {
        set_fg_rgb(THEME_R2);
        printf("  %-20s", cmds[i][0]);
        printf(ANSI_RESET ANSI_DIM " %s\n" ANSI_RESET, cmds[i][1]);
    }
    printf("\n");
}

void print_usage(const char *argv0)
{
    printf("Usage: %s [options]\n", argv0);
    printf("       %s config [--edit | --set KEY=VALUE | --provider]\n\n", argv0);
    printf("Options:\n");
    printf("  -m <message>    One-shot mode: send message and exit\n");
    printf("  -s <server>     Server address (default: from .env)\n");
    printf("  -p <port>       Server port\n");
    printf("  --no-color      Disable ANSI colors\n");
    printf("  --no-typewriter Disable typewriter text effect\n");
    printf("  -h, --help      Show this help\n");
    printf("  -v, --version   Show version\n");
    printf("\nConfig subcommands:\n");
    printf("  config            Show current configuration\n");
    printf("  config --edit     Open config in $EDITOR\n");
    printf(
        "  config --set K=V  Set a config value (e.g. --set ANTHROPIC_MODEL=claude-sonnet-4-6)\n");
    printf("  config --provider Re-run interactive provider setup\n");
}

/* ── Config helpers ──────────────────────────────────────────────────────── */

static const char *config_path(void)
{
    static char path[512];
    const char *home = getenv("HOME");
    if (!home)
        return NULL;
    snprintf(path, sizeof(path), "%s/.config/dnsclaw/.env", home);
    return path;
}

void config_show(void)
{
    const char *path = config_path();

    printf("\n");
    set_fg_rgb(THEME_R1);
    printf(ANSI_BOLD "  Configuration\n" ANSI_RESET);
    set_fg_rgb(THEME_DIM);
    printf("  ──────────────────────────────────────────\n" ANSI_RESET);

    /* Config file location */
    set_fg_rgb(THEME_R2);
    printf("  Config file:  " ANSI_RESET);
    if (path && access(path, F_OK) == 0)
        printf("%s\n", path);
    else
        printf(ANSI_DIM "(not found)" ANSI_RESET "\n");

    /* Detect provider */
    const char *provider = "none";
    const char *model = "default";
    const char *key_names[] = {"GEMINI_API_KEY", "OPENAI_API_KEY", "ANTHROPIC_API_KEY",
                               "OPENROUTER_API_KEY"};
    const char *prov_names[] = {"Gemini", "OpenAI", "Claude (Anthropic)", "OpenRouter"};
    const char *model_envs[] = {"GEMINI_MODEL", "OPENAI_MODEL", "ANTHROPIC_MODEL",
                                "OPENROUTER_MODEL"};

    /* Check explicit provider first */
    const char *explicit = getenv("LLM_PROVIDER");
    int found = -1;
    if (explicit && explicit[0]) {
        const char *slugs[] = {"gemini", "openai", "anthropic", "openrouter"};
        for (int i = 0; i < 4; i++) {
            if (strcasecmp(explicit, slugs[i]) == 0) {
                found = i;
                break;
            }
        }
    }
    /* Auto-detect */
    if (found < 0) {
        for (int i = 0; i < 4; i++) {
            const char *k = getenv(key_names[i]);
            if (k && k[0] && strcmp(k, "your-api-key-here") != 0) {
                found = i;
                break;
            }
        }
    }

    if (found >= 0) {
        provider = prov_names[found];
        const char *m = getenv(model_envs[found]);
        if (m && m[0])
            model = m;
    }

    set_fg_rgb(THEME_R2);
    printf("  Provider:     " ANSI_RESET "%s\n", provider);
    set_fg_rgb(THEME_R2);
    printf("  Model:        " ANSI_RESET "%s\n", model);

    /* API key (masked) */
    if (found >= 0) {
        const char *key = getenv(key_names[found]);
        set_fg_rgb(THEME_R2);
        printf("  API key:      " ANSI_RESET);
        if (key && strlen(key) > 8) {
            printf("%.4s...%s\n", key, key + strlen(key) - 4);
        } else {
            printf(ANSI_DIM "(not set)" ANSI_RESET "\n");
        }
    }

    /* Transport */
    const char *env;
    int use_dot = (env = getenv("USE_DOT")) && strcmp(env, "true") == 0;
    int use_doh = (env = getenv("USE_DOH")) && strcmp(env, "true") == 0;
    set_fg_rgb(THEME_R2);
    printf("  Transport:    " ANSI_RESET "%s\n", use_doh   ? "DoH (HTTPS)"
                                                 : use_dot ? "DoT (TLS)"
                                                           : "UDP");

    /* Port */
    const char *port_env = getenv("SERVER_PORT");
    int port = 0;
    if (port_env && port_env[0])
        port = atoi(port_env);
    if (port <= 0) {
        if (use_dot)
            port = 853;
        else if (use_doh)
            port = 443;
        else
            port = 53;
    }
    set_fg_rgb(THEME_R2);
    printf("  Port:         " ANSI_RESET "%d%s\n", port,
           (!port_env || !port_env[0]) ? " (auto)" : "");

    /* Server address */
    const char *addr = getenv("DNS_SERVER_ADDR");
    if (!addr || !addr[0]) {
        if (use_doh)
            addr = "https://127.0.0.1/dns-query";
        else
            addr = "127.0.0.1";
    }
    set_fg_rgb(THEME_R2);
    printf("  Server:       " ANSI_RESET "%s\n", addr);

    /* Encryption */
    set_fg_rgb(THEME_R2);
    printf("  Encryption:   " ANSI_RESET "%s\n",
           tunnel_crypto_enabled() ? "AES-256-GCM (PSK)" : "none");

    /* System prompt */
    const char *sp = getenv("SYSTEM_PROMPT");
    if (sp && sp[0]) {
        set_fg_rgb(THEME_R2);
        printf("  System prompt:" ANSI_RESET " %.60s%s\n", sp, strlen(sp) > 60 ? "..." : "");
    }

    printf("\n");
    set_fg_rgb(THEME_DIM);
    printf("  Edit:  dnsclaw config --edit\n");
    printf("  Set:   dnsclaw config --set KEY=VALUE\n");
    printf("  Swap:  dnsclaw config --provider\n");
    printf(ANSI_RESET "\n");
}

void config_edit(void)
{
    const char *path = config_path();
    if (!path) {
        fprintf(stderr, "Error: HOME not set\n");
        return;
    }

    /* Ensure config dir + file exist */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config/dnsclaw", getenv("HOME"));
    mkdir(dir, 0700);

    if (access(path, F_OK) != 0) {
        /* Create from template */
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "# DNS-CLAW Configuration\n");
            fprintf(f, "# See: dnsclaw config --help\n\n");
            fprintf(f, "# LLM Provider (set one API key — auto-detected)\n");
            fprintf(f, "# GEMINI_API_KEY=\"\"\n");
            fprintf(f, "# OPENAI_API_KEY=\"\"\n");
            fprintf(f, "# ANTHROPIC_API_KEY=\"\"\n");
            fprintf(f, "# OPENROUTER_API_KEY=\"\"\n\n");
            fprintf(f, "# Encryption (recommended)\n");
            fprintf(f, "# TUNNEL_PSK=\"\"\n\n");
            fprintf(f, "# Transport: UDP (default), USE_DOT=true, or USE_DOH=true\n");
            fprintf(f, "# Port is auto-detected from transport if not set\n");
            fclose(f);
            chmod(path, 0600);
        }
    }

    const char *editor = getenv("EDITOR");
    if (!editor)
        editor = getenv("VISUAL");
    if (!editor)
        editor = "vi";

    pid_t pid = fork();
    if (pid == 0) {
        execlp(editor, editor, path, (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    } else {
        fprintf(stderr, "Error: fork() failed\n");
    }
}

int config_set(const char *key_value)
{
    const char *eq = strchr(key_value, '=');
    if (!eq || eq == key_value) {
        fprintf(stderr, "Error: Expected KEY=VALUE format\n");
        return 1;
    }

    char key[128] = {0};
    size_t klen = (size_t)(eq - key_value);
    if (klen >= sizeof(key))
        klen = sizeof(key) - 1;
    memcpy(key, key_value, klen);

    const char *value = eq + 1;

    const char *path = config_path();
    if (!path) {
        fprintf(stderr, "Error: HOME not set\n");
        return 1;
    }

    /* Ensure dir exists */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config/dnsclaw", getenv("HOME"));
    mkdir(dir, 0700);

    /* Read existing file */
    char lines[256][1024];
    int nlines = 0;
    int found = 0;

    FILE *f = fopen(path, "r");
    if (f) {
        while (nlines < 256 && fgets(lines[nlines], sizeof(lines[0]), f)) {
            /* Check if this line sets the same key */
            char *p = lines[nlines];
            while (*p == ' ' || *p == '\t')
                p++;
            if (*p != '#' && *p != '\n' && *p != '\0') {
                char *e = strchr(p, '=');
                if (e) {
                    size_t lklen = (size_t)(e - p);
                    /* Trim trailing whitespace from key */
                    while (lklen > 0 && (p[lklen - 1] == ' ' || p[lklen - 1] == '\t'))
                        lklen--;
                    if (lklen == strlen(key) && strncmp(p, key, lklen) == 0) {
                        snprintf(lines[nlines], sizeof(lines[0]), "%s=\"%s\"\n", key, value);
                        found = 1;
                    }
                }
            }
            /* Also uncomment matching commented lines */
            if (!found && *p == '#') {
                p++;
                while (*p == ' ' || *p == '\t')
                    p++;
                char *e = strchr(p, '=');
                if (e) {
                    size_t lklen = (size_t)(e - p);
                    while (lklen > 0 && (p[lklen - 1] == ' ' || p[lklen - 1] == '\t'))
                        lklen--;
                    if (lklen == strlen(key) && strncmp(p, key, lklen) == 0) {
                        snprintf(lines[nlines], sizeof(lines[0]), "%s=\"%s\"\n", key, value);
                        found = 1;
                    }
                }
            }
            nlines++;
        }
        fclose(f);
    }

    if (!found) {
        snprintf(lines[nlines], sizeof(lines[0]), "%s=\"%s\"\n", key, value);
        nlines++;
    }

    f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "Error: Cannot write %s\n", path);
        return 1;
    }
    for (int i = 0; i < nlines; i++)
        fputs(lines[i], f);
    fclose(f);
    chmod(path, 0600);

    set_fg_rgb(THEME_R3);
    printf("  ✓ " ANSI_RESET);
    printf("Set %s in %s\n", key, path);
    return 0;
}

int config_provider_interactive(void)
{
    static const char *key_names[] = {"GEMINI_API_KEY", "OPENAI_API_KEY", "ANTHROPIC_API_KEY",
                                      "OPENROUTER_API_KEY"};
    static const char *model_names[] = {"GEMINI_MODEL", "OPENAI_MODEL", "ANTHROPIC_MODEL",
                                        "OPENROUTER_MODEL"};
    static const char *defaults[] = {"gemini-3.1-pro-preview", "gpt-5.4", "claude-sonnet-4-6",
                                     "openrouter/auto"};
    static const char *labels[] = {"Gemini", "OpenAI", "Claude (Anthropic)", "OpenRouter"};

    printf("\n");
    set_fg_rgb(THEME_R1);
    printf(ANSI_BOLD "  Provider Setup\n" ANSI_RESET);
    set_fg_rgb(THEME_DIM);
    printf("  ──────────────────────────────────────────\n" ANSI_RESET);
    printf("\n");

    for (int i = 0; i < 4; i++) {
        set_fg_rgb(THEME_R3);
        printf("    %d) " ANSI_RESET "%s\n", i + 1, labels[i]);
    }
    printf("\n");
    set_fg_rgb(THEME_R2);
    printf("  Choice [1-4]: " ANSI_RESET);
    fflush(stdout);

    char buf[16];
    if (!fgets(buf, sizeof(buf), stdin))
        return 1;
    int choice = atoi(buf);
    if (choice < 1 || choice > 4) {
        fprintf(stderr, "  Invalid choice\n");
        return 1;
    }
    int idx = choice - 1;

    printf("  Paste your API key: ");
    fflush(stdout);

    /* Disable echo so the API key isn't visible on screen */
    struct termios old_term, new_term;
    int have_term = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old_term) == 0;
    if (have_term) {
        new_term = old_term;
        new_term.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &new_term);
    }

    char key_buf[512] = {0};
    int got_key = fgets(key_buf, sizeof(key_buf), stdin) != NULL && strlen(key_buf) >= 2;

    if (have_term) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
        printf("\n");
    }

    if (!got_key) {
        fprintf(stderr, "  No API key provided\n");
        return 1;
    }
    /* Trim newline */
    size_t klen = strlen(key_buf);
    while (klen > 0 && (key_buf[klen - 1] == '\n' || key_buf[klen - 1] == '\r'))
        key_buf[--klen] = '\0';

    printf("  Model (enter for %s): ", defaults[idx]);
    fflush(stdout);
    char model_buf[128] = {0};
    if (fgets(model_buf, sizeof(model_buf), stdin)) {
        size_t mlen = strlen(model_buf);
        while (mlen > 0 && (model_buf[mlen - 1] == '\n' || model_buf[mlen - 1] == '\r'))
            model_buf[--mlen] = '\0';
    }
    if (!model_buf[0])
        strncpy(model_buf, defaults[idx], sizeof(model_buf) - 1);

    /* Save via config_set */
    char kv[600];
    snprintf(kv, sizeof(kv), "%s=%s", key_names[idx], key_buf);
    config_set(kv);

    snprintf(kv, sizeof(kv), "%s=%s", model_names[idx], model_buf);
    config_set(kv);

    printf("\n");
    set_fg_rgb(THEME_R3);
    printf("  ✓ " ANSI_RESET "Provider configured: %s (%s)\n", labels[idx], model_buf);
    printf(ANSI_DIM "    Restart the server to apply changes.\n" ANSI_RESET);
    printf("\n");
    return 0;
}
