/*
 * server/llm.c — Multi-provider LLM interaction
 *
 * Supports Gemini, OpenAI, Anthropic (Claude), and OpenRouter.
 * Each provider has its own API call, tool builder, history format,
 * and response parser, dispatched via g_config.provider.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>
#include <cJSON.h>

#include "base32.h"
#include "base64.h"
#include "crypto.h"
#include "protocol.h"
#include "server/llm.h"
#include "server/log.h"
#include "server/session.h"
#include "server/transport.h"

/* ── Provider tables ─────────────────────────────────────────────────────── */

const char *PROVIDER_NAMES[] = {
    "Gemini", "OpenAI", "Claude", "OpenRouter"
};

const char *PROVIDER_DEFAULT_MODELS[] = {
    "gemini-2.5-flash", "gpt-5.4", "claude-sonnet-4-6",
    "openrouter/auto"
};

static const char *DEFAULT_SYSTEM_PROMPT =
    "You are an agentic AI assistant running through a DNS tunnel (DNS-CLAW). "
    "You have direct access to the user's local machine through tools.\n\n"
    "# Tools\n"
    "You have these tools — use them proactively:\n"
    "- `client_execute_bash` — Run any shell command. Use for: git, builds, "
    "tests, package managers, system info, anything CLI.\n"
    "- `client_read_file` — Read file contents. Use to understand code before "
    "modifying it.\n"
    "- `client_write_file` — Create or overwrite a file.\n"
    "- `client_edit_file` — Surgical find-and-replace within a file. Preferred "
    "over write_file for modifying existing files — it only changes what needs "
    "changing.\n"
    "- `client_list_directory` — List files and directories with sizes.\n"
    "- `client_search_files` — Grep/search file contents recursively. Use to "
    "find definitions, usages, patterns across a codebase.\n"
    "- `client_fetch_url` — HTTP GET a URL and return the body. Use for APIs, "
    "docs, or web content.\n\n"
    "# Behavior\n"
    "- **Be agentic**: When asked to do something, DO it with tools. Don't just "
    "explain how — execute commands, read code, make changes.\n"
    "- **Read before writing**: Always read a file before modifying it. Use "
    "client_edit_file for surgical changes, client_write_file only for new files "
    "or complete rewrites.\n"
    "- **Explore first**: When working on unfamiliar code, use list_directory "
    "and search_files to understand the structure before diving in.\n"
    "- **Chain tools**: Complex tasks require multiple tool calls. Plan your "
    "approach, then execute step by step.\n"
    "- **Verify your work**: After making changes, run tests or builds to confirm "
    "things work.\n"
    "- **Be concise**: Format output with markdown. Use code blocks for code, "
    "headers for sections. Don't be verbose.\n"
    "- **Handle errors**: If a command fails, read the error, diagnose it, and "
    "try a different approach.\n";

static const char *get_system_prompt(void)
{
    const char *custom = getenv("SYSTEM_PROMPT");
    if (custom && custom[0]) return custom;
    return DEFAULT_SYSTEM_PROMPT;
}

/* ── Tool definition table ───────────────────────────────────────────────── */

