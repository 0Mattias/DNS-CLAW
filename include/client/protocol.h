/*
 * client/protocol.h — DNS query wrapper and session management
 */
#ifndef CLAW_CLIENT_PROTOCOL_H
#define CLAW_CLIENT_PROTOCOL_H

#include <stdatomic.h>
#include <stddef.h>

/* Global session state (defined in protocol.c) */
extern char       g_session_id[64];
extern int        g_msg_id;
extern int        g_turn;
extern atomic_int g_interrupted;

/* DNS query dispatcher (DoH/DoT/UDP) */
int do_dns_query(const char *qname, char *txt_out, size_t txt_out_len);

/* Initialize a new session with the server */
int init_session(int show_msg);

/* Main message processing loop (upload, LLM, download, tools) */
int process_message_loop(const char *type, const char *content,
                         const char *tool_name);

#endif /* CLAW_CLIENT_PROTOCOL_H */
