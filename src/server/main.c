/*
 * server/main.c — Agentic DNS-LLM Server (C Port)
 *
 * A DNS server that tunnels LLM interactions via TXT records.
 * Supports plain UDP, DNS-over-TLS (DoT), and DNS-over-HTTPS (DoH).
 * Talks to the Gemini REST API directly via libcurl.
 *
 * Protocol (wire encoding is handled by dns_proto.h):
 *   init.llm.local.             → returns session ID
 *   <b32chunk>.<seq>.up.<mid>.<sid>.llm.local. → upload chunk, returns ACK
 *   fin.<mid>.<sid>.llm.local.  → finalize upload, triggers LLM call
 *   <seq>.<mid>.down.<sid>.llm.local. → poll/download response chunks
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <curl/curl.h>
#include <cJSON.h>

#include "base32.h"
#include "base64.h"
#include "dns_proto.h"

/* Portable case-insensitive substring search (replaces strcasestr) */
static char *ci_strstr(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        size_t i;
        for (i = 0; i < nlen; i++) {
            if (tolower((unsigned char)haystack[i]) != tolower((unsigned char)needle[i]))
                break;
        }
        if (i == nlen) return (char *)haystack;
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Configuration
 * ═══════════════════════════════════════════════════════════════════════════ */

static struct {
    char api_key[512];
    char model[128];
    char tls_cert[256];
    char tls_key[256];
    int  use_dot;
    int  use_doh;
    int  port;
} g_config;

/* ═══════════════════════════════════════════════════════════════════════════
 * Session State
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_SESSIONS     64
#define MAX_CHUNKS       256
#define MAX_RESP_CHUNKS  512
#define CHUNK_SIZE       200  /* base64 chars per TXT response chunk */

typedef struct {
    int    seq;
    char   data[256]; /* base32-encoded chunk */
} upload_chunk_t;

typedef struct {
    int              valid;
    upload_chunk_t   chunks[MAX_CHUNKS];
    int              chunk_count;
} pending_prompt_t;

typedef struct {
    int   ready;
    int   failed;
    char *chunks[MAX_RESP_CHUNKS];
    int   chunk_count;
} msg_response_t;

typedef struct {
    char            id[32];
    int             active;
    cJSON          *history;       /* JSON array of {"role":..., "parts":...} */
    pending_prompt_t pending[64];  /* indexed by msgID */
    msg_response_t   responses[64];
} session_t;

static session_t   g_sessions[MAX_SESSIONS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ═══════════════════════════════════════════════════════════════════════════
 * Utility
 * ═══════════════════════════════════════════════════════════════════════════ */

static void generate_id(char *out, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len - 1 && i < 16; i++)
        out[i] = hex[rand() % 16];
    out[len > 16 ? 16 : len - 1] = '\0';
}

static char *load_env(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    if (v && v[0]) return strdup(v);
    return fallback ? strdup(fallback) : NULL;
}

/* Simple .env parser */
static void load_dotenv(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        /* Skip comments and empty lines */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';

        char *key = p;
        char *val = eq + 1;

        /* Trim key */
        char *ke = eq - 1;
        while (ke > key && (*ke == ' ' || *ke == '\t')) *ke-- = '\0';

        /* Trim val */
        while (*val == ' ' || *val == '\t' || *val == '"') val++;
        size_t vlen = strlen(val);
        while (vlen > 0 && (val[vlen-1] == '\n' || val[vlen-1] == '\r' ||
               val[vlen-1] == '"' || val[vlen-1] == ' '))
            val[--vlen] = '\0';

        setenv(key, val, 0); /* Don't overwrite existing */
    }
    fclose(f);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Gemini REST API Client
 * ═══════════════════════════════════════════════════════════════════════════ */

struct curl_buf {
    char  *data;
    size_t len;
    size_t cap;
};

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *ud)
{
    size_t total = size * nmemb;
    struct curl_buf *b = (struct curl_buf *)ud;
    if (b->len + total + 1 > b->cap) {
        size_t newcap = (b->cap + total + 1) * 2;
        char *tmp = realloc(b->data, newcap);
        if (!tmp) return 0;
        b->data = tmp;
        b->cap = newcap;
    }
    memcpy(b->data + b->len, ptr, total);
    b->len += total;
    b->data[b->len] = '\0';
    return total;
}