const tool_def_t TOOL_DEFS[] = {
    {"client_execute_bash",
     "Execute a shell command on the user's machine. Returns stdout+stderr. "
     "Use for: git, builds, tests, installs, system info, any CLI operation.",
     {{"command", "The bash command to execute."}},
     1, {"command"}, 1},

    {"client_read_file",
     "Read a file's contents. Always read before modifying.",
     {{"filepath", "Absolute or relative path to the file."}},
     1, {"filepath"}, 1},

    {"client_write_file",
     "Create a new file or completely overwrite an existing one. For surgical "
     "edits to existing files, prefer client_edit_file instead.",
     {{"filepath", "Path to write to."}, {"content", "Full file content."}},
     2, {"filepath", "content"}, 2},

    {"client_list_directory",
     "List files and subdirectories with type and size info.",
     {{"path", "Directory path to list. Defaults to current dir."}},
     1, {NULL}, 0},

    {"client_edit_file",
     "Apply a find-and-replace edit to an existing file. The old_string must "
     "match exactly (including whitespace/indentation). Only the first "
     "occurrence is replaced. Use this for surgical changes instead of "
     "rewriting entire files.",
     {{"filepath", "Path to the file to edit."},
      {"old_string", "Exact text to find (must be unique in the file)."},
      {"new_string", "Replacement text."}},
     3, {"filepath", "old_string", "new_string"}, 3},

    {"client_search_files",
     "Search file contents recursively using a pattern (grep -rn). Returns "
     "matching lines with file paths and line numbers. Use to find "
     "definitions, usages, imports, or any text pattern in a codebase.",
     {{"pattern", "Search pattern (basic regex)."},
      {"path", "Directory to search in. Defaults to current dir."},
      {"include", "File glob filter, e.g. '*.c' or '*.py'. Optional."}},
     3, {"pattern"}, 1},

    {"client_fetch_url",
     "Fetch the contents of a URL via HTTP GET. Returns the response body. "
     "Use for documentation, APIs, or web content.",
     {{"url", "The URL to fetch."}},
     1, {"url"}, 1},
};

/* ── Curl helpers ────────────────────────────────────────────────────────── */

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
        size_t needed = b->cap + total + 1;
        if (needed > SIZE_MAX / 2) return 0;  /* overflow guard */
        size_t newcap = needed * 2;
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

/* Generic HTTP POST with retry — returns parsed JSON or NULL */
static cJSON *llm_http_post(const char *url, struct curl_slist *extra_headers,
                            const char *body_str)
{
    int max_retries = 3;
    unsigned int backoff_ms = 1000;

    for (int attempt = 0; attempt < max_retries; attempt++) {
        if (attempt > 0) {
            log_warn("http", "Retry %d/%d after %ums...",
                     attempt, max_retries - 1, backoff_ms);
            usleep(backoff_ms * 1000);
            backoff_ms *= 2;
        }

        CURL *curl = curl_easy_init();
        if (!curl) return NULL;

        struct curl_buf resp = { .data = malloc(4096), .len = 0, .cap = 4096 };
        if (!resp.data) { curl_easy_cleanup(curl); return NULL; }

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        for (struct curl_slist *h = extra_headers; h; h = h->next)
            headers = curl_slist_append(headers, h->data);

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

        if (res != CURLE_OK) {
            log_err("http", "curl error: %s", curl_easy_strerror(res));
            free(resp.data);
            continue;  /* retry on network errors */
        }

        /* Retry on 429 (rate limit) and 5xx (server errors) */
        if (http_code == 429 || (http_code >= 500 && http_code < 600)) {
            log_err("http", "HTTP %ld (retryable): %.200s", http_code, resp.data);
            free(resp.data);
            continue;
        }

        if (http_code != 200) {
            log_err("http", "HTTP %ld: %.500s", http_code, resp.data);
            free(resp.data);
            return NULL;  /* non-retryable error */
        }

        cJSON *json = cJSON_Parse(resp.data);
        free(resp.data);
        return json;
    }

    log_err("http", "All %d attempts failed", max_retries);
    return NULL;
}

/* ── Tool builders (from TOOL_DEFS table) ────────────────────────────────── */

