/*
 * server/llm.h — Gemini LLM interaction
 */
#ifndef CLAW_SERVER_LLM_H
#define CLAW_SERVER_LLM_H

#include <cJSON.h>
#include "server/session.h"

typedef struct {
    session_t *sess;
    int        msg_id;
} llm_task_t;

/*
 * Call Gemini generateContent API.
 * `history` is the JSON array of content objects.
 * Returns the parsed JSON response, or NULL on error.
 * Caller must cJSON_Delete the result.
 */
cJSON *gemini_generate_content(cJSON *history);

/*
 * Background thread: reassemble upload, decrypt, call LLM, encrypt, chunk response.
 */
void *process_llm_thread(void *arg);

#endif /* CLAW_SERVER_LLM_H */