/*
 * Call Gemini generateContent API.
 * `history` is the JSON array of content objects.
 * Returns the parsed JSON response, or NULL on error.
 * Caller must cJSON_Delete the result.
 */
static cJSON *gemini_generate_content(cJSON *history)
{
    /* Build request body */
    cJSON *body = cJSON_CreateObject();

    /* System instruction */
    cJSON *sys = cJSON_CreateObject();
    cJSON *sys_parts = cJSON_CreateArray();
    cJSON *sys_part = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_part, "text",
        "You are a remote, DNS-based AI agent. You have the ability to execute "
        "terminal commands on the user's machine by calling the client_execute_bash "
        "tool. You can also read files using client_read_file. When the user asks "
        "you to do something to their local system, utilize your tools. Format all "
        "outputs nicely using markdown.");
    cJSON_AddItemToArray(sys_parts, sys_part);
    cJSON_AddItemToObject(sys, "parts", sys_parts);
    cJSON_AddItemToObject(body, "systemInstruction", sys);

    /* Contents (conversation history) */
    cJSON *contents_copy = cJSON_Duplicate(history, 1);
    cJSON_AddItemToObject(body, "contents", contents_copy);

    /* Tools */
    cJSON *tools = cJSON_CreateArray();
    cJSON *tool = cJSON_CreateObject();
    cJSON *funcs = cJSON_CreateArray();

    /* client_execute_bash */
    cJSON *fn1 = cJSON_CreateObject();
    cJSON_AddStringToObject(fn1, "name", "client_execute_bash");
    cJSON_AddStringToObject(fn1, "description",
        "Executes a bash command on the client's local machine and returns stdout/stderr.");
    cJSON *fn1_params = cJSON_CreateObject();
    cJSON_AddStringToObject(fn1_params, "type", "OBJECT");
    cJSON *fn1_props = cJSON_CreateObject();
    cJSON *fn1_cmd = cJSON_CreateObject();
    cJSON_AddStringToObject(fn1_cmd, "type", "STRING");
    cJSON_AddStringToObject(fn1_cmd, "description", "The bash command to execute.");
    cJSON_AddItemToObject(fn1_props, "command", fn1_cmd);
    cJSON_AddItemToObject(fn1_params, "properties", fn1_props);
    cJSON *fn1_req = cJSON_CreateArray();
    cJSON_AddItemToArray(fn1_req, cJSON_CreateString("command"));
    cJSON_AddItemToObject(fn1_params, "required", fn1_req);
    cJSON_AddItemToObject(fn1, "parameters", fn1_params);
    cJSON_AddItemToArray(funcs, fn1);

    /* client_read_file */
    cJSON *fn2 = cJSON_CreateObject();
    cJSON_AddStringToObject(fn2, "name", "client_read_file");
    cJSON_AddStringToObject(fn2, "description",
        "Reads a file on the client's local machine.");
    cJSON *fn2_params = cJSON_CreateObject();
    cJSON_AddStringToObject(fn2_params, "type", "OBJECT");
    cJSON *fn2_props = cJSON_CreateObject();
    cJSON *fn2_fp = cJSON_CreateObject();
    cJSON_AddStringToObject(fn2_fp, "type", "STRING");
    cJSON_AddStringToObject(fn2_fp, "description", "Path to the file.");
    cJSON_AddItemToObject(fn2_props, "filepath", fn2_fp);
    cJSON_AddItemToObject(fn2_params, "properties", fn2_props);
    cJSON *fn2_req = cJSON_CreateArray();
    cJSON_AddItemToArray(fn2_req, cJSON_CreateString("filepath"));
    cJSON_AddItemToObject(fn2_params, "required", fn2_req);
    cJSON_AddItemToObject(fn2, "parameters", fn2_params);
    cJSON_AddItemToArray(funcs, fn2);

    cJSON_AddItemToObject(tool, "functionDeclarations", funcs);
    cJSON_AddItemToArray(tools, tool);
    cJSON_AddItemToObject(body, "tools", tools);

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    /* Build URL */
    char url[1024];
    snprintf(url, sizeof(url),
        "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent?key=%s",
        g_config.model, g_config.api_key);

    /* Execute request */
    CURL *curl = curl_easy_init();
    if (!curl) { free(body_str); return NULL; }

    struct curl_buf resp = { .data = malloc(4096), .len = 0, .cap = 4096 };
    if (!resp.data) { curl_easy_cleanup(curl); free(body_str); return NULL; }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body_str);

    if (res != CURLE_OK) {
        fprintf(stderr, "[gemini] curl error: %s\n", curl_easy_strerror(res));
        free(resp.data);
        return NULL;
    }
    if (http_code != 200) {
        fprintf(stderr, "[gemini] HTTP %ld: %s\n", http_code, resp.data);
        free(resp.data);
        return NULL;
    }

    cJSON *json = cJSON_Parse(resp.data);
    free(resp.data);
    return json;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LLM Processing (runs in a background thread)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    session_t *sess;
    int        msg_id;
} llm_task_t;

