/*
 * client/ui.c — Banner, help, usage display
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>

#include "config.h"
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

void print_usage(const char *argv0)
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