static cJSON *build_gemini_tools(void)
{
    cJSON *arr = cJSON_CreateArray();
    cJSON *wrapper = cJSON_CreateObject();
    cJSON *funcs = cJSON_CreateArray();
    for (int i = 0; i < NUM_TOOLS; i++) {
        const tool_def_t *td = &TOOL_DEFS[i];
        cJSON *fn = cJSON_CreateObject();
        cJSON_AddStringToObject(fn, "name", td->name);
        cJSON_AddStringToObject(fn, "description", td->description);
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "type", "OBJECT");
        cJSON *props = cJSON_CreateObject();
        for (int j = 0; j < td->nparam; j++) {
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "type", "STRING");
            cJSON_AddStringToObject(p, "description", td->params[j].pdesc);
            cJSON_AddItemToObject(props, td->params[j].pname, p);
        }
        cJSON_AddItemToObject(params, "properties", props);
        if (td->nrequired > 0) {
            cJSON *req = cJSON_CreateArray();
            for (int j = 0; j < td->nrequired; j++)
                cJSON_AddItemToArray(req, cJSON_CreateString(td->required[j]));
            cJSON_AddItemToObject(params, "required", req);
        }
        cJSON_AddItemToObject(fn, "parameters", params);
        cJSON_AddItemToArray(funcs, fn);
    }
    cJSON_AddItemToObject(wrapper, "functionDeclarations", funcs);
    cJSON_AddItemToArray(arr, wrapper);
    return arr;
}

static cJSON *build_openai_tools(void)
{
    cJSON *tools = cJSON_CreateArray();
    for (int i = 0; i < NUM_TOOLS; i++) {
        const tool_def_t *td = &TOOL_DEFS[i];
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "function");
        cJSON *fn = cJSON_CreateObject();
        cJSON_AddStringToObject(fn, "name", td->name);
        cJSON_AddStringToObject(fn, "description", td->description);
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "type", "object");
        cJSON *props = cJSON_CreateObject();
        for (int j = 0; j < td->nparam; j++) {
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "type", "string");
            cJSON_AddStringToObject(p, "description", td->params[j].pdesc);
            cJSON_AddItemToObject(props, td->params[j].pname, p);
        }
        cJSON_AddItemToObject(params, "properties", props);
        if (td->nrequired > 0) {
            cJSON *req = cJSON_CreateArray();
            for (int j = 0; j < td->nrequired; j++)
                cJSON_AddItemToArray(req, cJSON_CreateString(td->required[j]));
            cJSON_AddItemToObject(params, "required", req);
        }
        cJSON_AddItemToObject(fn, "parameters", params);
        cJSON_AddItemToObject(t, "function", fn);
        cJSON_AddItemToArray(tools, t);
    }
    return tools;
}

static cJSON *build_anthropic_tools(void)
{
    cJSON *tools = cJSON_CreateArray();
    for (int i = 0; i < NUM_TOOLS; i++) {
        const tool_def_t *td = &TOOL_DEFS[i];
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", td->name);
        cJSON_AddStringToObject(t, "description", td->description);
        cJSON *schema = cJSON_CreateObject();
        cJSON_AddStringToObject(schema, "type", "object");
        cJSON *props = cJSON_CreateObject();
        for (int j = 0; j < td->nparam; j++) {
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "type", "string");
            cJSON_AddStringToObject(p, "description", td->params[j].pdesc);
            cJSON_AddItemToObject(props, td->params[j].pname, p);
        }
        cJSON_AddItemToObject(schema, "properties", props);
        if (td->nrequired > 0) {
            cJSON *req = cJSON_CreateArray();
            for (int j = 0; j < td->nrequired; j++)
                cJSON_AddItemToArray(req, cJSON_CreateString(td->required[j]));
            cJSON_AddItemToObject(schema, "required", req);
        }
        cJSON_AddItemToObject(t, "input_schema", schema);
        cJSON_AddItemToArray(tools, t);
    }
    return tools;
}

/* ── History entry builders (provider-dispatched) ────────────────────────── */

void history_add_user_msg(cJSON *history, const char *content)
{
    switch (g_config.provider) {
    case PROVIDER_GEMINI: {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "role", "user");
        cJSON *parts = cJSON_CreateArray();
        cJSON *part = cJSON_CreateObject();
        cJSON_AddStringToObject(part, "text", content);
        cJSON_AddItemToArray(parts, part);
        cJSON_AddItemToObject(entry, "parts", parts);
        cJSON_AddItemToArray(history, entry);
        break;
    }
    case PROVIDER_OPENAI:
    case PROVIDER_OPENROUTER:
    case PROVIDER_ANTHROPIC: {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "role", "user");
        cJSON_AddStringToObject(entry, "content", content);
        cJSON_AddItemToArray(history, entry);
        break;
    }
    }
}

