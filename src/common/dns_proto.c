/*
 * dns_proto.c — DNS wire-format builder/parser + transport (UDP, DoT, DoH).
 *
 * Implements only what the tunneling protocol needs: TXT queries/answers.
 * All multi-byte integers are big-endian on the wire (network byte order).
 */
#include "dns_proto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <curl/curl.h>

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/*
 * Encode a dotted name ("init.llm.local.") into DNS wire format.
 * Returns bytes written, or -1 on error.
 */
static int encode_qname(const char *name, uint8_t *buf, size_t buflen)
{
    size_t pos = 0;
    const char *p = name;

    while (*p) {
        const char *dot = strchr(p, '.');
        size_t label_len;
        if (dot)
            label_len = (size_t)(dot - p);
        else
            label_len = strlen(p);

        if (label_len == 0) {
            /* trailing dot — end of name */
            if (pos >= buflen) return -1;
            buf[pos++] = 0;
            return (int)pos;
        }

        if (label_len > 63 || pos + 1 + label_len >= buflen) return -1;
        buf[pos++] = (uint8_t)label_len;
        memcpy(buf + pos, p, label_len);
        pos += label_len;

        p += label_len;
        if (*p == '.') p++;
    }

    /* Null terminator label */
    if (pos >= buflen) return -1;
    buf[pos++] = 0;
    return (int)pos;
}

/*
 * Decode a DNS wire-format name starting at `msg[off]`.
 * Handles pointer compression. Writes dotted name to `out`.
 * Returns the number of bytes consumed from the original position
 * (not following pointers), or -1 on error.
 */
static int decode_name(const uint8_t *msg, size_t msglen, size_t off,
                       char *out, size_t out_len)
{
    size_t pos = off;
    size_t out_pos = 0;
    int consumed = -1;
    int jumps = 0;

    while (pos < msglen) {
        if (jumps > 10) return -1; /* loop guard */

        uint8_t len = msg[pos];
        if (len == 0) {
            if (consumed < 0) consumed = (int)(pos - off + 1);
            break;
        }

        if ((len & 0xC0) == 0xC0) {
            /* Pointer */
            if (pos + 1 >= msglen) return -1;
            if (consumed < 0) consumed = (int)(pos - off + 2);
            uint16_t ptr = ((uint16_t)(len & 0x3F) << 8) | msg[pos + 1];
            pos = ptr;
            jumps++;
            continue;
        }

        pos++;
        if (pos + len > msglen) return -1;
        if (out_pos + len + 1 >= out_len) return -1;

        memcpy(out + out_pos, msg + pos, len);
        out_pos += len;
        out[out_pos++] = '.';
        pos += len;
    }

    out[out_pos] = '\0';
    return consumed;
}

/* ── Build DNS query ─────────────────────────────────────────────────────── */

int dns_build_query(uint16_t id, const char *qname,
                    uint8_t *buf, size_t buflen)
{
    if (buflen < 12) return -1;

    memset(buf, 0, 12);
    put16(buf + 0, id);           /* ID */
    put16(buf + 2, 0x0100);       /* Flags: RD=1 (standard recursive query) */
    put16(buf + 4, 1);            /* QDCOUNT = 1 */
    /* ANCOUNT, NSCOUNT, ARCOUNT = 0 */

    int qlen = encode_qname(qname, buf + 12, buflen - 12);
    if (qlen < 0) return -1;

    size_t pos = 12 + (size_t)qlen;
    if (pos + 4 > buflen) return -1;

    put16(buf + pos, DNS_TYPE_TXT);  pos += 2;
    put16(buf + pos, DNS_CLASS_IN);  pos += 2;

    return (int)pos;
}

/* ── Build DNS response ──────────────────────────────────────────────────── */

