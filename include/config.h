/*
 * config.h — Shared configuration, banner, and color definitions.
 *
 * Used by both client and server to avoid duplication.
 */
#ifndef DNS_CLAW_CONFIG_H
#define DNS_CLAW_CONFIG_H

#include <stdio.h>

/* ── Theme Colors (ANSI true-color escape sequences) ─────────────────────── */

#define CLR_R1     "\033[38;2;255;60;50m"    /* Primary accent (hot red) */
#define CLR_R2     "\033[38;2;255;100;80m"   /* Secondary (warm coral) */
#define CLR_R3     "\033[38;2;255;140;110m"  /* Tertiary (salmon) */
#define CLR_R4     "\033[38;2;255;195;180m"  /* Light accent (blush) */
#define CLR_DIM    "\033[38;2;100;80;80m"    /* Muted red-grey */
#define CLR_RESET  "\033[0m"

/* ── Banner ──────────────────────────────────────────────────────────────── */

static const char *DNS_CLAW_BANNER_ART[] = {
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
#define DNS_CLAW_BANNER_LINES 10

static const int DNS_CLAW_BANNER_COLORS[][3] = {
    {255, 20,  20},   {255, 60,  50},   {255, 100, 80},
    {255, 140, 110},  {255, 170, 150},  {255, 195, 180},
    {255, 215, 210},  {255, 230, 225},  {255, 245, 242},
    {255, 255, 255},
};

/*
 * Print the DNS-CLAW gradient banner to the given file stream.
 */
static inline void print_banner_to(FILE *fp)
{
    fprintf(fp, "\n");
    for (int i = 0; i < DNS_CLAW_BANNER_LINES; i++) {
        fprintf(fp, "\033[38;2;%d;%d;%dm%s" CLR_RESET "\n",
                DNS_CLAW_BANNER_COLORS[i][0],
                DNS_CLAW_BANNER_COLORS[i][1],
                DNS_CLAW_BANNER_COLORS[i][2],
                DNS_CLAW_BANNER_ART[i]);
    }
    fprintf(fp, "\n");
}

/* ── .env file loader ────────────────────────────────────────────────────── */

/*
 * Parse a .env file and call setenv() for each key=value pair.
 * Does not overwrite existing environment variables.
 * Handles quoted values, inline comments, and whitespace trimming.
 */
void load_dotenv(const char *path);

#endif /* DNS_CLAW_CONFIG_H */
