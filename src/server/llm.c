/*
 * server/llm.c — Gemini LLM interaction (API call + processing thread)
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <cJSON.h>

#include "base32.h"
#include "base64.h"
#include "crypto.h"
#include "protocol.h"
#include "server/llm.h"
#include "server/log.h"
#include "server/session.h"
#include "server/transport.h"  /* g_config */

/* ── Curl write callback ──────────────────────────────────────────────────── */

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

/* ── Gemini API call ──────────────────────────────────────────────────────── */

cJSON *gemini_generate_content(cJSON *history)
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

    /* client_write_file */
    cJSON *fn3 = cJSON_CreateObject();
    cJSON_AddStringToObject(fn3, "name", "client_write_file");
    cJSON_AddStringToObject(fn3, "description",
        "Writes content to a file on the client's local machine. Creates or overwrites.");
    cJSON *fn3_params = cJSON_CreateObject();
    cJSON_AddStringToObject(fn3_params, "type", "OBJECT");
    cJSON *fn3_props = cJSON_CreateObject();
    cJSON *fn3_fp = cJSON_CreateObject();
    cJSON_AddStringToObject(fn3_fp, "type", "STRING");
    cJSON_AddStringToObject(fn3_fp, "description", "Path to write to.");
    cJSON_AddItemToObject(fn3_props, "filepath", fn3_fp);
    cJSON *fn3_ct = cJSON_CreateObject();
    cJSON_AddStringToObject(fn3_ct, "type", "STRING");
    cJSON_AddStringToObject(fn3_ct, "description", "Content to write.");
    cJSON_AddItemToObject(fn3_props, "content", fn3_ct);
    cJSON_AddItemToObject(fn3_params, "properties", fn3_props);
    cJSON *fn3_req = cJSON_CreateArray();
    cJSON_AddItemToArray(fn3_req, cJSON_CreateString("filepath"));
    cJSON_AddItemToArray(fn3_req, cJSON_CreateString("content"));
    cJSON_AddItemToObject(fn3_params, "required", fn3_req);
    cJSON_AddItemToObject(fn3, "parameters", fn3_params);
    cJSON_AddItemToArray(funcs, fn3);

    /* client_list_directory */
    cJSON *fn4 = cJSON_CreateObject();
    cJSON_AddStringToObject(fn4, "name", "client_list_directory");
    cJSON_AddStringToObject(fn4, "description",
        "Lists files and directories at a given path on the client's machine.");
    cJSON *fn4_params = cJSON_CreateObject();
    cJSON_AddStringToObject(fn4_params, "type", "OBJECT");
    cJSON *fn4_props = cJSON_CreateObject();
    cJSON *fn4_path = cJSON_CreateObject();
    cJSON_AddStringToObject(fn4_path, "type", "STRING");
    cJSON_AddStringToObject(fn4_path, "description", "Directory path to list. Defaults to current dir.");
    cJSON_AddItemToObject(fn4_props, "path", fn4_path);
    cJSON_AddItemToObject(fn4_params, "properties", fn4_props);
    cJSON_AddItemToObject(fn4, "parameters", fn4_params);
    cJSON_AddItemToArray(funcs, fn4);

    cJSON_AddItemToObject(tool, "functionDeclarations", funcs);
    cJSON_AddItemToArray(tools, tool);
    cJSON_AddItemToObject(body, "tools", tools);

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    /* Build URL */
    char url[1024];
    snprintf(url, sizeof(url),
        "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent",
        g_config.model);

    /* Execute request */
    CURL *curl = curl_easy_init();
    if (!curl) { free(body_str); return NULL; }

    struct curl_buf resp = { .data = malloc(4096), .len = 0, .cap = 4096 };
    if (!resp.data) { curl_easy_cleanup(curl); free(body_str); return NULL; }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    char key_header[600];
    snprintf(key_header, sizeof(key_header), "x-goog-api-key: %s", g_config.api_key);
    headers = curl_slist_append(headers, key_header);

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

/* ── LLM Processing Thread ────────────────────────────────────────────────── */