static void *process_llm_thread(void *arg)
{
    llm_task_t *task = (llm_task_t *)arg;
    session_t *sess = task->sess;
    int msg_id = task->msg_id;
    free(task);

    /* 1. Reassemble uploaded chunks */
    pthread_mutex_lock(&g_lock);
    pending_prompt_t *pp = &sess->pending[msg_id];

    int max_seq = -1;
    for (int i = 0; i < pp->chunk_count; i++) {
        if (pp->chunks[i].seq > max_seq) max_seq = pp->chunks[i].seq;
    }

    /* Sort by seq and concatenate base32 data */
    char b32_combined[65536] = {0};
    for (int s = 0; s <= max_seq; s++) {
        for (int i = 0; i < pp->chunk_count; i++) {
            if (pp->chunks[i].seq == s) {
                strcat(b32_combined, pp->chunks[i].data);
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_lock);

    /* Convert to uppercase for decoding */
    for (char *p = b32_combined; *p; p++)
        *p = (char)toupper((unsigned char)*p);

    /* Base32 decode */
    size_t b32_len = strlen(b32_combined);
    uint8_t payload_bytes[65536];
    int decoded_len = base32_decode(b32_combined, payload_bytes,
                                    sizeof(payload_bytes));
    if (decoded_len < 0) {
        fprintf(stderr, "[llm] base32 decode failed (input len=%zu)\n", b32_len);
        pthread_mutex_lock(&g_lock);
        sess->responses[msg_id].failed = 1;
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    payload_bytes[decoded_len] = '\0';

    /* Parse JSON payload: {"type":"user|tool_response","content":"...","tool_name":"..."} */
    cJSON *payload = cJSON_Parse((char *)payload_bytes);
    if (!payload) {
        fprintf(stderr, "[llm] JSON parse failed: %s\n", (char *)payload_bytes);
        pthread_mutex_lock(&g_lock);
        sess->responses[msg_id].failed = 1;
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "type"));
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "content"));
    const char *tool_name = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "tool_name"));

    if (!type || !content) {
        fprintf(stderr, "[llm] invalid payload fields\n");
        cJSON_Delete(payload);
        pthread_mutex_lock(&g_lock);
        sess->responses[msg_id].failed = 1;
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    fprintf(stderr, "[llm] Processing sid=%s mid=%d type=%s\n",
            sess->id, msg_id, type);

    /* 2. Build the content entry for the conversation history */
    pthread_mutex_lock(&g_lock);

    if (strcmp(type, "user") == 0) {
        /* Add user message to history */
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "role", "user");
        cJSON *parts = cJSON_CreateArray();
        cJSON *part = cJSON_CreateObject();
        cJSON_AddStringToObject(part, "text", content);
        cJSON_AddItemToArray(parts, part);
        cJSON_AddItemToObject(entry, "parts", parts);
        cJSON_AddItemToArray(sess->history, entry);
    } else if (strcmp(type, "tool_response") == 0 && tool_name) {
        /* Add function response to history */
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "role", "user");
        cJSON *parts = cJSON_CreateArray();
        cJSON *part = cJSON_CreateObject();

        cJSON *func_resp = cJSON_CreateObject();
        cJSON_AddStringToObject(func_resp, "name", tool_name);
        cJSON *resp_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(resp_obj, "result", content);
        cJSON_AddItemToObject(func_resp, "response", resp_obj);
        cJSON_AddItemToObject(part, "functionResponse", func_resp);

        cJSON_AddItemToArray(parts, part);
        cJSON_AddItemToObject(entry, "parts", parts);
        cJSON_AddItemToArray(sess->history, entry);
    }

    /* Make a copy of history for the API call (release lock during network I/O) */
    cJSON *history_copy = cJSON_Duplicate(sess->history, 1);
    pthread_mutex_unlock(&g_lock);

    cJSON_Delete(payload);

    /* 3. Call Gemini API */
    cJSON *api_resp = gemini_generate_content(history_copy);
    cJSON_Delete(history_copy);

    if (!api_resp) {
        fprintf(stderr, "[llm] Gemini API call failed\n");
        pthread_mutex_lock(&g_lock);
        sess->responses[msg_id].failed = 1;
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    /* 4. Parse response — look for functionCall or text */
    cJSON *result_json = cJSON_CreateObject();
    cJSON *candidates = cJSON_GetObjectItem(api_resp, "candidates");
    cJSON *cand0 = candidates ? cJSON_GetArrayItem(candidates, 0) : NULL;
    cJSON *cand_content = cand0 ? cJSON_GetObjectItem(cand0, "content") : NULL;
    cJSON *cand_parts = cand_content ? cJSON_GetObjectItem(cand_content, "parts") : NULL;

    int is_tool_call = 0;

    if (cand_parts) {
        cJSON *cp;
        cJSON_ArrayForEach(cp, cand_parts) {
            cJSON *fc = cJSON_GetObjectItem(cp, "functionCall");
            if (fc) {
                is_tool_call = 1;
                cJSON_AddStringToObject(result_json, "type", "tool_call");
                const char *fn = cJSON_GetStringValue(cJSON_GetObjectItem(fc, "name"));
                if (fn) cJSON_AddStringToObject(result_json, "tool_name", fn);
                cJSON *args = cJSON_GetObjectItem(fc, "args");
                if (args) {
                    cJSON *args_copy = cJSON_Duplicate(args, 1);
                    cJSON_AddItemToObject(result_json, "tool_args", args_copy);
                }

                /* Add model's function call to history */
                pthread_mutex_lock(&g_lock);
                cJSON *model_entry = cJSON_CreateObject();
                cJSON_AddStringToObject(model_entry, "role", "model");
                cJSON *model_parts = cJSON_CreateArray();
                cJSON *model_part = cJSON_CreateObject();
                cJSON *fc_copy = cJSON_Duplicate(fc, 1);
                cJSON_AddItemToObject(model_part, "functionCall", fc_copy);
                cJSON_AddItemToArray(model_parts, model_part);
                cJSON_AddItemToObject(model_entry, "parts", model_parts);
                cJSON_AddItemToArray(sess->history, model_entry);
                pthread_mutex_unlock(&g_lock);

                break;
            }
        }
    }

    if (!is_tool_call) {
        /* Extract text */
        cJSON_AddStringToObject(result_json, "type", "text");

        char text_buf[65536] = {0};
        if (cand_parts) {
            cJSON *cp;
            cJSON_ArrayForEach(cp, cand_parts) {
                const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(cp, "text"));
                if (t) {
                    size_t cur = strlen(text_buf);
                    snprintf(text_buf + cur, sizeof(text_buf) - cur, "%s", t);
                }
            }
        }
        cJSON_AddStringToObject(result_json, "content", text_buf);

        /* Add model response to history */
        pthread_mutex_lock(&g_lock);
        cJSON *model_entry = cJSON_CreateObject();
        cJSON_AddStringToObject(model_entry, "role", "model");
        cJSON *model_parts = cJSON_CreateArray();
        cJSON *model_part = cJSON_CreateObject();
        cJSON_AddStringToObject(model_part, "text", text_buf);
        cJSON_AddItemToArray(model_parts, model_part);
        cJSON_AddItemToObject(model_entry, "parts", model_parts);
        cJSON_AddItemToArray(sess->history, model_entry);
        pthread_mutex_unlock(&g_lock);
    }

    cJSON_Delete(api_resp);

    /* 5. Encode response → base64 → chunk into TXT-sized pieces */
    char *result_str = cJSON_PrintUnformatted(result_json);
    cJSON_Delete(result_json);

    size_t result_len = strlen(result_str);
    size_t b64_len = base64_encoded_len(result_len) + 1;
    char *b64_buf = malloc(b64_len);
    base64_encode((uint8_t *)result_str, result_len, b64_buf, b64_len);
    free(result_str);

    /* Chunk into CHUNK_SIZE-character pieces */
    size_t b64_total = strlen(b64_buf);
    int nchunks = 0;

    pthread_mutex_lock(&g_lock);
    msg_response_t *mr = &sess->responses[msg_id];

    for (size_t i = 0; i < b64_total && nchunks < MAX_RESP_CHUNKS; i += CHUNK_SIZE) {
        size_t clen = b64_total - i;
        if (clen > CHUNK_SIZE) clen = CHUNK_SIZE;
        mr->chunks[nchunks] = malloc(clen + 1);
        memcpy(mr->chunks[nchunks], b64_buf + i, clen);
        mr->chunks[nchunks][clen] = '\0';
        nchunks++;
    }
    mr->chunk_count = nchunks;
    mr->ready = 1;
    pthread_mutex_unlock(&g_lock);

    free(b64_buf);

    fprintf(stderr, "[llm] Response ready sid=%s mid=%d chunks=%d\n",
            sess->id, msg_id, nchunks);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DNS Query Handler
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Handle a single DNS query. Writes the response into `resp_buf`.
 * Returns the response length, or -1 on error.
 */
