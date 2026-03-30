/*
 * client/main.c — Agentic DNS-LLM Client (C Port)
 *
 * A fully-featured terminal chat client that communicates with the
 * DNS-LLM server via DNS tunneling (UDP, DoT, DoH).
 *
 * Features:
 *   - Gradient ASCII banner
 *   - Async spinner during network I/O
 *   - ANSI markdown rendering (bold, italic, headings, code blocks, lists)
 *   - Agentic tool execution (bash, file read) with user approval
 *   - Session management (/clear, /compact, /help)
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <cJSON.h>

#include "base32.h"
#include "base64.h"
#include "dns_proto.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Configuration
 * ═══════════════════════════════════════════════════════════════════════════ */

static struct {
    char server_addr[256]; /* IP or URL */
    int  port;
    int  use_dot;
    int  use_doh;
    int  insecure;
} g_cfg;

static char g_session_id[64];
static int  g_msg_id = 0;

/* ═══════════════════════════════════════════════════════════════════════════
 * .env loader (same as server)
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

/* ANSI color helpers */
#define ANSI_RESET    "\033[0m"
#define ANSI_BOLD     "\033[1m"
#define ANSI_DIM      "\033[2m"
#define ANSI_ITALIC   "\033[3m"
#define ANSI_ULINE    "\033[4m"

static void set_fg_rgb(int r, int g, int b)
{
    printf("\033[38;2;%d;%d;%dm", r, g, b);
}

static void set_bg_rgb(int r, int g, int b)
{
    printf("\033[48;2;%d;%d;%dm", r, g, b);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Spinner (runs in a separate thread)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    const char     *message;
    volatile int    running;
    pthread_t       thread;
    pthread_mutex_t msg_lock;
} spinner_t;

static const char *SPINNER_FRAMES[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"
};
#define SPINNER_FRAME_COUNT 10

static void *spinner_run(void *arg)
{
    spinner_t *s = (spinner_t *)arg;
    int frame = 0;

    while (s->running) {
        pthread_mutex_lock(&s->msg_lock);
        /* Magenta spinner + dim message */
        printf("\r\033[K");
        set_fg_rgb(190, 60, 255);
        printf(" %s ", SPINNER_FRAMES[frame % SPINNER_FRAME_COUNT]);
        printf(ANSI_DIM "%s" ANSI_RESET, s->message);
        fflush(stdout);
        pthread_mutex_unlock(&s->msg_lock);

        frame++;
        usleep(100000); /* 100ms */
    }
    printf("\r\033[K");
    fflush(stdout);
    return NULL;
}

static void spinner_start(spinner_t *s, const char *msg)
{
    s->message = msg;
    s->running = 1;
    pthread_mutex_init(&s->msg_lock, NULL);
    pthread_create(&s->thread, NULL, spinner_run, s);
}

