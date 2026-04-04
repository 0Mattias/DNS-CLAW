/*
 * client/render.c — Terminal helpers, typewriter effect, ANSI markdown renderer
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include "client/render.h"

/* ── Terminal Helpers ─────────────────────────────────────────────────────── */

int term_width(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

void set_fg_rgb(int r, int g, int b)
{
    if (!g_cfg.no_color)
        printf("\033[38;2;%d;%d;%dm", r, g, b);
}

void set_bg_rgb(int r, int g, int b)
{
    if (!g_cfg.no_color)
        printf("\033[48;2;%d;%d;%dm", r, g, b);
}

/* ── Timing ───────────────────────────────────────────────────────────────── */

double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

/* ── Typewriter Effect ────────────────────────────────────────────────────── */

void typewriter_putchar(int ch)
{
    putchar(ch);
    if (g_cfg.typewriter && g_is_tty) {
        fflush(stdout);
        usleep(800);
    }
}

void typewriter_puts(const char *s)
{
    while (*s)
        typewriter_putchar(*s++);
}

/* ── Inline Markdown ──────────────────────────────────────────────────────── */

void render_inline(const char *p)
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
        /* Links: [text](url) */
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

/* ── Block-level Markdown ─────────────────────────────────────────────────── */

void render_markdown(const char *text)
{
    int in_code_block = 0;
    const char *line = text;

    while (*line) {
        const char *eol = strchr(line, '\n');
        size_t llen = eol ? (size_t)(eol - line) : strlen(line);
        char buf[4096];
        if (llen >= sizeof(buf))
            llen = sizeof(buf) - 1;
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
                int pad = term_width() - 10 - (buf[3] ? (int)strlen(buf + 3) + 2 : 0);
                for (int i = 0; i < pad && i < 120; i++)
                    printf("─");
                printf("┐\n" ANSI_RESET);
            } else {
                in_code_block = 0;
                set_fg_rgb(THEME_DIM);
                printf("  └");
                int pad = term_width() - 6;
                for (int i = 0; i < pad && i < 120; i++)
                    printf("─");
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
            int pad = term_width() - 8 - (int)strlen(buf);
            for (int i = 0; i < pad; i++)
                putchar(' ');
            set_fg_rgb(THEME_DIM);
            printf(" │\n" ANSI_RESET);
            line = eol ? eol + 1 : line + llen;
            continue;
        }

        /* Headings */
        if (buf[0] == '#') {
            int level = 0;
            while (buf[level] == '#' && level < 6)
                level++;
            const char *heading = buf + level;
            while (*heading == ' ')
                heading++;

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

        /* Blockquotes */
        if (buf[0] == '>') {
            const char *qt = buf + 1;
            while (*qt == ' ')
                qt++;
            set_fg_rgb(THEME_DIM);
            printf("  ┃ ");
            set_fg_rgb(THEME_R4);
            printf(ANSI_ITALIC);
            typewriter_puts(qt);
            printf(ANSI_RESET "\n");
            line = eol ? eol + 1 : line + llen;
            continue;
        }

        /* Unordered list items */
        {
            int indent = 0;
            const char *lp = buf;
            while (*lp == ' ') {
                lp++;
                indent++;
            }
            if ((*lp == '-' || *lp == '*' || *lp == '+') && lp[1] == ' ') {
                int spaces = 2 + (indent / 2) * 2;
                for (int i = 0; i < spaces; i++)
                    putchar(' ');
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

        /* Numbered list items */
        {
            const char *lp = buf;
            int indent = 0;
            while (*lp == ' ') {
                lp++;
                indent++;
            }
            if (isdigit((unsigned char)lp[0])) {
                const char *dot = lp;
                while (isdigit((unsigned char)*dot))
                    dot++;
                if (*dot == '.' && dot[1] == ' ') {
                    int spaces = 2 + (indent / 2) * 2;
                    for (int i = 0; i < spaces; i++)
                        putchar(' ');
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
            if (w > 120)
                w = 120;
            for (int i = 0; i < w; i++)
                printf("─");
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

        /* Regular text */
        printf("  ");
        render_inline(buf);
        printf("\n");

        line = eol ? eol + 1 : line + llen;
    }
}