static int handle_dns_query(const uint8_t *query, size_t query_len,
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

    /* Split into labels */
    char *parts[16];
    int nparts = 0;
    char qname_copy[512];
    strncpy(qname_copy, qname, sizeof(qname_copy) - 1);
    qname_copy[sizeof(qname_copy) - 1] = '\0';

    char *tok = strtok(qname_copy, ".");
    while (tok && nparts < 16) {
        parts[nparts++] = tok;
        tok = strtok(NULL, ".");
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
        sess->history = cJSON_CreateArray();
        pthread_mutex_unlock(&g_lock);

        fprintf(stderr, "[init] New session: %s\n", sess->id);
        return dns_build_response(qid, qname, DNS_RCODE_OK, sess->id,
                                  resp_buf, resp_buf_len);
    }

    /* ── <chunk>.<seq>.up.<mid>.<sid>.llm.local. ──────────────────── */
    if (nparts >= 7 && strcmp(parts[2], "up") == 0) {
        const char *chunk_b32 = parts[0];
        int seq = atoi(parts[1]);
        int mid = atoi(parts[3]);
        const char *sid = parts[4];

        if (mid < 0 || mid >= 64) {
            return dns_build_response(qid, qname, DNS_RCODE_FORMERR, NULL,
                                      resp_buf, resp_buf_len);
        }

        pthread_mutex_lock(&g_lock);
        session_t *sess = NULL;
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (g_sessions[i].active && strcmp(g_sessions[i].id, sid) == 0) {
                sess = &g_sessions[i];
                break;
            }
        }

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
        int mid = atoi(parts[1]);
        const char *sid = parts[2];

        if (mid < 0 || mid >= 64) {
            return dns_build_response(qid, qname, DNS_RCODE_FORMERR, NULL,
                                      resp_buf, resp_buf_len);
        }

        pthread_mutex_lock(&g_lock);
        session_t *sess = NULL;
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (g_sessions[i].active && strcmp(g_sessions[i].id, sid) == 0) {
                sess = &g_sessions[i];
                break;
            }
        }

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
        int seq = atoi(parts[0]);
        int mid = atoi(parts[1]);
        const char *sid = parts[3];

        if (mid < 0 || mid >= 64) {
            return dns_build_response(qid, qname, DNS_RCODE_FORMERR, NULL,
                                      resp_buf, resp_buf_len);
        }

        pthread_mutex_lock(&g_lock);
        session_t *sess = NULL;
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (g_sessions[i].active && strcmp(g_sessions[i].id, sid) == 0) {
                sess = &g_sessions[i];
                break;
            }
        }

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
            }
        }
        pthread_mutex_unlock(&g_lock);

        return dns_build_response(qid, qname, DNS_RCODE_OK, reply,
                                  resp_buf, resp_buf_len);
    }

    /* Unknown query */
    return dns_build_response(qid, qname, DNS_RCODE_NXDOMAIN, NULL,
                              resp_buf, resp_buf_len);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Server: UDP Listener
 * ═══════════════════════════════════════════════════════════════════════════ */