int dns_build_response(uint16_t id, const char *qname,
                       int rcode, const char *txt,
                       uint8_t *buf, size_t buflen)
{
    if (buflen < 12) return -1;

    memset(buf, 0, 12);
    put16(buf + 0, id);
    uint16_t flags = 0x8000 | 0x0100; /* QR=1, RD=1 */
    flags |= (uint16_t)(rcode & 0x0F);
    put16(buf + 2, flags);
    put16(buf + 4, 1);    /* QDCOUNT */
    put16(buf + 6, txt ? 1 : 0); /* ANCOUNT */

    int qlen = encode_qname(qname, buf + 12, buflen - 12);
    if (qlen < 0) return -1;
    size_t pos = 12 + (size_t)qlen;

    /* Question section */
    if (pos + 4 > buflen) return -1;
    put16(buf + pos, DNS_TYPE_TXT);  pos += 2;
    put16(buf + pos, DNS_CLASS_IN);  pos += 2;

    /* Answer section (TXT record) */
    if (txt) {
        /* Name pointer to question name (offset 12) */
        if (pos + 2 > buflen) return -1;
        put16(buf + pos, 0xC00C); pos += 2;

        /* TYPE, CLASS, TTL */
        if (pos + 8 > buflen) return -1;
        put16(buf + pos, DNS_TYPE_TXT);  pos += 2;
        put16(buf + pos, DNS_CLASS_IN);  pos += 2;
        put16(buf + pos, 0); pos += 2; /* TTL high */
        put16(buf + pos, 0); pos += 2; /* TTL low  */

        /* RDLENGTH + RDATA: TXT records are length-prefixed strings.
         * We may need multiple 255-byte chunks for long TXT data. */
        size_t txt_len = strlen(txt);
        size_t rdlen_pos = pos;
        pos += 2; /* placeholder for RDLENGTH */

        size_t rdata_start = pos;
        size_t ti = 0;
        while (ti < txt_len) {
            size_t chunk = txt_len - ti;
            if (chunk > 255) chunk = 255;
            if (pos + 1 + chunk > buflen) return -1;
            buf[pos++] = (uint8_t)chunk;
            memcpy(buf + pos, txt + ti, chunk);
            pos += chunk;
            ti += chunk;
        }
        if (txt_len == 0) {
            /* Empty TXT: single zero-length string */
            if (pos + 1 > buflen) return -1;
            buf[pos++] = 0;
        }

        put16(buf + rdlen_pos, (uint16_t)(pos - rdata_start));
    }

    return (int)pos;
}

/* ── Parse DNS response (extract first TXT) ──────────────────────────────── */

int dns_parse_txt_response(const uint8_t *msg, size_t msglen,
                           int *rcode,
                           char *txt_out, size_t txt_out_len)
{
    if (msglen < 12) return -1;

    *rcode = msg[3] & 0x0F;
    uint16_t qdcount = get16(msg + 4);
    uint16_t ancount = get16(msg + 6);

    /* Skip question section */
    size_t pos = 12;
    for (uint16_t i = 0; i < qdcount; i++) {
        /* Skip QNAME */
        while (pos < msglen) {
            if (msg[pos] == 0) { pos++; break; }
            if ((msg[pos] & 0xC0) == 0xC0) {
                if (pos + 1 >= msglen) return -1;
                pos += 2; break;
            }
            size_t label_len = msg[pos];
            if (pos + 1 + label_len > msglen) return -1;
            pos += 1 + label_len;
        }
        if (pos + 4 > msglen) return -1;
        pos += 4; /* QTYPE + QCLASS */
    }

    /* Parse answer section */
    txt_out[0] = '\0';
    size_t txt_pos = 0;

    for (uint16_t i = 0; i < ancount; i++) {
        if (pos >= msglen) return -1;

        /* Skip NAME */
        char dummy[256];
        int nc = decode_name(msg, msglen, pos, dummy, sizeof(dummy));
        if (nc < 0) return -1;
        pos += (size_t)nc;

        if (pos + 10 > msglen) return -1;
        uint16_t rtype = get16(msg + pos);    pos += 2;
        pos += 2; /* RCLASS */
        pos += 4; /* TTL */
        uint16_t rdlen = get16(msg + pos);    pos += 2;

        if (rtype == DNS_TYPE_TXT && i == 0) {
            /* TXT RDATA: sequence of length-prefixed strings */
            size_t rdata_end = pos + rdlen;
            if (rdata_end > msglen) return -1;  /* malformed: RDATA exceeds packet */
            while (pos < rdata_end) {
                uint8_t slen = msg[pos++];
                if (pos + slen > msglen || pos + slen > rdata_end) return -1;
                if (txt_pos + slen >= txt_out_len) return -1;
                memcpy(txt_out + txt_pos, msg + pos, slen);
                txt_pos += slen;
                pos += slen;
            }
            txt_out[txt_pos] = '\0';
        } else {
            pos += rdlen; /* skip RDATA */
        }
    }

    return 0;
}

/* ── Parse DNS query ─────────────────────────────────────────────────────── */

int dns_parse_query(const uint8_t *msg, size_t msglen,
                    uint16_t *id_out,
                    char *qname_out, size_t qname_out_len)
{
    if (msglen < 12) return -1;

    *id_out = get16(msg + 0);

    int nc = decode_name(msg, msglen, 12, qname_out, qname_out_len);
    if (nc < 0) return -1;

    return 0;
}

/* ── Transport: UDP (with socket reuse) ──────────────────────────────────── */

static int         g_udp_fd = -1;
static uint16_t    g_udp_port = 0;
static uint32_t    g_udp_addr = 0;

