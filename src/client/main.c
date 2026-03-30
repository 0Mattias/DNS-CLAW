/*
 * client/main.c — DNS-CLAW Client
 *
 * A fully-featured terminal chat client that communicates with the
 * DNS-CLAW server via DNS tunneling (UDP, DoT, DoH).
 *
 * Features:
 *   - Gradient ASCII banner with version info
 *   - Async spinner during network I/O
 *   - ANSI markdown rendering (bold, italic, headings, code, blockquotes)
 *   - Agentic tool execution (bash, file read/write, ls) with user approval
 *   - Session management (/clear, /compact, /help, /status)
 *   - One-shot mode (-m "message")
 *   - Pipe support (auto-detect non-interactive stdin)
 *   - Graceful Ctrl+C handling
 *   - Response timing
 *   - Typewriter text reveal
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <cJSON.h>

#include "base32.h"
#include "base64.h"
#include "dns_proto.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Configuration & Global State
 * ═══════════════════════════════════════════════════════════════════════════ */

#define DNS_CLAW_VERSION "1.0.0"

static struct {
    char server_addr[256];
    int  port;
    int  use_dot;
    int  use_doh;
    int  insecure;
    int  no_color;
    int  typewriter;       /* enable typewriter reveal (default on for tty) */
} g_cfg;

static char g_session_id[64];
static int  g_msg_id = 0;
static int  g_turn   = 0;
static atomic_int g_interrupted = 0;

/* ═══════════════════════════════════════════════════════════════════════════
 * Signal Handler
 * ═══════════════════════════════════════════════════════════════════════════ */