static void *udp_server_thread(void *arg)
{
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return NULL; }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)g_config.port),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return NULL;
    }

    fprintf(stderr, "[udp] Listening on 0.0.0.0:%d\n", g_config.port);

    uint8_t buf[DNS_MAX_MSG];
    while (1) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&client, &clen);
        if (n <= 0) continue;

        uint8_t resp[DNS_MAX_MSG];
        int rlen = handle_dns_query(buf, (size_t)n, resp, sizeof(resp));
        if (rlen > 0) {
            sendto(fd, resp, (size_t)rlen, 0,
                   (struct sockaddr *)&client, clen);
        }
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Server: DoT (TLS) Listener
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    SSL *ssl;
    int  fd;
} dot_client_t;

static void *dot_client_thread(void *arg)
{
    dot_client_t *dc = (dot_client_t *)arg;
    uint8_t lenbuf[2];

    while (1) {
        /* Read 2-byte length prefix */
        int nr = SSL_read(dc->ssl, lenbuf, 2);
        if (nr != 2) break;
        uint16_t msglen = (uint16_t)((lenbuf[0] << 8) | lenbuf[1]);
        if (msglen > DNS_MAX_MSG) break;

        uint8_t qbuf[DNS_MAX_MSG];
        int total = 0;
        while (total < msglen) {
            int r = SSL_read(dc->ssl, qbuf + total, msglen - total);
            if (r <= 0) goto done;
            total += r;
        }

        uint8_t rbuf[DNS_MAX_MSG];
        int rlen = handle_dns_query(qbuf, (size_t)msglen, rbuf, sizeof(rbuf));
        if (rlen > 0) {
            uint8_t rlb[2] = { (uint8_t)(rlen >> 8), (uint8_t)rlen };
            SSL_write(dc->ssl, rlb, 2);
            SSL_write(dc->ssl, rbuf, rlen);
        }
    }

done:
    SSL_shutdown(dc->ssl);
    SSL_free(dc->ssl);
    close(dc->fd);
    free(dc);
    return NULL;
}

