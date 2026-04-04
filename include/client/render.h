/*
 * client/render.h — ANSI markdown renderer and terminal helpers
 */
#ifndef CLAW_CLIENT_RENDER_H
#define CLAW_CLIENT_RENDER_H

/* Theme colors — warm red/crimson palette matching DNS CLAW banner */
#define THEME_R1  255, 60, 50   /* Primary accent (hot red) */
#define THEME_R2  255, 100, 80  /* Secondary accent (warm coral) */
#define THEME_R3  255, 140, 110 /* Tertiary (salmon) */
#define THEME_R4  255, 195, 180 /* Light accent (blush) */
#define THEME_DIM 100, 80, 80   /* Muted red-grey for borders */
#define THEME_TXT 255, 230, 225 /* Near-white warm text */

#define ANSI_RESET  "\033[0m"
#define ANSI_BOLD   "\033[1m"
#define ANSI_DIM    "\033[2m"
#define ANSI_ITALIC "\033[3m"
#define ANSI_ULINE  "\033[4m"
#define ANSI_STRIKE "\033[9m"

/* Client config (defined in main.c) */
typedef struct {
    char server_addr[256];
    int port;
    int use_dot;
    int use_doh;
    int insecure;
    int no_color;
    int typewriter;
    char auth_token[128];
} client_config_t;

extern client_config_t g_cfg;
extern int g_is_tty;

/* Terminal */
int term_width(void);
void set_fg_rgb(int r, int g, int b);
void set_bg_rgb(int r, int g, int b);

/* Timing */
double now_ms(void);

/* Typewriter */
void typewriter_putchar(int ch);
void typewriter_puts(const char *s);

/* Markdown */
void render_inline(const char *p);
void render_markdown(const char *text);

#endif /* CLAW_CLIENT_RENDER_H */
