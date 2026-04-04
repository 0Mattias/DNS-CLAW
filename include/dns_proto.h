/*
 * dns_proto.h — Minimal DNS wire-format builder/parser (RFC 1035).
 *
 * Only supports building TXT queries and parsing TXT answers,
 * which is all the tunneling protocol needs.
 */
#ifndef DNS_PROTO_H
#define DNS_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define DNS_TYPE_TXT 16
#define DNS_CLASS_IN 1
#define DNS_MAX_MSG  4096
#define DNS_MAX_TXT  2048

#define DNS_RCODE_OK       0
#define DNS_RCODE_FORMERR  1
#define DNS_RCODE_SERVFAIL 2
#define DNS_RCODE_NXDOMAIN 3
#define DNS_RCODE_REFUSED  5

/* ── Wire-format builder/parser ──────────────────────────────────────────── */

/*
 * Build a DNS query for `qname` (e.g. "init.llm.local.") with type TXT.
 * Writes the wire-format message into `buf`.
 * Returns the length written, or -1 on error.
 * `id` is the 16-bit query ID (caller should randomize).
 */
int dns_build_query(uint16_t id, const char *qname, uint8_t *buf, size_t buflen);

/*
 * Build a DNS response to a query.  Sets the given rcode.
 * If `txt` is non-NULL, adds a TXT answer record for `qname`.
 * Returns message length, or -1 on error.
 */
int dns_build_response(uint16_t id, const char *qname, int rcode, const char *txt, uint8_t *buf,
                       size_t buflen);

/*
 * Parse a DNS response message and extract the first TXT record data.
 * `txt_out` will be NUL-terminated.
 * Returns 0 on success, negative on error.
 * Sets `*rcode` to the response RCODE.
 */
int dns_parse_txt_response(const uint8_t *msg, size_t msglen, int *rcode, char *txt_out,
                           size_t txt_out_len);

/*
 * Parse a DNS query message and extract the QNAME.
 * `qname_out` will be NUL-terminated (e.g. "init.llm.local.").
 * Returns the query ID, or -1 on error.
 */
int dns_parse_query(const uint8_t *msg, size_t msglen, uint16_t *id_out, char *qname_out,
                    size_t qname_out_len);

/* ── High-level query transports ─────────────────────────────────────────── */

/*
 * Perform a DNS TXT query over plain UDP.
 * Returns the TXT content in `txt_out` (NUL-terminated), 0 on success.
 */
int dns_query_udp(const char *server_ip, uint16_t port, const char *qname, char *txt_out,
                  size_t txt_out_len);

/*
 * Perform a DNS TXT query over TLS (DNS-over-TLS, port 853).
 * If `insecure` is nonzero, certificate verification is skipped.
 */
int dns_query_dot(const char *server_ip, uint16_t port, const char *qname, int insecure,
                  char *txt_out, size_t txt_out_len);

/*
 * Perform a DNS TXT query over HTTPS (DNS-over-HTTPS, RFC 8484).
 * `url` is e.g. "https://127.0.0.1/dns-query".
 * If `insecure` is nonzero, certificate verification is skipped.
 */
int dns_query_doh(const char *url, const char *qname, int insecure, char *txt_out,
                  size_t txt_out_len);

#endif /* DNS_PROTO_H */