/* Extract last tool call ID from history for OpenAI/OpenRouter */
static const char *extract_openai_tool_id(cJSON *history)
{
    int n = cJSON_GetArraySize(history);
    for (int i = n - 1; i >= 0; i--) {
        cJSON *entry = cJSON_GetArrayItem(history, i);
        cJSON *tc = cJSON_GetObjectItem(entry, "tool_calls");
        if (tc) {
            cJSON *tc0 = cJSON_GetArrayItem(tc, 0);
            if (tc0) {
                const char *id = cJSON_GetStringValue(
                    cJSON_GetObjectItem(tc0, "id"));
                if (id) return id;
            }
        }
    }
    return "call_0";
}

/* Extract last tool_use ID from history for Anthropic */
static const char *extract_anthropic_tool_id(cJSON *history)
{
    int n = cJSON_GetArraySize(history);
    for (int i = n - 1; i >= 0; i--) {
        cJSON *entry = cJSON_GetArrayItem(history, i);
        cJSON *ct = cJSON_GetObjectItem(entry, "content");
        if (cJSON_IsArray(ct)) {
            cJSON *block;
            cJSON_ArrayForEach(block, ct) {
                const char *tp = cJSON_GetStringValue(
                    cJSON_GetObjectItem(block, "type"));
                if (tp && strcmp(tp, "tool_use") == 0) {
                    const char *id = cJSON_GetStringValue(
                        cJSON_GetObjectItem(block, "id"));
                    if (id) return id;
                }
            }
        }
    }
    return "toolu_0";
}

void history_add_tool_response(cJSON *history, const char *tool_name,
                               const char *content)
{
    switch (g_config.provider) {
    case PROVIDER_GEMINI: {
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
        cJSON_AddItemToArray(history, entry);
        break;
    }
    case PROVIDER_OPENAI:
    case PROVIDER_OPENROUTER: {
        const char *tcid = extract_openai_tool_id(history);
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "role", "tool");
        cJSON_AddStringToObject(entry, "tool_call_id", tcid);
        cJSON_AddStringToObject(entry, "content", content);
        cJSON_AddItemToArray(history, entry);
        break;
    }
    case PROVIDER_ANTHROPIC: {
        const char *tuid = extract_anthropic_tool_id(history);
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "role", "user");
        cJSON *blocks = cJSON_CreateArray();
        cJSON *block = cJSON_CreateObject();
        cJSON_AddStringToObject(block, "type", "tool_result");
        cJSON_AddStringToObject(block, "tool_use_id", tuid);
        cJSON_AddStringToObject(block, "content", content);
        cJSON_AddItemToArray(blocks, block);
        cJSON_AddItemToObject(entry, "content", blocks);
        cJSON_AddItemToArray(history, entry);
        break;
    }
    }
}

/* ── Provider API calls ──────────────────────────────────────────────────── */

static cJSON *gemini_generate_content(cJSON *history)
{
    cJSON *body = cJSON_CreateObject();

    /* System instruction */
    cJSON *sys = cJSON_CreateObject();
    cJSON *sp = cJSON_CreateArray();
    cJSON *spt = cJSON_CreateObject();
    cJSON_AddStringToObject(spt, "text", get_system_prompt());
    cJSON_AddItemToArray(sp, spt);
    cJSON_AddItemToObject(sys, "parts", sp);
    cJSON_AddItemToObject(body, "systemInstruction", sys);

    cJSON_AddItemToObject(body, "contents", cJSON_Duplicate(history, 1));
    cJSON_AddItemToObject(body, "tools", build_gemini_tools());

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    char url[1024];
    snprintf(url, sizeof(url),
        "https://generativelanguage.googleapis.com/v1beta/models/"
        "%s:generateContent", g_config.model);

    char key_hdr[600];
    snprintf(key_hdr, sizeof(key_hdr), "x-goog-api-key: %s", g_config.api_key);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, key_hdr);

    cJSON *result = llm_http_post(url, hdrs, body_str);
    curl_slist_free_all(hdrs);
    free(body_str);
    return result;
}