static int get_udp_socket(const char *server_ip, uint16_t port)
{
    struct in_addr ia;
    if (inet_pton(AF_INET, server_ip, &ia) != 1) return -1;

    /* Reuse if same server:port and socket still valid */
    if (g_udp_fd >= 0 && g_udp_port == port && g_udp_addr == ia.s_addr)
        return g_udp_fd;

    /* Close old socket if server changed */
    if (g_udp_fd >= 0) { close(g_udp_fd); g_udp_fd = -1; }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    struct timeval tv = { .tv_sec = 5 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Connect the UDP socket so we can use send/recv instead of sendto/recvfrom.
     * This also filters out packets from other sources. */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = ia;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    g_udp_fd = fd;
    g_udp_port = port;
    g_udp_addr = ia.s_addr;
    return fd;
}

int dns_query_udp(const char *server_ip, uint16_t port,
                  const char *qname,
                  char *txt_out, size_t txt_out_len)
{
    uint8_t qbuf[DNS_MAX_MSG];
    uint16_t qid = (uint16_t)(arc4random() & 0xFFFF);
    int qlen = dns_build_query(qid, qname, qbuf, sizeof(qbuf));
    if (qlen < 0) return -1;

    int fd = get_udp_socket(server_ip, port);
    if (fd < 0) return -1;

    ssize_t sent = send(fd, qbuf, (size_t)qlen, 0);
    if (sent < 0) {
        /* Socket went bad — invalidate and retry once */
        close(g_udp_fd);
        g_udp_fd = -1;
        fd = get_udp_socket(server_ip, port);
        if (fd < 0) return -1;
        sent = send(fd, qbuf, (size_t)qlen, 0);
        if (sent < 0) return -1;
    }

    uint8_t rbuf[DNS_MAX_MSG];
    ssize_t rlen = recv(fd, rbuf, sizeof(rbuf), 0);
    if (rlen < 0) {
        /* Timeout or error — invalidate socket for next call */
        close(g_udp_fd);
        g_udp_fd = -1;
        return -1;
    }

    int rcode;
    return dns_parse_txt_response(rbuf, (size_t)rlen, &rcode,
                                  txt_out, txt_out_len);
}

/* ── Transport: DoT (DNS over TLS) ───────────────────────────────────────── */

int dns_query_dot(const char *server_ip, uint16_t port,
                  const char *qname, int insecure,
                  char *txt_out, size_t txt_out_len)
{
    uint8_t qbuf[DNS_MAX_MSG];
    uint16_t qid = (uint16_t)(arc4random() & 0xFFFF);
    int qlen = dns_build_query(qid, qname, qbuf, sizeof(qbuf));
    if (qlen < 0) return -1;

    /* TCP connection */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    /* TLS handshake */
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { close(fd); return -1; }

    if (insecure)
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) <= 0) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        return -1;
    }

    /* DNS over TCP: 2-byte length prefix + message */
    uint8_t lenbuf[2];
    put16(lenbuf, (uint16_t)qlen);
    if (SSL_write(ssl, lenbuf, 2) != 2 ||
        SSL_write(ssl, qbuf, qlen) != qlen) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        return -1;
    }

    /* Read response: 2-byte length prefix */
    uint8_t rlenbuf[2];
    if (SSL_read(ssl, rlenbuf, 2) != 2) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        return -1;
    }
    uint16_t rlen = get16(rlenbuf);
    if (rlen > DNS_MAX_MSG) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        return -1;
    }

    uint8_t rbuf[DNS_MAX_MSG];
    int total = 0;
    while (total < rlen) {
        int n = SSL_read(ssl, rbuf + total, rlen - total);
        if (n <= 0) break;
        total += n;
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);

    if (total < rlen) return -1;

    int rcode;
    return dns_parse_txt_response(rbuf, (size_t)rlen, &rcode,
                                  txt_out, txt_out_len);
}

/* ── Transport: DoH (DNS over HTTPS) ─────────────────────────────────────── */

struct doh_buf {
    uint8_t *data;
    size_t   len;
    size_t   cap;
};

static size_t doh_write_cb(void *contents, size_t size, size_t nmemb,
                           void *userp)
{
    size_t total = size * nmemb;
    struct doh_buf *buf = (struct doh_buf *)userp;
    if (buf->len + total > buf->cap) return 0;
    memcpy(buf->data + buf->len, contents, total);
    buf->len += total;
    return total;
}

int dns_query_doh(const char *url, const char *qname, int insecure,
                  char *txt_out, size_t txt_out_len)
{
    uint8_t qbuf[DNS_MAX_MSG];
    uint16_t qid = (uint16_t)(arc4random() & 0xFFFF);
    int qlen = dns_build_query(qid, qname, qbuf, sizeof(qbuf));
    if (qlen < 0) return -1;

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    uint8_t resp_data[DNS_MAX_MSG];
    struct doh_buf resp = { .data = resp_data, .len = 0, .cap = sizeof(resp_data) };

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/dns-message");
    headers = curl_slist_append(headers, "Accept: application/dns-message");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, qbuf);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)qlen);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, doh_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    if (insecure) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) return -1;

    int rcode;
    return dns_parse_txt_response(resp_data, resp.len, &rcode,
                                  txt_out, txt_out_len);
}