static void sigint_handler(int sig)
{
    (void)sig;
    if (g_interrupted) {
        /* Second Ctrl+C = hard exit */
        const char rst[] = "\n\033[0m";
        write(STDOUT_FILENO, rst, sizeof(rst) - 1);
        _exit(130);
    }
    g_interrupted = 1;
    /* Write newline so blocking reads unblock */
    write(STDOUT_FILENO, "\n", 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * .env loader
 * ═══════════════════════════════════════════════════════════════════════════ */

static void load_dotenv(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        char *ke = eq - 1;
        while (ke > key && (*ke == ' ' || *ke == '\t')) *ke-- = '\0';
        while (*val == ' ' || *val == '\t' || *val == '"') val++;
        size_t vlen = strlen(val);
        while (vlen > 0 && (val[vlen-1] == '\n' || val[vlen-1] == '\r' ||
               val[vlen-1] == '"' || val[vlen-1] == ' '))
            val[--vlen] = '\0';
        setenv(key, val, 0);
    }
    fclose(f);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Terminal Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static int term_width(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

static int g_is_tty = 0;

#define ANSI_RESET    "\033[0m"
#define ANSI_BOLD     "\033[1m"
#define ANSI_DIM      "\033[2m"
#define ANSI_ITALIC   "\033[3m"
#define ANSI_ULINE    "\033[4m"
#define ANSI_STRIKE   "\033[9m"

static void set_fg_rgb(int r, int g, int b)
{
    if (!g_cfg.no_color) printf("\033[38;2;%d;%d;%dm", r, g, b);
}

static void set_bg_rgb(int r, int g, int b)
{
    if (!g_cfg.no_color) printf("\033[48;2;%d;%d;%dm", r, g, b);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Timing
 * ═══════════════════════════════════════════════════════════════════════════ */

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Typewriter Effect
 * ═══════════════════════════════════════════════════════════════════════════ */

static void typewriter_putchar(int ch)
{
    putchar(ch);
    if (g_cfg.typewriter && g_is_tty) {
        fflush(stdout);
        usleep(800);  /* 0.8ms per char — fast but visible */
    }
}

static void typewriter_puts(const char *s)
{
    while (*s) typewriter_putchar(*s++);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Spinner (runs in a separate thread)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    char            message[256];
    volatile int    running;
    pthread_t       thread;
    pthread_mutex_t msg_lock;
    double          start_time;
} spinner_t;

static const char *SPINNER_FRAMES[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"
};
/* Theme colors — warm red/crimson palette matching DNS CLAW banner */
#define THEME_R1  255, 60, 50     /* Primary accent (hot red) */
#define THEME_R2  255, 100, 80    /* Secondary accent (warm coral) */
#define THEME_R3  255, 140, 110   /* Tertiary (salmon) */
#define THEME_R4  255, 195, 180   /* Light accent (blush) */
#define THEME_DIM 100, 80, 80     /* Muted red-grey for borders */
#define THEME_TXT 255, 230, 225   /* Near-white warm text */
#define SPINNER_FRAME_COUNT 10

static void *spinner_run(void *arg)
{
    spinner_t *s = (spinner_t *)arg;
    int frame = 0;

    while (s->running && !g_interrupted) {
        pthread_mutex_lock(&s->msg_lock);
        double elapsed = (now_ms() - s->start_time) / 1000.0;

        printf("\r\033[K");
        set_fg_rgb(THEME_R1);
        printf(" %s ", SPINNER_FRAMES[frame % SPINNER_FRAME_COUNT]);
        printf(ANSI_DIM "%s" ANSI_RESET, s->message);
        set_fg_rgb(THEME_DIM);
        printf(" (%.1fs)" ANSI_RESET, elapsed);
        fflush(stdout);
        pthread_mutex_unlock(&s->msg_lock);

        frame++;
        usleep(100000);
    }
    printf("\r\033[K");
    fflush(stdout);
    return NULL;
}

static void spinner_start(spinner_t *s, const char *msg)
{
    strncpy(s->message, msg, sizeof(s->message) - 1);
    s->message[sizeof(s->message) - 1] = '\0';
    s->running = 1;
    s->start_time = now_ms();
    pthread_mutex_init(&s->msg_lock, NULL);
    pthread_create(&s->thread, NULL, spinner_run, s);
}

static void spinner_set_message(spinner_t *s, const char *msg)
{
    pthread_mutex_lock(&s->msg_lock);
    strncpy(s->message, msg, sizeof(s->message) - 1);
    s->message[sizeof(s->message) - 1] = '\0';
    pthread_mutex_unlock(&s->msg_lock);
}

static void spinner_stop(spinner_t *s)
{
    s->running = 0;
    pthread_join(s->thread, NULL);
    pthread_mutex_destroy(&s->msg_lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DNS Query Wrapper
 * ═══════════════════════════════════════════════════════════════════════════ */

static int do_dns_query(const char *qname, char *txt_out, size_t txt_out_len)
{
    if (g_interrupted) return -1;

    if (g_cfg.use_doh) {
        return dns_query_doh(g_cfg.server_addr, qname, g_cfg.insecure,
                             txt_out, txt_out_len);
    } else if (g_cfg.use_dot) {
        return dns_query_dot(g_cfg.server_addr, (uint16_t)g_cfg.port,
                             qname, g_cfg.insecure, txt_out, txt_out_len);
    } else {
        return dns_query_udp(g_cfg.server_addr, (uint16_t)g_cfg.port,
                             qname, txt_out, txt_out_len);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Session Init
 * ═══════════════════════════════════════════════════════════════════════════ */

static int init_session(int show_msg)
{
    spinner_t sp;
    spinner_start(&sp, "Initializing session...");

    char txt[DNS_MAX_TXT];
    int rc = do_dns_query("init.llm.local.", txt, sizeof(txt));

    spinner_stop(&sp);

    if (rc < 0) {
        set_fg_rgb(255, 80, 80);
        printf("  ✗ Failed to initialize session.\n" ANSI_RESET);
        return -1;
    }

    char *p = txt;
    while (*p == ' ' || *p == '\n' || *p == '\r') p++;
    strncpy(g_session_id, p, sizeof(g_session_id) - 1);
    size_t slen = strlen(g_session_id);
    while (slen > 0 && (g_session_id[slen-1] == ' ' ||
           g_session_id[slen-1] == '\n' || g_session_id[slen-1] == '\r'))
        g_session_id[--slen] = '\0';

    g_msg_id = 0;
    g_turn   = 0;

    if (show_msg) {
        set_fg_rgb(THEME_R2);
        printf("  ✓ " ANSI_RESET);
        printf(ANSI_DIM "Session: %s\n" ANSI_RESET, g_session_id);
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ANSI Markdown Renderer (improved)
 *   - Blockquotes (> text)
 *   - Nested numbered lists (multi-digit)
 *   - Indented sublists
 *   - Typewriter reveal
 * ═══════════════════════════════════════════════════════════════════════════ */

static void render_inline(const char *p)
{
    while (*p) {
        /* Bold: **text** */
        if (p[0] == '*' && p[1] == '*') {
            const char *end = strstr(p + 2, "**");
            if (end) {
                printf(ANSI_BOLD);
                for (const char *c = p + 2; c < end; c++)
                    typewriter_putchar(*c);
                printf(ANSI_RESET);
                p = end + 2;
                continue;
            }
        }
        /* Italic: *text* */
        if (p[0] == '*' && p[1] != '*') {
            const char *end = strchr(p + 1, '*');
            if (end && end != p + 1) {
                printf(ANSI_ITALIC);
                for (const char *c = p + 1; c < end; c++)
                    typewriter_putchar(*c);
                printf(ANSI_RESET);
                p = end + 1;
                continue;
            }
        }
        /* Strikethrough: ~~text~~ */
        if (p[0] == '~' && p[1] == '~') {
            const char *end = strstr(p + 2, "~~");
            if (end) {
                printf(ANSI_STRIKE);
                for (const char *c = p + 2; c < end; c++)
                    typewriter_putchar(*c);
                printf(ANSI_RESET);
                p = end + 2;
                continue;
            }
        }
        /* Inline code: `text` */
        if (p[0] == '`') {
            const char *end = strchr(p + 1, '`');
            if (end) {
                set_fg_rgb(THEME_R4);
                set_bg_rgb(50, 30, 30);
                typewriter_putchar(' ');
                for (const char *c = p + 1; c < end; c++)
                    typewriter_putchar(*c);
                typewriter_putchar(' ');
                printf(ANSI_RESET);
                p = end + 1;
                continue;
            }
        }
        /* Links: [text](url) — show text underlined */
        if (p[0] == '[') {
            const char *close = strchr(p + 1, ']');
            if (close && close[1] == '(') {
                const char *url_close = strchr(close + 2, ')');
                if (url_close) {
                    printf(ANSI_ULINE);
                    set_fg_rgb(THEME_R3);
                    for (const char *c = p + 1; c < close; c++)
                        typewriter_putchar(*c);
                    printf(ANSI_RESET);
                    p = url_close + 1;
                    continue;
                }
            }
        }
        typewriter_putchar(*p++);
    }
}

static void render_markdown(const char *text)
{
    int in_code_block = 0;
    const char *line = text;

    while (*line) {
        const char *eol = strchr(line, '\n');
        size_t llen = eol ? (size_t)(eol - line) : strlen(line);
        char buf[4096];
        if (llen >= sizeof(buf)) llen = sizeof(buf) - 1;
        memcpy(buf, line, llen);
        buf[llen] = '\0';

        /* Code block fences */
        if (strncmp(buf, "```", 3) == 0) {
            if (!in_code_block) {
                in_code_block = 1;
                set_fg_rgb(THEME_DIM);
                printf("  ┌─");
                if (buf[3]) {
                    set_fg_rgb(THEME_R2);
                    printf(" %s ", buf + 3);
                    set_fg_rgb(THEME_DIM);
                }
                int pad = term_width() - 10 - (buf[3] ? (int)strlen(buf+3) + 2 : 0);
                for (int i = 0; i < pad && i < 120; i++) printf("─");
                printf("┐\n" ANSI_RESET);
            } else {
                in_code_block = 0;
                set_fg_rgb(THEME_DIM);
                printf("  └");
                int pad = term_width() - 6;
                for (int i = 0; i < pad && i < 120; i++) printf("─");
                printf("┘\n" ANSI_RESET);
            }
            line = eol ? eol + 1 : line + llen;
            continue;
        }

        if (in_code_block) {
            set_fg_rgb(THEME_DIM);
            printf("  │ ");
            set_fg_rgb(THEME_TXT);
            printf("%s", buf);
            /* Pad to right border */
            int pad = term_width() - 8 - (int)strlen(buf);
            for (int i = 0; i < pad; i++) putchar(' ');
            set_fg_rgb(THEME_DIM);
            printf(" │\n" ANSI_RESET);
            line = eol ? eol + 1 : line + llen;
            continue;
        }

        /* Headings */
        if (buf[0] == '#') {
            int level = 0;
            while (buf[level] == '#' && level < 6) level++;
            const char *heading = buf + level;
            while (*heading == ' ') heading++;

            if (level == 1) {
                printf("\n");
                set_fg_rgb(THEME_R1);
                printf(ANSI_BOLD "  %s\n" ANSI_RESET, heading);
                set_fg_rgb(THEME_R1);
                printf("  ");
                for (int i = 0; i < (int)strlen(heading) && i < 120; i++)
                    printf("═");
                printf("\n" ANSI_RESET);
            } else if (level == 2) {
                printf("\n");
                set_fg_rgb(THEME_R2);
                printf(ANSI_BOLD "  %s\n" ANSI_RESET, heading);
                set_fg_rgb(THEME_DIM);
                printf("  ");
                for (int i = 0; i < (int)strlen(heading) && i < 120; i++)
                    printf("─");
                printf("\n" ANSI_RESET);
            } else {
                set_fg_rgb(THEME_R3);
                printf(ANSI_BOLD "  %s\n" ANSI_RESET, heading);
            }
            line = eol ? eol + 1 : line + llen;
            continue;
        }

        /* Blockquotes: > text */
        if (buf[0] == '>') {
            const char *qt = buf + 1;
            while (*qt == ' ') qt++;
            set_fg_rgb(THEME_DIM);
            printf("  ┃ ");
            set_fg_rgb(THEME_R4);
            printf(ANSI_ITALIC);
            typewriter_puts(qt);
            printf(ANSI_RESET "\n");
            line = eol ? eol + 1 : line + llen;
            continue;
        }

        /* Unordered list items (with indentation support) */
        {
            int indent = 0;
            const char *lp = buf;
            while (*lp == ' ') { lp++; indent++; }
            if ((*lp == '-' || *lp == '*' || *lp == '+') && lp[1] == ' ') {
                int spaces = 2 + (indent / 2) * 2;
                for (int i = 0; i < spaces; i++) putchar(' ');
                if (indent > 0) {
                    set_fg_rgb(THEME_R3);
                    printf("◦ ");
                } else {
                    set_fg_rgb(THEME_R2);
                    printf("• ");
                }
                printf(ANSI_RESET);
                render_inline(lp + 2);
                printf("\n");
                line = eol ? eol + 1 : line + llen;
                continue;
            }
        }

        /* Numbered list items (multi-digit support) */
        {
            const char *lp = buf;
            int indent = 0;
            while (*lp == ' ') { lp++; indent++; }
            if (isdigit((unsigned char)lp[0])) {
                const char *dot = lp;
                while (isdigit((unsigned char)*dot)) dot++;
                if (*dot == '.' && dot[1] == ' ') {
                    int spaces = 2 + (indent / 2) * 2;
                    for (int i = 0; i < spaces; i++) putchar(' ');
                    set_fg_rgb(THEME_R2);
                    fwrite(lp, 1, (size_t)(dot - lp + 1), stdout);
                    printf(" " ANSI_RESET);
                    render_inline(dot + 2);
                    printf("\n");
                    line = eol ? eol + 1 : line + llen;
                    continue;
                }
            }
        }

        /* Horizontal rules */
        if (strncmp(buf, "---", 3) == 0 || strncmp(buf, "***", 3) == 0 ||
            strncmp(buf, "___", 3) == 0) {
            set_fg_rgb(THEME_DIM);
            printf("  ");
            int w = term_width() - 4;
            if (w > 120) w = 120;
            for (int i = 0; i < w; i++) printf("─");
            printf("\n" ANSI_RESET);
            line = eol ? eol + 1 : line + llen;
            continue;
        }

        /* Empty line */
        if (buf[0] == '\0') {
            printf("\n");
            line = eol ? eol + 1 : line + llen;
            continue;
        }

        /* Regular text with inline formatting */
        printf("  ");
        render_inline(buf);
        printf("\n");

        line = eol ? eol + 1 : line + llen;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Banner
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_banner(void)
{
    static const char *ART[] = {
        "▓█████▄  ███▄    █   ██████     ▄████▄   ██▓    ▄▄▄       █     █░",
        "▒██▀ ██▌ ██ ▀█   █ ▒██    ▒    ▒██▀ ▀█  ▓██▒   ▒████▄    ▓█░ █ ░█░",
        "░██   █▌▓██  ▀█ ██▒░ ▓██▄      ▒▓█    ▄ ▒██░   ▒██  ▀█▄  ▒█░ █ ░█ ",
        "░▓█▄   ▌▓██▒  ▐▌██▒  ▒   ██▒   ▒▓▓▄ ▄██▒▒██░   ░██▄▄▄▄██ ░█░ █ ░█ ",
        "░▒████▓ ▒██░   ▓██░▒██████▒▒   ▒ ▓███▀ ░░██████▒▓█   ▓██▒░░██▒██▓ ",
        " ▒▒▓  ▒ ░ ▒░   ▒ ▒ ▒ ▒▓▒ ▒ ░   ░ ░▒ ▒  ░░ ▒░▓  ░▒▒   ▓▒█░░ ▓░▒ ▒  ",
        " ░ ▒  ▒ ░ ░░   ░ ▒░░ ░▒  ░ ░     ░  ▒   ░ ░ ▒  ░ ▒   ▒▒ ░  ▒ ░ ░  ",
        " ░ ░  ░    ░   ░ ░ ░  ░  ░     ░          ░ ░    ░   ▒     ░   ░  ",
        "   ░             ░       ░     ░ ░          ░  ░     ░  ░    ░    ",
        " ░                             ░                                  ",
    };

    /* Red → White gradient */
    int colors[][3] = {
        {255, 20,  20},   {255, 60,  50},   {255, 100, 80},
        {255, 140, 110},  {255, 170, 150},  {255, 195, 180},
        {255, 215, 210},  {255, 230, 225},  {255, 245, 242},
        {255, 255, 255},
    };

    printf("\n");
    for (int i = 0; i < 10; i++) {
        set_fg_rgb(colors[i][0], colors[i][1], colors[i][2]);
        printf("%s\n", ART[i]);
    }
    printf(ANSI_RESET "\n");

    set_fg_rgb(THEME_R4);
    printf("  C Language Agentic Wireformat");
    set_fg_rgb(THEME_DIM);
    printf("  v%s\n", DNS_CLAW_VERSION);
    printf(ANSI_RESET);

    set_fg_rgb(THEME_DIM);
    printf("  ──────────────────────────────────────────────────────────────────\n");
    printf(ANSI_RESET);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Help
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_help(void)
{
    printf("\n");
    set_fg_rgb(THEME_R1);
    printf(ANSI_BOLD "  Commands\n" ANSI_RESET);
    set_fg_rgb(THEME_DIM);
    printf("  ──────────────────────────────────────────\n" ANSI_RESET);

    const char *cmds[][2] = {
        {"/help",            "Show this help"},
        {"/clear",           "Start a new chat session"},
        {"/compact [focus]", "Compact conversation context"},
        {"/status",          "Show session info"},
        {"/exit",            "Exit the application"},
    };
    for (int i = 0; i < 5; i++) {
        set_fg_rgb(THEME_R2);
        printf("  %-20s", cmds[i][0]);
        printf(ANSI_RESET ANSI_DIM " %s\n" ANSI_RESET, cmds[i][1]);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Usage (for -h flag)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_usage(const char *argv0)
{
    printf("Usage: %s [options]\n", argv0);
    printf("Options:\n");
    printf("  -m <message>    One-shot mode: send message and exit\n");
    printf("  -s <server>     Server address (default: from .env)\n");
    printf("  -p <port>       Server port\n");
    printf("  --no-color      Disable ANSI colors\n");
    printf("  --no-typewriter Disable typewriter text effect\n");
    printf("  -h, --help      Show this help\n");
    printf("  -v, --version   Show version\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Message Processing Loop
 * ═══════════════════════════════════════════════════════════════════════════ */

static int process_message_loop(const char *type, const char *content,
                                const char *tool_name)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "type", type);
    cJSON_AddStringToObject(payload, "content", content);
    if (tool_name && tool_name[0])
        cJSON_AddStringToObject(payload, "tool_name", tool_name);

    double round_start = now_ms();

    while (1) {
        if (g_interrupted) {
            cJSON_Delete(payload);
            return -1;
        }

        g_msg_id++;
        int current_mid = g_msg_id;

        char spin_msg[128];
        snprintf(spin_msg, sizeof(spin_msg),
                 "Transmitting [msg %d]...", current_mid);
        spinner_t sp;
        spinner_start(&sp, spin_msg);

        char *payload_str = cJSON_PrintUnformatted(payload);
        size_t payload_len = strlen(payload_str);

        /* 1. Upload chunks (base32) */
        int chunk_size = 35;  /* base32(35 bytes) = 56 chars, fits DNS 63-char label limit */
        char b32_buf[256];
        char qname[1024];
        char txt[DNS_MAX_TXT];

        int seq = 0;
        int upload_ok = 1;
        for (size_t i = 0; i < payload_len && upload_ok; i += (size_t)chunk_size) {
            size_t clen = payload_len - i;
            if (clen > (size_t)chunk_size) clen = (size_t)chunk_size;

            base32_encode((uint8_t *)payload_str + i, clen,
                          b32_buf, sizeof(b32_buf));

            for (char *p = b32_buf; *p; p++)
                *p = (char)tolower((unsigned char)*p);

            snprintf(qname, sizeof(qname), "%s.%d.up.%d.%s.llm.local.",
                     b32_buf, seq, current_mid, g_session_id);

            if (do_dns_query(qname, txt, sizeof(txt)) < 0 ||
                strcmp(txt, "ACK") != 0) {
                upload_ok = 0;
            }
            seq++;
        }
        free(payload_str);

        if (!upload_ok) {
            spinner_stop(&sp);
            cJSON_Delete(payload);
            set_fg_rgb(255, 80, 80);
            printf("  ✗ Upload failed at chunk %d\n" ANSI_RESET, seq - 1);
            return -1;
        }

        /* 2. Send fin */
        snprintf(spin_msg, sizeof(spin_msg), "Waiting for agent...");
        spinner_set_message(&sp, spin_msg);

        snprintf(qname, sizeof(qname), "fin.%d.%s.llm.local.",
                 current_mid, g_session_id);
        if (do_dns_query(qname, txt, sizeof(txt)) < 0 ||
            strcmp(txt, "ACK") != 0) {
            spinner_stop(&sp);
            cJSON_Delete(payload);
            set_fg_rgb(255, 80, 80);
            printf("  ✗ Server did not ACK finalize\n" ANSI_RESET);
            return -1;
        }

        /* 3. Poll + download response */
        int down_seq = 0;
        char full_response[131072] = {0};
        size_t resp_pos = 0;
        int poll_count = 0;

        while (!g_interrupted) {
            snprintf(qname, sizeof(qname), "%d.%d.down.%s.llm.local.",
                     down_seq, current_mid, g_session_id);
            if (do_dns_query(qname, txt, sizeof(txt)) < 0) {
                spinner_stop(&sp);
                cJSON_Delete(payload);
                set_fg_rgb(255, 80, 80);
                printf("  ✗ Download failed at chunk %d\n" ANSI_RESET, down_seq);
                return -1;
            }

            if (strcmp(txt, "PENDING") == 0) {
                poll_count++;
                snprintf(spin_msg, sizeof(spin_msg),
                         "Agent is thinking... (poll %d)", poll_count);
                spinner_set_message(&sp, spin_msg);
                usleep(500000);
                continue;
            } else if (strcmp(txt, "EOF") == 0) {
                break;
            } else if (strncmp(txt, "ERR:", 4) == 0) {
                spinner_stop(&sp);
                cJSON_Delete(payload);
                set_fg_rgb(255, 80, 80);
                printf("  ✗ %s\n" ANSI_RESET, txt);
                return -1;
            }

            uint8_t decoded[4096];
            int dlen = base64_decode(txt, decoded, sizeof(decoded));
            if (dlen < 0) {
                spinner_stop(&sp);
                cJSON_Delete(payload);
                set_fg_rgb(255, 80, 80);
                printf("  ✗ Base64 decode error on chunk %d\n" ANSI_RESET, down_seq);
                return -1;
            }

            snprintf(spin_msg, sizeof(spin_msg),
                     "Receiving chunk %d...", down_seq);
            spinner_set_message(&sp, spin_msg);

            if (resp_pos + (size_t)dlen < sizeof(full_response)) {
                memcpy(full_response + resp_pos, decoded, (size_t)dlen);
                resp_pos += (size_t)dlen;
            }
            down_seq++;
        }
        full_response[resp_pos] = '\0';

        spinner_stop(&sp);

        if (g_interrupted) {
            cJSON_Delete(payload);
            return -1;
        }

        /* 4. Parse JSON response */
        cJSON *resp_json = cJSON_Parse(full_response);
        if (!resp_json) {
            cJSON_Delete(payload);
            set_fg_rgb(255, 80, 80);
            printf("  ✗ Malformed response JSON\n" ANSI_RESET);
            return -1;
        }

        const char *resp_type = cJSON_GetStringValue(
            cJSON_GetObjectItem(resp_json, "type"));

        if (resp_type && strcmp(resp_type, "tool_call") == 0) {
            /* ── Tool call ───────────────────────────────────────── */
            const char *fn = cJSON_GetStringValue(
                cJSON_GetObjectItem(resp_json, "tool_name"));
            cJSON *args = cJSON_GetObjectItem(resp_json, "tool_args");

            printf("\n");
            set_fg_rgb(THEME_R1);
            printf("  ┌ ");
            set_bg_rgb(THEME_R1);
            set_fg_rgb(0, 0, 0);
            printf(ANSI_BOLD " ⚡ Tool Call " ANSI_RESET);
            set_fg_rgb(THEME_R2);
            printf(" ── %s\n" ANSI_RESET, fn ? fn : "unknown");

            char tool_result[8192] = {0};

            if (fn && strcmp(fn, "client_execute_bash") == 0) {
                const char *cmd = cJSON_GetStringValue(
                    cJSON_GetObjectItem(args, "command"));
                if (!cmd) {
                    snprintf(tool_result, sizeof(tool_result),
                             "Error: Invalid command argument");
                } else {
                    set_fg_rgb(THEME_DIM);
                    printf("  │\n");
                    printf("  │ " ANSI_RESET);
                    set_fg_rgb(THEME_R4);
                    set_bg_rgb(50, 30, 30);
                    printf(" $ %s " ANSI_RESET, cmd);
                    printf("\n");
                    set_fg_rgb(THEME_DIM);
                    printf("  │\n" ANSI_RESET);

                    printf("  ");
                    set_fg_rgb(THEME_R4);
                    printf("  Allow? " ANSI_RESET);
                    set_fg_rgb(THEME_R2);
                    printf("[Y]es");
                    printf(ANSI_RESET " / ");
                    set_fg_rgb(THEME_DIM);
                    printf("[n]o" ANSI_RESET ": ");
                    fflush(stdout);

                    char confirm[32] = {0};
                    if (fgets(confirm, sizeof(confirm), stdin)) {
                        char *c = confirm;
                        while (*c == ' ') c++;

                        if (c[0] == 'y' || c[0] == 'Y' ||
                            c[0] == '\n' || c[0] == '\0') {
                            FILE *proc = popen(cmd, "r");
                            if (!proc) {
                                snprintf(tool_result, sizeof(tool_result),
                                         "Error: popen failed: %s", strerror(errno));
                            } else {
                                size_t total = 0;
                                while (total < sizeof(tool_result) - 1) {
                                    size_t n = fread(tool_result + total, 1,
                                                     sizeof(tool_result) - 1 - total,
                                                     proc);
                                    if (n == 0) break;
                                    total += n;
                                }
                                tool_result[total] = '\0';
                                int status = pclose(proc);
                                if (status != 0 && total == 0)
                                    snprintf(tool_result, sizeof(tool_result),
                                             "Command exited with status %d", status);
                                if (strlen(tool_result) == 0)
                                    strcpy(tool_result, "(no output)");
                            }
                        } else {
                            set_fg_rgb(THEME_R1);
                            printf("  ✗ Rejected\n" ANSI_RESET);
                            strcpy(tool_result,
                                "User rejected command execution. "
                                "Ask what they'd like instead.");
                        }
                    }
                    if (strlen(tool_result) > 2000)
                        strcpy(tool_result + 2000, "\n…[truncated]");
                }

            } else if (fn && strcmp(fn, "client_read_file") == 0) {
                const char *fpath = cJSON_GetStringValue(
                    cJSON_GetObjectItem(args, "filepath"));
                if (!fpath) {
                    snprintf(tool_result, sizeof(tool_result),
                             "Error: Invalid filepath");
                } else {
                    set_fg_rgb(THEME_DIM);
                    printf("  │ ");
                    set_fg_rgb(THEME_R3);
                    printf("📄 %s\n" ANSI_RESET, fpath);

                    FILE *fp = fopen(fpath, "r");
                    if (!fp) {
                        snprintf(tool_result, sizeof(tool_result),
                                 "Error: %s", strerror(errno));
                    } else {
                        size_t total = fread(tool_result, 1,
                                             sizeof(tool_result) - 1, fp);
                        tool_result[total] = '\0';
                        fclose(fp);
                        if (strlen(tool_result) > 2000)
                            strcpy(tool_result + 2000, "\n…[truncated]");
                    }
                }

            } else if (fn && strcmp(fn, "client_write_file") == 0) {
                const char *fpath = cJSON_GetStringValue(
                    cJSON_GetObjectItem(args, "filepath"));
                const char *fcontent = cJSON_GetStringValue(
                    cJSON_GetObjectItem(args, "content"));
                if (!fpath || !fcontent) {
                    snprintf(tool_result, sizeof(tool_result),
                             "Error: Missing filepath or content");
                } else {
                    set_fg_rgb(THEME_DIM);
                    printf("  │ ");
                    set_fg_rgb(THEME_R3);
                    printf("📝 Writing: %s (%zu bytes)\n" ANSI_RESET,
                           fpath, strlen(fcontent));

                    printf("  ");
                    set_fg_rgb(THEME_R4);
                    printf("  Allow write? " ANSI_RESET);
                    set_fg_rgb(THEME_R2);
                    printf("[Y]es");
                    printf(ANSI_RESET " / ");
                    set_fg_rgb(THEME_DIM);
                    printf("[n]o" ANSI_RESET ": ");
                    fflush(stdout);

                    char confirm[32] = {0};
                    if (fgets(confirm, sizeof(confirm), stdin)) {
                        char *c = confirm;
                        while (*c == ' ') c++;
                        if (c[0] == 'y' || c[0] == 'Y' ||
                            c[0] == '\n' || c[0] == '\0') {
                            FILE *fp = fopen(fpath, "w");
                            if (!fp) {
                                snprintf(tool_result, sizeof(tool_result),
                                         "Error: %s", strerror(errno));
                            } else {
                                fwrite(fcontent, 1, strlen(fcontent), fp);
                                fclose(fp);
                                snprintf(tool_result, sizeof(tool_result),
                                         "Wrote %zu bytes to %s",
                                         strlen(fcontent), fpath);
                            }
                        } else {
                            strcpy(tool_result,
                                "User rejected file write.");
                        }
                    }
                }

            } else if (fn && strcmp(fn, "client_list_directory") == 0) {
                const char *dpath = cJSON_GetStringValue(
                    cJSON_GetObjectItem(args, "path"));
                if (!dpath) dpath = ".";

                set_fg_rgb(THEME_DIM);
                printf("  │ ");
                set_fg_rgb(THEME_R3);
                printf("📁 %s\n" ANSI_RESET, dpath);

                char ls_cmd[512];
                snprintf(ls_cmd, sizeof(ls_cmd),
                         "ls -la '%s' 2>&1", dpath);
                FILE *proc = popen(ls_cmd, "r");
                if (!proc) {
                    snprintf(tool_result, sizeof(tool_result),
                             "Error: %s", strerror(errno));
                } else {
                    size_t total = 0;
                    while (total < sizeof(tool_result) - 1) {
                        size_t n = fread(tool_result + total, 1,
                                         sizeof(tool_result) - 1 - total, proc);
                        if (n == 0) break;
                        total += n;
                    }
                    tool_result[total] = '\0';
                    pclose(proc);
                    if (strlen(tool_result) > 2000)
                        strcpy(tool_result + 2000, "\n…[truncated]");
                }

            } else {
                snprintf(tool_result, sizeof(tool_result),
                         "Error: Unknown tool '%s'", fn ? fn : "(null)");
            }

            /* Print result in bordered box */
            set_fg_rgb(THEME_DIM);
            printf("  │\n");
            int w = term_width() - 6;
            if (w > 120) w = 120;

            const char *rl = tool_result;
            while (*rl) {
                const char *nl = strchr(rl, '\n');
                size_t ll = nl ? (size_t)(nl - rl) : strlen(rl);
                if (ll > (size_t)w) ll = (size_t)w;
                set_fg_rgb(THEME_DIM);
                printf("  │ " ANSI_RESET ANSI_DIM);
                fwrite(rl, 1, ll, stdout);
                printf(ANSI_RESET "\n");
                rl = nl ? nl + 1 : rl + strlen(rl);
            }

            set_fg_rgb(THEME_DIM);
            printf("  └");
            for (int i = 0; i < w; i++) printf("─");
            printf("\n" ANSI_RESET);

            /* Continue loop with tool response */
            cJSON_Delete(payload);
            cJSON_Delete(resp_json);

            payload = cJSON_CreateObject();
            cJSON_AddStringToObject(payload, "type", "tool_response");
            cJSON_AddStringToObject(payload, "content", tool_result);
            if (fn) cJSON_AddStringToObject(payload, "tool_name", fn);
            continue;

        } else {
            /* ── Text response ───────────────────────────────────── */
            const char *text = cJSON_GetStringValue(
                cJSON_GetObjectItem(resp_json, "content"));

            double elapsed = (now_ms() - round_start) / 1000.0;

            printf("\n");
            set_bg_rgb(THEME_R1);
            set_fg_rgb(255, 255, 255);
            printf(ANSI_BOLD " ✦ Gemini " ANSI_RESET);
            set_fg_rgb(THEME_DIM);
            printf(" %.1fs", elapsed);
            printf(ANSI_RESET "\n");

            if (text) {
                render_markdown(text);
            }

            cJSON_Delete(resp_json);
            cJSON_Delete(payload);
            break;
        }
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════════════ */

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
            g_cfg.port = atoi(optarg);
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
        else                    g_cfg.port = 53535;

        if ((env = getenv("SERVER_PORT")) && env[0])
            g_cfg.port = atoi(env);
    }

    /* Non-interactive: disable typewriter */
    if (!g_is_tty) g_cfg.typewriter = 0;

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
        /* Reset interrupt flag at start of each prompt cycle */
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
                /* Ctrl+C while at the prompt → clean exit */
                set_fg_rgb(THEME_DIM);
                printf("\n  Goodbye.\n" ANSI_RESET);
                break;
            }
            /* EOF */
            break;
        }

        /* If Ctrl+C fired during fgets but fgets still returned a line,
         * treat it as a cancelled input */
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