static cJSON *openai_generate_content(cJSON *history)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", g_config.model);

    cJSON *messages = cJSON_CreateArray();
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", get_system_prompt());
    cJSON_AddItemToArray(messages, sys_msg);

    cJSON *h;
    cJSON_ArrayForEach(h, history) {
        cJSON_AddItemToArray(messages, cJSON_Duplicate(h, 1));
    }
    cJSON_AddItemToObject(body, "messages", messages);
    cJSON_AddItemToObject(body, "tools", build_openai_tools());

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    char auth[600];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", g_config.api_key);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, auth);

    cJSON *result = llm_http_post(
        "https://api.openai.com/v1/chat/completions", hdrs, body_str);
    curl_slist_free_all(hdrs);
    free(body_str);
    return result;
}

static cJSON *anthropic_generate_content(cJSON *history)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", g_config.model);
    cJSON_AddNumberToObject(body, "max_tokens", 8192);
    cJSON_AddStringToObject(body, "system", get_system_prompt());

    cJSON *messages = cJSON_CreateArray();
    cJSON *h;
    cJSON_ArrayForEach(h, history) {
        cJSON_AddItemToArray(messages, cJSON_Duplicate(h, 1));
    }
    cJSON_AddItemToObject(body, "messages", messages);
    cJSON_AddItemToObject(body, "tools", build_anthropic_tools());

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    char key_hdr[600];
    snprintf(key_hdr, sizeof(key_hdr), "x-api-key: %s", g_config.api_key);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, key_hdr);
    hdrs = curl_slist_append(hdrs, "anthropic-version: 2023-06-01");

    cJSON *result = llm_http_post(
        "https://api.anthropic.com/v1/messages", hdrs, body_str);
    curl_slist_free_all(hdrs);
    free(body_str);
    return result;
}

static cJSON *openrouter_generate_content(cJSON *history)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", g_config.model);

    cJSON *messages = cJSON_CreateArray();
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", get_system_prompt());
    cJSON_AddItemToArray(messages, sys_msg);

    cJSON *h;
    cJSON_ArrayForEach(h, history) {
        cJSON_AddItemToArray(messages, cJSON_Duplicate(h, 1));
    }
    cJSON_AddItemToObject(body, "messages", messages);
    cJSON_AddItemToObject(body, "tools", build_openai_tools());

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    char auth[600];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", g_config.api_key);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, auth);
    hdrs = curl_slist_append(hdrs,
        "HTTP-Referer: https://github.com/0Mattias/DNS-CLAW");
    hdrs = curl_slist_append(hdrs, "X-Title: DNS-CLAW");

    cJSON *result = llm_http_post(
        "https://openrouter.ai/api/v1/chat/completions", hdrs, body_str);
    curl_slist_free_all(hdrs);
    free(body_str);
    return result;
}

/* ── Unified dispatcher ──────────────────────────────────────────────────── */

cJSON *llm_generate_content(cJSON *history)
{
    switch (g_config.provider) {
    case PROVIDER_GEMINI:     return gemini_generate_content(history);
    case PROVIDER_OPENAI:     return openai_generate_content(history);
    case PROVIDER_ANTHROPIC:  return anthropic_generate_content(history);
    case PROVIDER_OPENROUTER: return openrouter_generate_content(history);
    }
    return NULL;
}

/* ── Response parsing (provider → canonical result_json) ─────────────────── */

