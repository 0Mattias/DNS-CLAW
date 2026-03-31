/*
 * server/transport.c — UDP / DoT / DoH listeners
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "dns_proto.h"
#include "server/handler.h"
#include "server/transport.h"

/* Portable case-insensitive substring search */
static char *ci_strstr(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;
    size_t nlen = strlen(needle);
    size_t hlen = strlen(haystack);
    for (size_t h = 0; h + nlen <= hlen; h++) {
        size_t i;
        for (i = 0; i < nlen; i++) {
            if (tolower((unsigned char)haystack[h + i]) != tolower((unsigned char)needle[i]))
                break;
        }
        if (i == nlen) return (char *)(haystack + h);
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * UDP Listener
 * ═══════════════════════════════════════════════════════════════════════════ */

void *udp_server_thread(void *arg)
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
    g_server_fd = fd;

    uint8_t buf[DNS_MAX_MSG];
    while (g_running) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&client, &clen);
        if (n <= 0) {
            if (!g_running) break;
            continue;
        }

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
 * DoT (TLS) Listener
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
            if (SSL_write(dc->ssl, rlb, 2) != 2 ||
                SSL_write(dc->ssl, rbuf, rlen) != rlen)
                break;
        }
    }

done:
    SSL_shutdown(dc->ssl);
    SSL_free(dc->ssl);
    close(dc->fd);
    free(dc);
    return NULL;
}

void *dot_server_thread(void *arg)
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
    g_server_fd = fd;

    while (g_running) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int cfd = accept(fd, (struct sockaddr *)&client, &clen);
        if (cfd < 0) {
            if (!g_running) break;
            continue;
        }

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
    SSL_CTX_free(ctx);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DoH (HTTPS) Listener
 * ═══════════════════════════════════════════════════════════════════════════ */

static void *doh_client_thread(void *arg)
{
    dot_client_t *dc = (dot_client_t *)arg;

    char http_buf[65536];
    int total = 0;
    int header_end = -1;

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
        if (strncmp(http_buf, "POST ", 5) != 0) {
            const char *err = "HTTP/1.1 405 Method Not Allowed\r\n"
                              "Content-Length: 0\r\nConnection: close\r\n\r\n";
            SSL_write(dc->ssl, err, (int)strlen(err));
            goto done;
        }
        if (!strstr(http_buf, "/dns-query")) {
            const char *err = "HTTP/1.1 404 Not Found\r\n"
                              "Content-Length: 0\r\nConnection: close\r\n\r\n";
            SSL_write(dc->ssl, err, (int)strlen(err));
            goto done;
        }

        int content_length = 0;
        char *cl = ci_strstr(http_buf, "Content-Length:");
        if (cl) {
            cl += 15;
            while (*cl == ' ') cl++;
            content_length = atoi(cl);
        }

        int body_so_far = total - header_end;
        while (body_so_far < content_length &&
               total < (int)sizeof(http_buf) - 1) {
            int n = SSL_read(dc->ssl, http_buf + total,
                            (int)sizeof(http_buf) - 1 - total);
            if (n <= 0) break;
            total += n;
            body_so_far = total - header_end;
        }

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

void *doh_server_thread(void *arg)
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
    g_server_fd = fd;

    while (g_running) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int cfd = accept(fd, (struct sockaddr *)&client, &clen);
        if (cfd < 0) {
            if (!g_running) break;
            continue;
        }

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
    SSL_CTX_free(ctx);
    return NULL;
}
