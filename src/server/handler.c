/*
 * server/handler.c — DNS query handler (protocol dispatcher)
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cJSON.h>

#include "dns_proto.h"
#include "protocol.h"
#include "server/handler.h"
#include "server/llm.h"
#include "server/log.h"
#include "server/session.h"

/* macOS provides arc4random_buf in <stdlib.h> but _POSIX_C_SOURCE hides it */
extern void arc4random_buf(void *, size_t);

/* ── Utility ──────────────────────────────────────────────────────────────── */

static void generate_id(char *out, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t randbuf[16];
    arc4random_buf(randbuf, sizeof(randbuf));
    size_t n = (len - 1) / 2;
    if (n > 16) n = 16;
    size_t pos = 0;
    for (size_t i = 0; i < n && pos + 1 < len; i++) {
        out[pos++] = hex[(randbuf[i] >> 4) & 0x0F];
        out[pos++] = hex[randbuf[i] & 0x0F];
    }
    out[pos] = '\0';
}

static int safe_atoi(const char *s)
{
    if (!s || !*s) return -1;
    char *end;
    errno = 0;
    long val = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0' || val < 0 || val > INT_MAX)
        return -1;
    return (int)val;
}

/* ── DNS Query Handler ────────────────────────────────────────────────────── */

int handle_dns_query(const uint8_t *query, size_t query_len,
                     uint8_t *resp_buf, size_t resp_buf_len)
{
    uint16_t qid;
    char qname[512];

    if (dns_parse_query(query, query_len, &qid, qname, sizeof(qname)) < 0)
        return -1;

    /* Lowercase the qname */
    for (char *p = qname; *p; p++)
        *p = (char)tolower((unsigned char)*p);

    /* Must end with llm.local. */
    if (!strstr(qname, "llm.local.")) {
        return dns_build_response(qid, qname, DNS_RCODE_NXDOMAIN, NULL,
                                  resp_buf, resp_buf_len);
    }

    /* Split into labels (thread-safe) */
    char *parts[16];
    int nparts = 0;
    char qname_copy[512];
    strncpy(qname_copy, qname, sizeof(qname_copy) - 1);
    qname_copy[sizeof(qname_copy) - 1] = '\0';

    char *saveptr = NULL;
    char *tok = strtok_r(qname_copy, ".", &saveptr);
    while (tok && nparts < 16) {
        parts[nparts++] = tok;
        tok = strtok_r(NULL, ".", &saveptr);
    }

    /* ── init.llm.local. ──────────────────────────────────────────── */
    if (nparts >= 3 && strcmp(parts[0], "init") == 0) {
        pthread_mutex_lock(&g_lock);
        session_t *sess = NULL;
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (!g_sessions[i].active) {
                sess = &g_sessions[i];
                break;
            }
        }
        if (!sess) {
            pthread_mutex_unlock(&g_lock);
            return dns_build_response(qid, qname, DNS_RCODE_SERVFAIL, NULL,
                                      resp_buf, resp_buf_len);
        }

        memset(sess, 0, sizeof(*sess));
        generate_id(sess->id, sizeof(sess->id));
        sess->active = 1;
        sess->created_at = time(NULL);
        sess->last_active = sess->created_at;
        sess->history = cJSON_CreateArray();
        pthread_mutex_unlock(&g_lock);

        log_ok("init", "New session: %s", sess->id);
        return dns_build_response(qid, qname, DNS_RCODE_OK, sess->id,
                                  resp_buf, resp_buf_len);
    }

    /* ── <chunk>.<seq>.up.<mid>.<sid>.llm.local. ──────────────────── */
    if (nparts >= 7 && strcmp(parts[2], "up") == 0) {
        const char *chunk_b32 = parts[0];
        int seq = safe_atoi(parts[1]);
        int mid = safe_atoi(parts[3]);
        const char *sid = parts[4];

        if (seq < 0) {
            return dns_build_response(qid, qname, DNS_RCODE_FORMERR, NULL,
                                      resp_buf, resp_buf_len);
        }

        if (mid < 0 || mid >= MAX_MSG_IDS) {
            return dns_build_response(qid, qname, DNS_RCODE_FORMERR, NULL,
                                      resp_buf, resp_buf_len);
        }

        pthread_mutex_lock(&g_lock);
        session_t *sess = find_session(sid);

        const char *reply = "ERR:NOSESSION";
        if (sess) {
            pending_prompt_t *pp = &sess->pending[mid];
            if (!pp->valid) {
                pp->valid = 1;
                pp->chunk_count = 0;
            }
            if (pp->chunk_count < MAX_CHUNKS) {
                pp->chunks[pp->chunk_count].seq = seq;
                strncpy(pp->chunks[pp->chunk_count].data, chunk_b32,
                        sizeof(pp->chunks[0].data) - 1);
                pp->chunk_count++;
            }
            reply = "ACK";
        }
        pthread_mutex_unlock(&g_lock);

        return dns_build_response(qid, qname, DNS_RCODE_OK, reply,
                                  resp_buf, resp_buf_len);
    }

    /* ── fin.<mid>.<sid>.llm.local. ───────────────────────────────── */
    if (nparts >= 5 && strcmp(parts[0], "fin") == 0) {
        int mid = safe_atoi(parts[1]);
        const char *sid = parts[2];

        if (mid < 0 || mid >= MAX_MSG_IDS) {
            return dns_build_response(qid, qname, DNS_RCODE_FORMERR, NULL,
                                      resp_buf, resp_buf_len);
        }

        pthread_mutex_lock(&g_lock);
        session_t *sess = find_session(sid);

        const char *reply = "ERR:NOSESSION";
        if (sess) {
            memset(&sess->responses[mid], 0, sizeof(msg_response_t));
            reply = "ACK";

            /* Spawn LLM processing thread */
            llm_task_t *task = malloc(sizeof(llm_task_t));
            task->sess = sess;
            task->msg_id = mid;
            pthread_t tid;
            pthread_create(&tid, NULL, process_llm_thread, task);
            pthread_detach(tid);
        }
        pthread_mutex_unlock(&g_lock);

        return dns_build_response(qid, qname, DNS_RCODE_OK, reply,
                                  resp_buf, resp_buf_len);
    }

    /* ── <seq>.<mid>.down.<sid>.llm.local. ────────────────────────── */
    if (nparts >= 6 && strcmp(parts[2], "down") == 0) {
        int seq = safe_atoi(parts[0]);
        int mid = safe_atoi(parts[1]);
        const char *sid = parts[3];

        if (seq < 0) {
            return dns_build_response(qid, qname, DNS_RCODE_FORMERR, NULL,
                                      resp_buf, resp_buf_len);
        }

        if (mid < 0 || mid >= MAX_MSG_IDS) {
            return dns_build_response(qid, qname, DNS_RCODE_FORMERR, NULL,
                                      resp_buf, resp_buf_len);
        }

        pthread_mutex_lock(&g_lock);
        session_t *sess = find_session(sid);

        const char *reply = "ERR:NOSESSION";
        if (sess) {
            msg_response_t *mr = &sess->responses[mid];
            if (mr->failed) {
                reply = "ERR:API_FAILED";
            } else if (!mr->ready) {
                reply = "PENDING";
            } else if (seq < mr->chunk_count) {
                reply = mr->chunks[seq];
            } else {
                reply = "EOF";
                for (int c = 0; c < mr->chunk_count; c++) {
                    free(mr->chunks[c]);
                    mr->chunks[c] = NULL;
                }
                mr->chunk_count = 0;
                mr->ready = 0;
            }
            sess->last_active = time(NULL);
        }
        pthread_mutex_unlock(&g_lock);

        return dns_build_response(qid, qname, DNS_RCODE_OK, reply,
                                  resp_buf, resp_buf_len);
    }

    /* Unknown query */
    return dns_build_response(qid, qname, DNS_RCODE_NXDOMAIN, NULL,
                              resp_buf, resp_buf_len);
}