int llm_parse_response(cJSON *api_resp, cJSON **out_result, cJSON **out_history)
{
    *out_result = NULL;
    *out_history = NULL;
    if (!api_resp) return -1;

    cJSON *result = cJSON_CreateObject();

    switch (g_config.provider) {
    case PROVIDER_GEMINI: {
        cJSON *candidates = cJSON_GetObjectItem(api_resp, "candidates");
        cJSON *cand0 = candidates ? cJSON_GetArrayItem(candidates, 0) : NULL;
        cJSON *content = cand0 ? cJSON_GetObjectItem(cand0, "content") : NULL;
        cJSON *parts = content ? cJSON_GetObjectItem(content, "parts") : NULL;

        int is_tool = 0;
        if (parts) {
            cJSON *cp;
            cJSON_ArrayForEach(cp, parts) {
                cJSON *fc = cJSON_GetObjectItem(cp, "functionCall");
                if (fc) {
                    is_tool = 1;
                    cJSON_AddStringToObject(result, "type", "tool_call");
                    const char *fn = cJSON_GetStringValue(
                        cJSON_GetObjectItem(fc, "name"));
                    if (fn) cJSON_AddStringToObject(result, "tool_name", fn);
                    cJSON *args = cJSON_GetObjectItem(fc, "args");
                    if (args)
                        cJSON_AddItemToObject(result, "tool_args",
                                              cJSON_Duplicate(args, 1));
                    break;
                }
            }
        }
        if (!is_tool) {
            cJSON_AddStringToObject(result, "type", "text");
            #define TEXT_BUF_SIZE 65536
            char *text_buf = calloc(TEXT_BUF_SIZE, 1);
            if (text_buf && parts) {
                size_t cur = 0;
                cJSON *cp;
                cJSON_ArrayForEach(cp, parts) {
                    const char *t = cJSON_GetStringValue(
                        cJSON_GetObjectItem(cp, "text"));
                    if (t) {
                        int w = snprintf(text_buf + cur, TEXT_BUF_SIZE - cur,
                                         "%s", t);
                        if (w > 0) cur += (size_t)w;
                        if (cur >= TEXT_BUF_SIZE - 1) break;
                    }
                }
            }
            cJSON_AddStringToObject(result, "content",
                                    text_buf ? text_buf : "");
            free(text_buf);
            #undef TEXT_BUF_SIZE
        }
        /* Preserve full content for Gemini history (thoughtSignature etc.) */
        if (content) *out_history = cJSON_Duplicate(content, 1);
        break;
    }

    case PROVIDER_OPENAI:
    case PROVIDER_OPENROUTER: {
        cJSON *choices = cJSON_GetObjectItem(api_resp, "choices");
        cJSON *c0 = choices ? cJSON_GetArrayItem(choices, 0) : NULL;
        cJSON *msg = c0 ? cJSON_GetObjectItem(c0, "message") : NULL;

        int is_tool = 0;
        if (msg) {
            cJSON *tool_calls = cJSON_GetObjectItem(msg, "tool_calls");
            if (tool_calls && cJSON_GetArraySize(tool_calls) > 0) {
                cJSON *tc0 = cJSON_GetArrayItem(tool_calls, 0);
                cJSON *fn_obj = cJSON_GetObjectItem(tc0, "function");
                if (fn_obj) {
                    is_tool = 1;
                    cJSON_AddStringToObject(result, "type", "tool_call");
                    const char *fn = cJSON_GetStringValue(
                        cJSON_GetObjectItem(fn_obj, "name"));
                    if (fn) cJSON_AddStringToObject(result, "tool_name", fn);
                    const char *args_str = cJSON_GetStringValue(
                        cJSON_GetObjectItem(fn_obj, "arguments"));
                    if (args_str) {
                        cJSON *args = cJSON_Parse(args_str);
                        if (args)
                            cJSON_AddItemToObject(result, "tool_args", args);
                    }
                }
            }
        }
        if (!is_tool && msg) {
            cJSON_AddStringToObject(result, "type", "text");
            const char *ct = cJSON_GetStringValue(
                cJSON_GetObjectItem(msg, "content"));
            cJSON_AddStringToObject(result, "content", ct ? ct : "");
        }
        if (msg) *out_history = cJSON_Duplicate(msg, 1);
        break;
    }

    case PROVIDER_ANTHROPIC: {
        cJSON *content_arr = cJSON_GetObjectItem(api_resp, "content");
        int is_tool = 0;
        if (content_arr) {
            cJSON *block;
            cJSON_ArrayForEach(block, content_arr) {
                const char *tp = cJSON_GetStringValue(
                    cJSON_GetObjectItem(block, "type"));
                if (tp && strcmp(tp, "tool_use") == 0) {
                    is_tool = 1;
                    cJSON_AddStringToObject(result, "type", "tool_call");
                    const char *fn = cJSON_GetStringValue(
                        cJSON_GetObjectItem(block, "name"));
                    if (fn) cJSON_AddStringToObject(result, "tool_name", fn);
                    cJSON *input = cJSON_GetObjectItem(block, "input");
                    if (input)
                        cJSON_AddItemToObject(result, "tool_args",
                                              cJSON_Duplicate(input, 1));
                    break;
                }
            }
        }
        if (!is_tool) {
            cJSON_AddStringToObject(result, "type", "text");
            char *text_buf = calloc(65536, 1);
            size_t cur = 0;
            if (text_buf && content_arr) {
                cJSON *block;
                cJSON_ArrayForEach(block, content_arr) {
                    const char *tp = cJSON_GetStringValue(
                        cJSON_GetObjectItem(block, "type"));
                    if (tp && strcmp(tp, "text") == 0) {
                        const char *t = cJSON_GetStringValue(
                            cJSON_GetObjectItem(block, "text"));
                        if (t) {
                            int w = snprintf(text_buf + cur, 65536 - cur,
                                             "%s", t);
                            if (w > 0) cur += (size_t)w;
                        }
                    }
                }
            }
            cJSON_AddStringToObject(result, "content",
                                    text_buf ? text_buf : "");
            free(text_buf);
        }
        /* Anthropic history: {role:"assistant", content:[...]} */
        {
            cJSON *he = cJSON_CreateObject();
            cJSON_AddStringToObject(he, "role", "assistant");
            if (content_arr)
                cJSON_AddItemToObject(he, "content",
                                      cJSON_Duplicate(content_arr, 1));
            *out_history = he;
        }
        break;
    }
    }

    cJSON_AddStringToObject(result, "provider", g_config.provider_name);
    *out_result = result;
    return 0;
}

