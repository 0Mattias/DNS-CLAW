/*
 * server/session.c — Session state management and reaper
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <cJSON.h>

#include "protocol.h"
#include "server/session.h"
#include "server/log.h"
#include "server/transport.h" /* g_running */

/* ── Global session state ─────────────────────────────────────────────────── */

session_t g_sessions[MAX_SESSIONS];
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── Functions ────────────────────────────────────────────────────────────── */

void session_free_responses(session_t *sess)
{
    for (int m = 0; m < MAX_MSG_IDS; m++) {
        msg_response_t *mr = &sess->responses[m];
        for (int c = 0; c < mr->chunk_count; c++) {
            free(mr->chunks[c]);
            mr->chunks[c] = NULL;
        }
        mr->chunk_count = 0;
    }
}

void session_destroy(session_t *sess)
{
    session_free_responses(sess);
    if (sess->history) {
        cJSON_Delete(sess->history);
        sess->history = NULL;
    }
    sess->active = 0;
}

void *session_reaper_thread(void *arg)
{
    (void)arg;
    while (g_running) {
        sleep(60);
        time_t now = time(NULL);
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (g_sessions[i].active && g_sessions[i].busy == 0 &&
                difftime(now, g_sessions[i].last_active) > SESSION_TIMEOUT_SEC) {
                log_warn("reaper", "Expiring session %s (idle %ds)", g_sessions[i].id,
                         (int)difftime(now, g_sessions[i].last_active));
                session_destroy(&g_sessions[i]);
            }
        }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

session_t *find_session(const char *sid)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].active && strcmp(g_sessions[i].id, sid) == 0)
            return &g_sessions[i];
    }
    return NULL;
}
