/*
 * server/llm.h — Multi-provider LLM interaction
 *
 * Supports Gemini, OpenAI, Anthropic, and OpenRouter.
 */
#ifndef CLAW_SERVER_LLM_H
#define CLAW_SERVER_LLM_H

#include <cJSON.h>
#include "server/session.h"

/* ── Provider enum ───────────────────────────────────────────────────────── */

typedef enum {
    PROVIDER_GEMINI,
    PROVIDER_OPENAI,
    PROVIDER_ANTHROPIC,
    PROVIDER_OPENROUTER
} llm_provider_t;

#define NUM_PROVIDERS 4

extern const char *PROVIDER_NAMES[];
extern const char *PROVIDER_DEFAULT_MODELS[];

/* ── Tool definition table ───────────────────────────────────────────────── */

typedef struct {
    const char *pname;
    const char *pdesc;
} tool_param_t;

typedef struct {
    const char *name;
    const char *description;
    tool_param_t params[4];
    int nparam;
    const char *required[4];
    int nrequired;
} tool_def_t;

extern const tool_def_t TOOL_DEFS[];
#define NUM_TOOLS 7

/* ── LLM thread task ─────────────────────────────────────────────────────── */

typedef struct {
    session_t *sess;
    int msg_id;
} llm_task_t;

/*
 * Background thread: reassemble upload, decrypt, call LLM, encrypt, chunk response.
 */
void *process_llm_thread(void *arg);

/* ── History helpers (provider-dispatched) ────────────────────────────────── */

void history_add_user_msg(cJSON *history, const char *content);
void history_add_tool_response(cJSON *history, const char *tool_name, const char *content);

/* ── Provider API calls ──────────────────────────────────────────────────── */

cJSON *llm_generate_content(cJSON *history);

/*
 * Parse provider API response into canonical result_json:
 *   {type:"text", content:"...", provider:"..."} or
 *   {type:"tool_call", tool_name, tool_args, provider:"..."}
 * Also produces a history_entry to append to session history.
 * Caller must cJSON_Delete both *out_result and *out_history.
 */
int llm_parse_response(cJSON *api_resp, cJSON **out_result, cJSON **out_history);

/*
 * High-level: add entry to history, call API, parse, append model response.
 * Returns canonical result_json or NULL on failure.
 */
cJSON *llm_process_request(session_t *sess, const char *type, const char *content,
                           const char *tool_name);

#endif /* CLAW_SERVER_LLM_H */