static void spinner_set_message(spinner_t *s, const char *msg)
{
    pthread_mutex_lock(&s->msg_lock);
    s->message = msg;
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
    spinner_start(&sp, "Initializing Agent Session...");

    char txt[DNS_MAX_TXT];
    int rc = do_dns_query("init.llm.local.", txt, sizeof(txt));

    spinner_stop(&sp);

    if (rc < 0) {
        set_fg_rgb(255, 80, 80);
        printf("Failed to initialize session.\n" ANSI_RESET);
        return -1;
    }

    /* Trim whitespace */
    char *p = txt;
    while (*p == ' ' || *p == '\n' || *p == '\r') p++;
    strncpy(g_session_id, p, sizeof(g_session_id) - 1);
    size_t slen = strlen(g_session_id);
    while (slen > 0 && (g_session_id[slen-1] == ' ' ||
           g_session_id[slen-1] == '\n' || g_session_id[slen-1] == '\r'))
        g_session_id[--slen] = '\0';

    g_msg_id = 0;

    if (show_msg) {
        printf(ANSI_DIM "Session ID established: %s\n" ANSI_RESET, g_session_id);
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Basic ANSI Markdown Renderer
 * ═══════════════════════════════════════════════════════════════════════════ */

static void render_markdown(const char *text)
{
    int width = term_width();
    int wrap = width - 4;
    if (wrap < 40) wrap = 40;
    if (wrap > 120) wrap = 120;

    int in_code_block = 0;
    const char *line = text;

    while (*line) {
        /* Extract one line */
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
                /* Print language hint if present */
                set_fg_rgb(100, 100, 100);
                printf("  ┌─");
                if (buf[3]) printf(" %s ", buf + 3);
                printf("─\n" ANSI_RESET);
            } else {
                in_code_block = 0;
                set_fg_rgb(100, 100, 100);
                printf("  └──\n" ANSI_RESET);
            }
            line = eol ? eol + 1 : line + llen;
            continue;
        }

        if (in_code_block) {
            /* Render code line with dim background */
            set_fg_rgb(180, 220, 255);
            printf("  │ %s\n" ANSI_RESET, buf);
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
                set_fg_rgb(0, 255, 255);
                printf(ANSI_BOLD "\n  %s\n" ANSI_RESET, heading);
                set_fg_rgb(0, 255, 255);
                printf("  ");
                for (int i = 0; i < (int)strlen(heading) && i < wrap; i++)
                    printf("─");
                printf("\n" ANSI_RESET);
            } else if (level == 2) {
                set_fg_rgb(120, 180, 255);
                printf(ANSI_BOLD "\n  %s\n" ANSI_RESET, heading);
            } else {
                set_fg_rgb(180, 140, 255);
                printf(ANSI_BOLD "  %s\n" ANSI_RESET, heading);
            }
            line = eol ? eol + 1 : line + llen;
            continue;
        }

        /* Unordered list items */
        if ((buf[0] == '-' || buf[0] == '*') && buf[1] == ' ') {
            set_fg_rgb(0, 255, 200);
            printf("  • ");
            printf(ANSI_RESET "%s\n", buf + 2);
            line = eol ? eol + 1 : line + llen;
            continue;
        }

        /* Numbered list items */
        if (isdigit((unsigned char)buf[0]) && buf[1] == '.' && buf[2] == ' ') {
            set_fg_rgb(0, 255, 200);
            printf("  %c. ", buf[0]);
            printf(ANSI_RESET "%s\n", buf + 3);
            line = eol ? eol + 1 : line + llen;
            continue;
        }

        /* Horizontal rules */
        if (strncmp(buf, "---", 3) == 0 || strncmp(buf, "***", 3) == 0) {
            set_fg_rgb(100, 100, 100);
            printf("  ");
            for (int i = 0; i < wrap; i++) printf("─");
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

        /* Regular text — inline formatting */
        printf("  ");
        const char *p = buf;
        while (*p) {
            /* Bold: **text** */
            if (p[0] == '*' && p[1] == '*') {
                const char *end = strstr(p + 2, "**");
                if (end) {
                    printf(ANSI_BOLD);
                    fwrite(p + 2, 1, (size_t)(end - p - 2), stdout);
                    printf(ANSI_RESET);
                    p = end + 2;
                    continue;
                }
            }
            /* Italic: *text* (single) */
            if (p[0] == '*' && p[1] != '*') {
                const char *end = strchr(p + 1, '*');
                if (end && end != p + 1) {
                    printf(ANSI_ITALIC);
                    fwrite(p + 1, 1, (size_t)(end - p - 1), stdout);
                    printf(ANSI_RESET);
                    p = end + 1;
                    continue;
                }
            }
            /* Inline code: `text` */
            if (p[0] == '`') {
                const char *end = strchr(p + 1, '`');
                if (end) {
                    set_fg_rgb(255, 180, 100);
                    set_bg_rgb(40, 40, 40);
                    printf(" ");
                    fwrite(p + 1, 1, (size_t)(end - p - 1), stdout);
                    printf(" " ANSI_RESET);
                    p = end + 1;
                    continue;
                }
            }
            putchar(*p++);
        }
        printf("\n");

        line = eol ? eol + 1 : line + llen;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Banner
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_banner(void)
{
    static const char *BANNER[] = {
        "▓█████▄  ███▄    █   ██████     ██▓     ██▓     ███▄ ▄███▓",
        "▒██▀ ██▌ ██ ▀█   █ ▒██    ▒    ▓██▒    ▓██▒    ▓██▒▀█▀ ██▒",
        "░██   █▌▓██  ▀█ ██▒░ ▓██▄      ▒██░    ▒██░    ▓██    ▓██░",
        "░▓█▄   ▌▓██▒  ▐▌██▒  ▒   ██▒   ▒██░    ▒██░    ▒██    ▒██ ",
        "░▒████▓ ▒██░   ▓██░▒██████▒▒   ░██████▒░██████▒▒██▒   ░██▒",
        " ▒▒▓  ▒ ░ ▒░   ▒ ▒ ▒ ▒▓▒ ▒ ░   ░ ▒░▓  ░░ ▒░▓  ░░ ▒░   ░  ░",
        " ░ ▒  ▒ ░ ░░   ░ ▒░░ ░▒  ░ ░   ░ ░ ▒  ░░ ░ ▒  ░░  ░      ░",
        " ░ ░  ░    ░   ░ ░ ░  ░  ░       ░ ░     ░ ░   ░      ░   ",
        "   ░             ░       ░         ░  ░    ░  ░       ░   ",
        " ░                                                        ",
    };

    /* Cyan → Magenta gradient */
    int colors[][3] = {
        {0, 255, 255},   {28, 226, 255},  {56, 198, 255},
        {85, 170, 255},  {113, 142, 255}, {142, 113, 255},
        {170, 85, 255},  {198, 56, 255},  {226, 28, 255},
        {255, 0, 255},
    };

    printf("\n");
    for (int i = 0; i < 10; i++) {
        set_fg_rgb(colors[i][0], colors[i][1], colors[i][2]);
        printf("%s\n", BANNER[i]);
    }
    printf(ANSI_RESET);

    printf(ANSI_DIM);
    printf("          A stealthy, high-capacity Agentic DNS tunnel\n");
    printf("          Written in C — zero dependencies on cloud SDKs\n");
    printf(ANSI_RESET);
    printf("\n");
    printf(ANSI_DIM);
    printf("──────────────────────────────────────────────────────────────────\n");
    printf(ANSI_RESET);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Help
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_help(void)
{
    printf("\n");
    set_fg_rgb(243, 156, 18);
    printf(ANSI_BOLD "Available Commands:\n" ANSI_RESET);
    printf("  /help                     Show this help\n");
    printf("  /clear, /new, /reset      Start a new chat session\n");
    printf("  /exit, /quit              Exit the application\n");
    printf("  /compact [instructions]   Ask the LLM to compact context\n");
    printf("\n");
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

    while (1) {
        g_msg_id++;
        int current_mid = g_msg_id;

        char spin_msg[128];
        snprintf(spin_msg, sizeof(spin_msg),
                 "Transmitting payload [%d] over DNS...", current_mid);
        spinner_t sp;
        spinner_start(&sp, spin_msg);

        char *payload_str = cJSON_PrintUnformatted(payload);
        size_t payload_len = strlen(payload_str);

        /* 1. Upload chunks (base32) */
        int chunk_size = 35;
        char b32_buf[256];
        char qname[1024];
        char txt[DNS_MAX_TXT];

        int seq = 0;
        for (size_t i = 0; i < payload_len; i += (size_t)chunk_size) {
            size_t clen = payload_len - i;
            if (clen > (size_t)chunk_size) clen = (size_t)chunk_size;

            base32_encode((uint8_t *)payload_str + i, clen,
                          b32_buf, sizeof(b32_buf));

            /* Lowercase for DNS label compatibility */
            for (char *p = b32_buf; *p; p++)
                *p = (char)tolower((unsigned char)*p);

            snprintf(qname, sizeof(qname), "%s.%d.up.%d.%s.llm.local.",
                     b32_buf, seq, current_mid, g_session_id);

            if (do_dns_query(qname, txt, sizeof(txt)) < 0 ||
                strcmp(txt, "ACK") != 0) {
                spinner_stop(&sp);
                free(payload_str);
                cJSON_Delete(payload);
                set_fg_rgb(255, 80, 80);
                printf("Failed to send chunk %d\n" ANSI_RESET, seq);
                return -1;
            }
            seq++;
        }
        free(payload_str);

        /* 2. Send fin */
        snprintf(spin_msg, sizeof(spin_msg),
                 "Waiting for Agent [%d]...", current_mid);
        spinner_set_message(&sp, spin_msg);

        snprintf(qname, sizeof(qname), "fin.%d.%s.llm.local.",
                 current_mid, g_session_id);
        if (do_dns_query(qname, txt, sizeof(txt)) < 0 ||
            strcmp(txt, "ACK") != 0) {
            spinner_stop(&sp);
            cJSON_Delete(payload);
            set_fg_rgb(255, 80, 80);
            printf("Server did not ACK fin\n" ANSI_RESET);
            return -1;
        }

        /* 3. Poll + download response */
        int down_seq = 0;
        char full_response[131072] = {0};
        size_t resp_pos = 0;

        while (1) {
            snprintf(qname, sizeof(qname), "%d.%d.down.%s.llm.local.",
                     down_seq, current_mid, g_session_id);
            if (do_dns_query(qname, txt, sizeof(txt)) < 0) {
                spinner_stop(&sp);
                cJSON_Delete(payload);
                set_fg_rgb(255, 80, 80);
                printf("Failed to poll chunk %d\n" ANSI_RESET, down_seq);
                return -1;
            }

            if (strcmp(txt, "PENDING") == 0) {
                usleep(500000);
                continue;
            } else if (strcmp(txt, "EOF") == 0) {
                break;
            } else if (strncmp(txt, "ERR:", 4) == 0) {
                spinner_stop(&sp);
                cJSON_Delete(payload);
                set_fg_rgb(255, 80, 80);
                printf("Server error: %s\n" ANSI_RESET, txt);
                return -1;
            }

            /* Base64 decode this chunk */
            uint8_t decoded[4096];
            int dlen = base64_decode(txt, decoded, sizeof(decoded));
            if (dlen < 0) {
                spinner_stop(&sp);
                cJSON_Delete(payload);
                set_fg_rgb(255, 80, 80);
                printf("Base64 decode error on chunk %d\n" ANSI_RESET, down_seq);
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

        /* 4. Parse JSON response */
        cJSON *resp_json = cJSON_Parse(full_response);
        if (!resp_json) {
            cJSON_Delete(payload);
            set_fg_rgb(255, 80, 80);
            printf("Failed to parse response JSON\n" ANSI_RESET);
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
            set_bg_rgb(243, 156, 18);
            set_fg_rgb(0, 0, 0);
            printf(ANSI_BOLD " Agent Tool Executing " ANSI_RESET);
            printf("\n\n");

            char tool_result[8192] = {0};

            if (fn && strcmp(fn, "client_execute_bash") == 0) {
                const char *cmd = cJSON_GetStringValue(
                    cJSON_GetObjectItem(args, "command"));
                if (!cmd) {
                    snprintf(tool_result, sizeof(tool_result),
                             "Error: Invalid command argument");
                } else {
                    set_fg_rgb(243, 156, 18);
                    printf(ANSI_BOLD "⚙️  Targeting Bash Command:\n" ANSI_RESET);
                    printf(ANSI_DIM "  %s\n\n" ANSI_RESET, cmd);

                    set_fg_rgb(0, 255, 255);
                    printf("  Allow this command to run? [Y/n]: " ANSI_RESET);
                    fflush(stdout);

                    char confirm[32] = {0};
                    if (fgets(confirm, sizeof(confirm), stdin)) {
                        char *c = confirm;
                        while (*c == ' ') c++;
                        /* Lowercase */
                        for (char *p = c; *p; p++)
                            *p = (char)tolower((unsigned char)*p);

                        if (c[0] == 'y' || c[0] == '\n' || c[0] == '\0') {
                            /* Execute */
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
                                if (status != 0 && total == 0) {
                                    snprintf(tool_result, sizeof(tool_result),
                                             "Command exited with status %d", status);
                                }
                                if (strlen(tool_result) == 0) {
                                    strcpy(tool_result, "Success (No Output)");
                                }
                            }
                        } else {
                            set_fg_rgb(255, 80, 80);
                            printf("\n  Command execution rejected.\n" ANSI_RESET);
                            strcpy(tool_result,
                                "User rejected the command execution. "
                                "Ask them what they would like to do instead.");
                        }
                    }
                    /* Truncate if too long */
                    if (strlen(tool_result) > 1500)
                        strcpy(tool_result + 1500, "\n...[Output Truncated]");
                }

            } else if (fn && strcmp(fn, "client_read_file") == 0) {
                const char *fpath = cJSON_GetStringValue(
                    cJSON_GetObjectItem(args, "filepath"));
                if (!fpath) {
                    snprintf(tool_result, sizeof(tool_result),
                             "Error: Invalid filepath argument");
                } else {
                    set_fg_rgb(52, 152, 219);
                    printf(ANSI_BOLD "📄 Reading File: " ANSI_RESET);
                    printf("%s\n", fpath);

                    FILE *fp = fopen(fpath, "r");
                    if (!fp) {
                        snprintf(tool_result, sizeof(tool_result),
                                 "Error reading file: %s", strerror(errno));
                    } else {
                        size_t total = fread(tool_result, 1,
                                             sizeof(tool_result) - 1, fp);
                        tool_result[total] = '\0';
                        fclose(fp);
                        if (strlen(tool_result) > 1500)
                            strcpy(tool_result + 1500, "\n...[Content Truncated]");
                    }
                }

            } else {
                snprintf(tool_result, sizeof(tool_result),
                         "Error: Unknown tool '%s'", fn ? fn : "(null)");
            }

            /* Print result in a bordered box */
            printf("\n");
            set_fg_rgb(243, 156, 18);
            int w = term_width() - 4;
            printf("  ╭");
            for (int i = 0; i < w - 2; i++) printf("─");
            printf("╮\n");

            /* Print tool result line by line inside box */
            const char *rl = tool_result;
            while (*rl) {
                const char *nl = strchr(rl, '\n');
                size_t ll = nl ? (size_t)(nl - rl) : strlen(rl);
                if (ll > (size_t)(w - 4)) ll = (size_t)(w - 4);
                printf("  │ " ANSI_RESET);
                fwrite(rl, 1, ll, stdout);
                /* Pad to width */
                for (int i = (int)ll; i < w - 4; i++) putchar(' ');
                set_fg_rgb(243, 156, 18);
                printf(" │\n");
                rl = nl ? nl + 1 : rl + strlen(rl);
            }

            printf("  ╰");
            for (int i = 0; i < w - 2; i++) printf("─");
            printf("╯\n" ANSI_RESET);

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

            /* Gemini label */
            printf("\n");
            set_bg_rgb(162, 32, 179);
            set_fg_rgb(255, 255, 255);
            printf(ANSI_BOLD " Gemini " ANSI_RESET);
            printf("\n");

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

int main(void)
{
    srand((unsigned)time(NULL));
    curl_global_init(CURL_GLOBAL_DEFAULT);

    load_dotenv("../.env");
    load_dotenv(".env");

    /* Config */
    const char *env;
    g_cfg.use_dot = (env = getenv("USE_DOT")) && strcmp(env, "true") == 0;
    g_cfg.use_doh = (env = getenv("USE_DOH")) && strcmp(env, "true") == 0;
    g_cfg.insecure = (env = getenv("INSECURE_SKIP_VERIFY")) && strcmp(env, "true") == 0;

    if ((env = getenv("DNS_SERVER_ADDR")) && env[0]) {
        strncpy(g_cfg.server_addr, env, sizeof(g_cfg.server_addr) - 1);
    } else if (g_cfg.use_doh) {
        strcpy(g_cfg.server_addr, "https://127.0.0.1/dns-query");
    } else {
        strcpy(g_cfg.server_addr, "127.0.0.1");
    }

    if (g_cfg.use_dot)      g_cfg.port = 853;
    else if (g_cfg.use_doh) g_cfg.port = 443;
    else                    g_cfg.port = 53535;

    if ((env = getenv("SERVER_PORT")) && env[0])
        g_cfg.port = atoi(env);

    /* ── Banner ──────────────────────────────────────────────── */
    print_banner();

    /* ── Init session ────────────────────────────────────────── */
    if (init_session(1) < 0) {
        curl_global_cleanup();
        return 1;
    }

    printf(ANSI_DIM);
    printf("Type your message below. Press Enter to send, or Ctrl+C to exit.\n");
    printf("Type /help for a list of commands.\n");
    printf("Ready for input.\n");
    printf(ANSI_RESET "\n");

    /* ── REPL ────────────────────────────────────────────────── */
    char input[4096];

    while (1) {
        /* Prompt: [You]❯  */
        set_bg_rgb(0, 255, 255);
        set_fg_rgb(0, 0, 0);
        printf(ANSI_BOLD " You " ANSI_RESET);
        set_fg_rgb(0, 255, 255);
        printf("❯ " ANSI_RESET);
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) break;

        /* Trim trailing newline */
        size_t ilen = strlen(input);
        while (ilen > 0 && (input[ilen-1] == '\n' || input[ilen-1] == '\r'))
            input[--ilen] = '\0';

        /* Trim leading whitespace */
        char *text = input;
        while (*text == ' ' || *text == '\t') text++;

        if (text[0] == '\0') continue;

        /* Commands */
        if (strcmp(text, "/exit") == 0 || strcmp(text, "/quit") == 0) {
            printf(ANSI_DIM "\nExiting session. Goodbye!\n" ANSI_RESET);
            break;
        }
        if (strcmp(text, "/clear") == 0 || strcmp(text, "/new") == 0 ||
            strcmp(text, "/reset") == 0) {
            printf("\n");
            set_fg_rgb(0, 255, 255);
            printf("Starting a new session...\n" ANSI_RESET);
            init_session(1);
            continue;
        }
        if (strcmp(text, "/help") == 0) {
            print_help();
            continue;
        }
        if (strncmp(text, "/compact", 8) == 0) {
            const char *focus = text + 8;
            while (*focus == ' ') focus++;

            char compact_msg[4096];
            if (*focus) {
                snprintf(compact_msg, sizeof(compact_msg),
                    "SYSTEM COMMAND: Please summarize our conversation so far, "
                    "keeping all important context. Acknowledge you have compacted "
                    "your memory. For all future responses, please focus strictly on: %s",
                    focus);
            } else {
                strcpy(compact_msg,
                    "SYSTEM COMMAND: Please summarize our conversation so far, "
                    "keeping all important facts and context, then acknowledge you "
                    "have compacted your memory to save space.");
            }
            set_fg_rgb(190, 60, 255);
            printf("\n[Sending compact instruction to Agent...]\n" ANSI_RESET);
            text = compact_msg;
        }

        process_message_loop("user", text, NULL);
        printf("\n");
    }

    curl_global_cleanup();
    return 0;
}
