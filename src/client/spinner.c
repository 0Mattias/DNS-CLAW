/*
 * client/spinner.c — Async braille spinner
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "client/render.h"
#include "client/spinner.h"
#include "client/protocol.h"  /* g_interrupted */

static const char *SPINNER_FRAMES[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"
};
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

void spinner_start(spinner_t *s, const char *msg)
{
    strncpy(s->message, msg, sizeof(s->message) - 1);
    s->message[sizeof(s->message) - 1] = '\0';
    s->running = 1;
    s->start_time = now_ms();
    pthread_mutex_init(&s->msg_lock, NULL);
    pthread_create(&s->thread, NULL, spinner_run, s);
}

void spinner_set_message(spinner_t *s, const char *msg)
{
    pthread_mutex_lock(&s->msg_lock);
    strncpy(s->message, msg, sizeof(s->message) - 1);
    s->message[sizeof(s->message) - 1] = '\0';
    pthread_mutex_unlock(&s->msg_lock);
}

void spinner_stop(spinner_t *s)
{
    s->running = 0;
    pthread_join(s->thread, NULL);
    pthread_mutex_destroy(&s->msg_lock);
}