/* ── High-level request processor ────────────────────────────────────────── */

cJSON *llm_process_request(session_t *sess, const char *type,
                           const char *content, const char *tool_name)
{
    /* 1. Add user/tool entry to history */
    pthread_mutex_lock(&g_lock);
    if (strcmp(type, "user") == 0) {
        history_add_user_msg(sess->history, content);
    } else if (strcmp(type, "tool_response") == 0 && tool_name) {
        history_add_tool_response(sess->history, tool_name, content);
    }
    cJSON *hist_copy = cJSON_Duplicate(sess->history, 1);
    pthread_mutex_unlock(&g_lock);

    /* 2. Call provider API */
    cJSON *api_resp = llm_generate_content(hist_copy);
    cJSON_Delete(hist_copy);

    if (!api_resp) {
        log_err("llm", "%s API call failed", g_config.provider_name);
        return NULL;
    }

    /* 3. Parse response */
    cJSON *result_json = NULL;
    cJSON *history_entry = NULL;
    if (llm_parse_response(api_resp, &result_json, &history_entry) < 0) {
        cJSON_Delete(api_resp);
        return NULL;
    }
    cJSON_Delete(api_resp);

    /* 4. Append model response to history */
    if (history_entry) {
        pthread_mutex_lock(&g_lock);
        cJSON_AddItemToArray(sess->history, history_entry);
        pthread_mutex_unlock(&g_lock);
    }

    return result_json;
}

