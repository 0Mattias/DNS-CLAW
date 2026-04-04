/*
 * client/spinner.h — Async braille spinner
 */
#ifndef CLAW_CLIENT_SPINNER_H
#define CLAW_CLIENT_SPINNER_H

#include <pthread.h>

typedef struct {
    char message[256];
    volatile int running;
    pthread_t thread;
    pthread_mutex_t msg_lock;
    double start_time;
} spinner_t;

void spinner_start(spinner_t *s, const char *msg);
void spinner_set_message(spinner_t *s, const char *msg);
void spinner_stop(spinner_t *s);

#endif /* CLAW_CLIENT_SPINNER_H */
