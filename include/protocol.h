/*
 * protocol.h — DNS CLAW Protocol Constants
 *
 * Shared definitions for the DNS tunneling protocol between client and server.
 */
#ifndef CLAW_PROTOCOL_H
#define CLAW_PROTOCOL_H

#define CLAW_PROTO_VERSION  2

/* Message types (JSON "type" field) */
#define CLAW_MSG_USER           "user"
#define CLAW_MSG_TOOL_RESPONSE  "tool_response"
#define CLAW_MSG_TEXT           "text"
#define CLAW_MSG_TOOL_CALL      "tool_call"

/* Session & chunk limits (must match on both sides) */
#define MAX_SESSIONS     64
#define MAX_CHUNKS       256
#define MAX_RESP_CHUNKS  512
#define CHUNK_SIZE       200   /* base64 chars per TXT response chunk */
#define MAX_MSG_IDS      256   /* max concurrent message IDs per session */
#define UPLOAD_B32_MAX   (MAX_CHUNKS * 256)  /* max reassembled base32 data */

/* Timeouts */
#define SESSION_TIMEOUT_SEC  (30 * 60)  /* 30 minutes */

/* Client upload chunk size */
#define UPLOAD_CHUNK_SZ  35   /* base32(35 bytes) = 56 chars, fits DNS 63-char label */

/* Response buffer */
#define RESP_BUF_SIZE    131072  /* 128KB response buffer */

/* Tool result size limit */
#define TOOL_RESULT_SIZE 8192

#endif /* CLAW_PROTOCOL_H */
