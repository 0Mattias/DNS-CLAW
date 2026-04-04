/*
 * server/session.h — Session management
 */
#ifndef CLAW_SERVER_SESSION_H
#define CLAW_SERVER_SESSION_H

#include <pthread.h>
#include <time.h>
#include <cJSON.h>
#include "protocol.h"

/* ── Data structures ──────────────────────────────────────────────────────── */

typedef struct {
    int seq;
    char data[256]; /* base32-encoded chunk */
} upload_chunk_t;

typedef struct {
    int valid;
    upload_chunk_t chunks[MAX_CHUNKS];
    int chunk_count;
} pending_prompt_t;

typedef struct {
    int ready;
    int failed;
    char *chunks[MAX_RESP_CHUNKS];
    int chunk_count;
} msg_response_t;

typedef struct {
    char id[32];
    int active;
    int busy; /* refcount: >0 means LLM threads are using this */
    time_t created_at;
    time_t last_active;
    cJSON *history; /* JSON array of {"role":..., "parts":...} */
    pending_prompt_t pending[MAX_MSG_IDS];
    msg_response_t responses[MAX_MSG_IDS];
} session_t;

/* ── Global state (defined in session.c) ──────────────────────────────────── */

extern session_t g_sessions[MAX_SESSIONS];
extern pthread_mutex_t g_lock;

/* ── Functions ────────────────────────────────────────────────────────────── */

void session_free_responses(session_t *sess);
void session_destroy(session_t *sess);
void *session_reaper_thread(void *arg);
session_t *find_session(const char *sid);

/* Persistence: save/load session history as JSON files */
int session_save(session_t *sess);
int session_load(const char *sid, session_t *sess);
int session_list_saved(char ids[][32], time_t dates[], int max);

#endif /* CLAW_SERVER_SESSION_H */