/* ── LLM Processing Thread ───────────────────────────────────────────────── */

void *process_llm_thread(void *arg)
{
    llm_task_t *task = (llm_task_t *)arg;
    session_t *sess = task->sess;
    int msg_id = task->msg_id;
    free(task);

    /* sess->busy was already incremented by handler.c before thread spawn,
     * so the reaper can't destroy this session while we're running. */

    /* 1. Reassemble uploaded chunks */
    pthread_mutex_lock(&g_lock);
    pending_prompt_t *pp = &sess->pending[msg_id];

    int max_seq = -1;
    for (int i = 0; i < pp->chunk_count; i++) {
        if (pp->chunks[i].seq > max_seq) max_seq = pp->chunks[i].seq;
    }

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
    if (decoded_len < 0 || (size_t)decoded_len >= sizeof(payload_bytes)) {
        fprintf(stderr, "[llm] base32 decode failed (input len=%zu, decoded=%d)\n",
                b32_pos, decoded_len);
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

    /* Parse JSON payload */
    cJSON *payload = cJSON_Parse((char *)payload_bytes);
    if (!payload) {
        fprintf(stderr, "[llm] JSON parse failed (len=%d): %.128s\n",
                decoded_len, (char *)payload_bytes);
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

    /* 2. Process through provider-dispatched LLM layer */
    cJSON *result_json = llm_process_request(sess, type, content, tool_name);

    /* Free payload AFTER llm_process_request — type/content/tool_name point
     * into the cJSON tree's memory and become dangling after cJSON_Delete. */
    cJSON_Delete(payload);
    if (!result_json) {
        pthread_mutex_lock(&g_lock);
        sess->responses[msg_id].failed = 1;
        pthread_mutex_unlock(&g_lock);
        goto done;
    }

    /* 3. Encode response → [encrypt →] base64 → chunk */
    char *result_str = cJSON_PrintUnformatted(result_json);
    cJSON_Delete(result_json);

    size_t result_len = strlen(result_str);

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
    if (!b64_buf) {
        log_err("llm", "Out of memory for base64 buffer");
        free(encrypted);
        free(result_str);
        pthread_mutex_lock(&g_lock);
        sess->responses[msg_id].failed = 1;
        pthread_mutex_unlock(&g_lock);
        goto done;
    }
    base64_encode(encode_data, encode_len, b64_buf, b64_len);
    free(encrypted);
    free(result_str);

    size_t b64_total = strlen(b64_buf);
    int nchunks = 0;

    pthread_mutex_lock(&g_lock);
    msg_response_t *mr = &sess->responses[msg_id];

    int alloc_failed = 0;
    for (size_t i = 0; i < b64_total && nchunks < MAX_RESP_CHUNKS;
         i += CHUNK_SIZE) {
        size_t clen = b64_total - i;
        if (clen > CHUNK_SIZE) clen = CHUNK_SIZE;
        mr->chunks[nchunks] = malloc(clen + 1);
        if (!mr->chunks[nchunks]) {
            log_err("llm", "Out of memory allocating response chunk");
            alloc_failed = 1;
            break;
        }
        memcpy(mr->chunks[nchunks], b64_buf + i, clen);
        mr->chunks[nchunks][clen] = '\0';
        nchunks++;
    }
    mr->chunk_count = nchunks;
    if (alloc_failed) {
        for (int c = 0; c < nchunks; c++) {
            free(mr->chunks[c]);
            mr->chunks[c] = NULL;
        }
        mr->chunk_count = 0;
        mr->failed = 1;
    } else {
        mr->ready = 1;
    }
    pthread_mutex_unlock(&g_lock);

    free(b64_buf);

    fprintf(stderr, "[llm] Response ready sid=%s mid=%d chunks=%d\n",
            sess->id, msg_id, nchunks);

done:
    pthread_mutex_lock(&g_lock);
    sess->busy--;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}