static void *dot_server_thread(void *arg)
{
    (void)arg;

    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        fprintf(stderr, "[dot] SSL_CTX_new failed\n");
        return NULL;
    }

    if (SSL_CTX_use_certificate_file(ctx, g_config.tls_cert, SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ctx, g_config.tls_key, SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "[dot] Failed to load TLS certs: %s / %s\n",
                g_config.tls_cert, g_config.tls_key);
        SSL_CTX_free(ctx);
        return NULL;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); SSL_CTX_free(ctx); return NULL; }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)g_config.port),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        SSL_CTX_free(ctx);
        return NULL;
    }
    listen(fd, 32);

    fprintf(stderr, "[dot] TLS listening on 0.0.0.0:%d\n", g_config.port);

    while (1) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int cfd = accept(fd, (struct sockaddr *)&client, &clen);
        if (cfd < 0) continue;

        SSL *ssl = SSL_new(ctx);
        SSL_set_fd(ssl, cfd);
        if (SSL_accept(ssl) <= 0) {
            SSL_free(ssl);
            close(cfd);
            continue;
        }

        dot_client_t *dc = malloc(sizeof(dot_client_t));
        dc->ssl = ssl;
        dc->fd = cfd;

        pthread_t tid;
        pthread_create(&tid, NULL, dot_client_thread, dc);
        pthread_detach(tid);
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Server: DoH (HTTPS) Listener
 * ═══════════════════════════════════════════════════════════════════════════ */