void *process_llm_thread(void *arg)
{
    llm_task_t *task = (llm_task_t *)arg;
    session_t *sess = task->sess;
    int msg_id = task->msg_id;
    free(task);

    /* Mark session as busy so the reaper won't destroy it mid-flight */
    pthread_mutex_lock(&g_lock);
    sess->busy++;
    pthread_mutex_unlock(&g_lock);

    /* 1. Reassemble uploaded chunks */
    pthread_mutex_lock(&g_lock);
    pending_prompt_t *pp = &sess->pending[msg_id];

    int max_seq = -1;
    for (int i = 0; i < pp->chunk_count; i++) {
        if (pp->chunks[i].seq > max_seq) max_seq = pp->chunks[i].seq;
    }

    /* Sort by seq and concatenate base32 data */
    char b32_combined[UPLOAD_B32_MAX];
    size_t b32_pos = 0;
    b32_combined[0] = '\0';
    for (int s = 0; s <= max_seq; s++) {
        for (int i = 0; i < pp->chunk_count; i++) {
            if (pp->chunks[i].seq == s) {
                size_t dlen = strlen(pp->chunks[i].data);
                if (b32_pos + dlen >= sizeof(b32_combined)) {
                    fprintf(stderr, "[llm] b32 reassembly overflow\n");
                    pthread_mutex_unlock(&g_lock);
                    goto done;
                }
                memcpy(b32_combined + b32_pos, pp->chunks[i].data, dlen);
                b32_pos += dlen;
                break;
            }
        }
    }
    b32_combined[b32_pos] = '\0';
    pthread_mutex_unlock(&g_lock);

    /* Convert to uppercase for decoding */
    for (char *p = b32_combined; *p; p++)
        *p = (char)toupper((unsigned char)*p);

    /* Base32 decode */
    uint8_t payload_bytes[65536];
    int decoded_len = base32_decode(b32_combined, payload_bytes,
                                    sizeof(payload_bytes));
    if (decoded_len < 0) {
        fprintf(stderr, "[llm] base32 decode failed (input len=%zu)\n", b32_pos);
        pthread_mutex_lock(&g_lock);
        sess->responses[msg_id].failed = 1;
        pthread_mutex_unlock(&g_lock);
        goto done;
    }
    payload_bytes[decoded_len] = '\0';

    /* Decrypt payload if PSK is configured */
    if (tunnel_crypto_enabled() && decoded_len > (int)CRYPTO_OVERHEAD) {
        uint8_t decrypted[65536];
        size_t dec_len = 0;
        if (tunnel_decrypt(payload_bytes, (size_t)decoded_len,
                           decrypted, &dec_len) == 0) {
            memcpy(payload_bytes, decrypted, dec_len);
            payload_bytes[dec_len] = '\0';
            decoded_len = (int)dec_len;
        } else {
            log_err("llm", "Payload decryption failed — PSK mismatch or corruption");
            pthread_mutex_lock(&g_lock);
            sess->responses[msg_id].failed = 1;
            pthread_mutex_unlock(&g_lock);
            goto done;
        }
    }

    /* Parse JSON payload: {"type":"user|tool_response","content":"...","tool_name":"..."} */
    cJSON *payload = cJSON_Parse((char *)payload_bytes);
    if (!payload) {
        fprintf(stderr, "[llm] JSON parse failed: %s\n", (char *)payload_bytes);
        pthread_mutex_lock(&g_lock);
        sess->responses[msg_id].failed = 1;
        pthread_mutex_unlock(&g_lock);
        goto done;
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
        goto done;
    }

    fprintf(stderr, "[llm] Processing sid=%s mid=%d type=%s\n",
            sess->id, msg_id, type);

    /* 2. Build the content entry for the conversation history */
    pthread_mutex_lock(&g_lock);

    if (strcmp(type, "user") == 0) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "role", "user");
        cJSON *parts = cJSON_CreateArray();
        cJSON *part = cJSON_CreateObject();
        cJSON_AddStringToObject(part, "text", content);
        cJSON_AddItemToArray(parts, part);
        cJSON_AddItemToObject(entry, "parts", parts);
        cJSON_AddItemToArray(sess->history, entry);
    } else if (strcmp(type, "tool_response") == 0 && tool_name) {
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
        goto done;
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
                break;
            }
        }
    }

    if (!is_tool_call) {
        cJSON_AddStringToObject(result_json, "type", "text");

        #define TEXT_BUF_SIZE 65536
        char *text_buf = calloc(TEXT_BUF_SIZE, 1);
        if (text_buf && cand_parts) {
            cJSON *cp;
            size_t cur = 0;
            cJSON_ArrayForEach(cp, cand_parts) {
                const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(cp, "text"));
                if (t) {
                    int written = snprintf(text_buf + cur, TEXT_BUF_SIZE - cur, "%s", t);
                    if (written > 0) cur += (size_t)written;
                    if (cur >= TEXT_BUF_SIZE - 1) {
                        log_warn("llm", "Response text truncated at %zu bytes", cur);
                        break;
                    }
                }
            }
        }
        cJSON_AddStringToObject(result_json, "content", text_buf ? text_buf : "");
        free(text_buf);
        #undef TEXT_BUF_SIZE
    }

    /*
     * Add model response to history — duplicate the entire content object
     * from the API response to preserve thoughtSignature fields, thinking
     * parts, and any other metadata.
     */
    if (cand_content) {
        pthread_mutex_lock(&g_lock);
        cJSON *history_entry = cJSON_Duplicate(cand_content, 1);
        cJSON_AddItemToArray(sess->history, history_entry);
        pthread_mutex_unlock(&g_lock);
    }

    cJSON_Delete(api_resp);

    /* 5. Encode response → [encrypt →] base64 → chunk into TXT-sized pieces */
    char *result_str = cJSON_PrintUnformatted(result_json);
    cJSON_Delete(result_json);

    size_t result_len = strlen(result_str);

    /* Encrypt response if PSK is configured */
    uint8_t *encode_data = (uint8_t *)result_str;
    size_t encode_len = result_len;
    uint8_t *encrypted = NULL;
    if (tunnel_crypto_enabled()) {
        encrypted = malloc(result_len + CRYPTO_OVERHEAD);
        if (encrypted &&
            tunnel_encrypt((uint8_t *)result_str, result_len,
                           encrypted, &encode_len) == 0) {
            encode_data = encrypted;
        } else {
            log_err("llm", "Response encryption failed");
            free(encrypted);
            free(result_str);
            pthread_mutex_lock(&g_lock);
            sess->responses[msg_id].failed = 1;
            pthread_mutex_unlock(&g_lock);
            goto done;
        }
    }

    size_t b64_len = base64_encoded_len(encode_len) + 1;
    char *b64_buf = malloc(b64_len);
    base64_encode(encode_data, encode_len, b64_buf, b64_len);
    free(encrypted);
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

done:
    /* Release busy refcount */
    pthread_mutex_lock(&g_lock);
    sess->busy--;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}
