/*
 * server/handler.h — DNS query handler (protocol dispatcher)
 */
#ifndef CLAW_SERVER_HANDLER_H
#define CLAW_SERVER_HANDLER_H

#include <stddef.h>
#include <stdint.h>

/*
 * Handle a single DNS query. Writes the response into `resp_buf`.
 * Returns the response length, or -1 on error.
 */
int handle_dns_query(const uint8_t *query, size_t query_len,
                     uint8_t *resp_buf, size_t resp_buf_len);

#endif /* CLAW_SERVER_HANDLER_H */