static void *doh_client_thread(void *arg)
{
    dot_client_t *dc = (dot_client_t *)arg;

    /* Read HTTP request (simplified parser for POST /dns-query) */
    char http_buf[65536];
    int total = 0;
    int header_end = -1;

    /* Read until we find \r\n\r\n */
    while (total < (int)sizeof(http_buf) - 1) {
        int n = SSL_read(dc->ssl, http_buf + total, (int)sizeof(http_buf) - 1 - total);
        if (n <= 0) goto done;
        total += n;
        http_buf[total] = '\0';

        char *hdr_end = strstr(http_buf, "\r\n\r\n");
        if (hdr_end) {
            header_end = (int)(hdr_end - http_buf) + 4;
            break;
        }
    }

    if (header_end < 0) goto done;

    {
        /* Parse Content-Length */
        int content_length = 0;
        char *cl = ci_strstr(http_buf, "Content-Length:");
        if (cl) content_length = atoi(cl + 15);

        /* Read remaining body if needed */
        int body_so_far = total - header_end;
        while (body_so_far < content_length &&
               total < (int)sizeof(http_buf) - 1) {
            int n = SSL_read(dc->ssl, http_buf + total,
                            (int)sizeof(http_buf) - 1 - total);
            if (n <= 0) break;
            total += n;
            body_so_far = total - header_end;
        }

        /* Process DNS message from body */
        uint8_t *dns_msg = (uint8_t *)(http_buf + header_end);
        int dns_len = body_so_far;

        uint8_t resp[DNS_MAX_MSG];
        int rlen = handle_dns_query(dns_msg, (size_t)dns_len, resp, sizeof(resp));

        if (rlen > 0) {
            char http_resp[DNS_MAX_MSG + 256];
            int hlen = snprintf(http_resp, sizeof(http_resp),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/dns-message\r\n"
                "Content-Length: %d\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n"
                "\r\n", rlen);
            SSL_write(dc->ssl, http_resp, hlen);
            SSL_write(dc->ssl, resp, rlen);
        } else {
            const char *err = "HTTP/1.1 400 Bad Request\r\n"
                              "Content-Length: 0\r\n"
                              "Connection: close\r\n\r\n";
            SSL_write(dc->ssl, err, (int)strlen(err));
        }
    }

done:
    SSL_shutdown(dc->ssl);
    SSL_free(dc->ssl);
    close(dc->fd);
    free(dc);
    return NULL;
}

static void *doh_server_thread(void *arg)
{
    (void)arg;

    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        fprintf(stderr, "[doh] SSL_CTX_new failed\n");
        return NULL;
    }

    if (SSL_CTX_use_certificate_file(ctx, g_config.tls_cert, SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ctx, g_config.tls_key, SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "[doh] Failed to load TLS certs\n");
        SSL_CTX_free(ctx);
        return NULL;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); SSL_CTX_free(ctx); return NULL; }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)g_config.port),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind (DoH)");
        close(fd);
        SSL_CTX_free(ctx);
        return NULL;
    }
    listen(fd, 32);

    fprintf(stderr, "[doh] HTTPS listening on 0.0.0.0:%d\n", g_config.port);

    while (1) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int cfd = accept(fd, (struct sockaddr *)&client, &clen);
        if (cfd < 0) continue;

        SSL *ssl = SSL_new(ctx);
        SSL_set_fd(ssl, cfd);
        if (SSL_accept(ssl) <= 0) {
            SSL_free(ssl);
            close(cfd);
            continue;
        }

        dot_client_t *dc = malloc(sizeof(dot_client_t));
        dc->ssl = ssl;
        dc->fd = cfd;

        pthread_t tid;
        pthread_create(&tid, NULL, doh_client_thread, dc);
        pthread_detach(tid);
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    srand((unsigned)time(NULL));
    signal(SIGPIPE, SIG_IGN);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Load .env files */
    load_dotenv("../.env");
    load_dotenv(".env");

    /* Read config */
    char *api_key = load_env("GEMINI_API_KEY", NULL);
    if (!api_key || !api_key[0]) {
        fprintf(stderr, "FATAL: GEMINI_API_KEY not set in .env or environment\n");
        return 1;
    }
    strncpy(g_config.api_key, api_key, sizeof(g_config.api_key) - 1);
    free(api_key);

    char *model = load_env("GEMINI_MODEL", "gemini-2.5-flash");
    strncpy(g_config.model, model, sizeof(g_config.model) - 1);
    free(model);

    g_config.use_dot = getenv("USE_DOT") && strcmp(getenv("USE_DOT"), "true") == 0;
    g_config.use_doh = getenv("USE_DOH") && strcmp(getenv("USE_DOH"), "true") == 0;

    char *port_str = load_env("SERVER_PORT", NULL);
    if (port_str) {
        g_config.port = atoi(port_str);
        free(port_str);
    }

    if (g_config.port == 0) {
        if (g_config.use_dot)      g_config.port = 853;
        else if (g_config.use_doh) g_config.port = 443;
        else                       g_config.port = 53535;
    }

    char *cert = load_env("TLS_CERT", "cert.pem");
    strncpy(g_config.tls_cert, cert, sizeof(g_config.tls_cert) - 1);
    free(cert);

    char *key = load_env("TLS_KEY", "key.pem");
    strncpy(g_config.tls_key, key, sizeof(g_config.tls_key) - 1);
    free(key);

    /* Start the appropriate server */
    fprintf(stderr,
        "╔══════════════════════════════════════════════════════╗\n"
        "║   Agentic DNS-LLM Server (C)                        ║\n"
        "║   Model: %-40s  ║\n"
        "║   Mode:  %-40s  ║\n"
        "║   Port:  %-40d  ║\n"
        "╚══════════════════════════════════════════════════════╝\n",
        g_config.model,
        g_config.use_doh ? "DoH (HTTPS)" :
        g_config.use_dot ? "DoT (TLS)"   : "UDP (plain)",
        g_config.port);

    if (g_config.use_doh) {
        doh_server_thread(NULL);
    } else if (g_config.use_dot) {
        dot_server_thread(NULL);
    } else {
        udp_server_thread(NULL);
    }

    curl_global_cleanup();
    return 0;
}
