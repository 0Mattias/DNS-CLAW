/*
 * test_integration.c — Integration tests for the DNS protocol round-trip.
 *
 * Tests the full server handler pipeline (init → upload → finalize → download)
 * by calling handle_dns_query() directly with crafted DNS packets.
 * No network I/O or live LLM API needed.
 */
#undef NDEBUG
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>

#include "base32.h"
#include "crypto.h"
#include "dns_proto.h"
#include "protocol.h"
#include "server/handler.h"
#include "server/session.h"
#include "server/transport.h"

#define PASS(name) printf("  PASS  %s\n", name)

/* ── Globals normally defined in server main.c / transport.c ─────────────── */

atomic_int g_running = ATOMIC_VAR_INIT(1);
int g_server_fd = -1;
server_config_t g_config;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static int query_and_get_txt(const char *qname, char *txt_out, size_t txt_len)
{
    uint8_t query_buf[DNS_MAX_MSG];
    uint8_t resp_buf[DNS_MAX_MSG];

    int qlen = dns_build_query(0x1234, qname, query_buf, sizeof(query_buf));
    if (qlen < 0)
        return -1;

    int rlen = handle_dns_query(query_buf, (size_t)qlen, resp_buf, sizeof(resp_buf));
    if (rlen < 0)
        return -1;

    int rcode;
    if (dns_parse_txt_response(resp_buf, (size_t)rlen, &rcode, txt_out, txt_len) < 0)
        return -1;

    return rcode;
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

static void test_init_session(void)
{
    char txt[DNS_MAX_TXT];
    int rcode = query_and_get_txt("init.llm.local.", txt, sizeof(txt));

    assert(rcode == DNS_RCODE_OK);
    assert(strlen(txt) > 0);
    assert(strlen(txt) <= 32); /* hex session ID */
    PASS("init_session");
}

static void test_init_returns_unique_ids(void)
{
    char txt1[DNS_MAX_TXT], txt2[DNS_MAX_TXT];

    assert(query_and_get_txt("init.llm.local.", txt1, sizeof(txt1)) == DNS_RCODE_OK);
    assert(query_and_get_txt("init.llm.local.", txt2, sizeof(txt2)) == DNS_RCODE_OK);
    assert(strcmp(txt1, txt2) != 0);
    PASS("init_returns_unique_ids");
}

static void test_upload_and_finalize(void)
{
    /* Create session */
    char sid[DNS_MAX_TXT];
    assert(query_and_get_txt("init.llm.local.", sid, sizeof(sid)) == DNS_RCODE_OK);

    /* Prepare a small payload and base32-encode it */
    const char *payload = "{\"type\":\"user\",\"content\":\"hello\"}";
    char b32[256];
    base32_encode((const uint8_t *)payload, strlen(payload), b32, sizeof(b32));
    /* Lowercase the base32 (DNS labels are case-insensitive) */
    for (char *p = b32; *p; p++)
        *p = (char)tolower((unsigned char)*p);

    /* Upload single chunk: <b32>.0.up.0.<sid>.llm.local. */
    char qname[1024];
    snprintf(qname, sizeof(qname), "%s.0.up.0.%s.llm.local.", b32, sid);

    char txt[DNS_MAX_TXT];
    assert(query_and_get_txt(qname, txt, sizeof(txt)) == DNS_RCODE_OK);
    assert(strcmp(txt, "ACK") == 0);

    /* Finalize: fin.0.<sid>.llm.local. */
    snprintf(qname, sizeof(qname), "fin.0.%s.llm.local.", sid);
    assert(query_and_get_txt(qname, txt, sizeof(txt)) == DNS_RCODE_OK);
    assert(strcmp(txt, "ACK") == 0);

    PASS("upload_and_finalize");
}

static void test_download_after_finalize(void)
{
    /* Create session */
    char sid[DNS_MAX_TXT];
    assert(query_and_get_txt("init.llm.local.", sid, sizeof(sid)) == DNS_RCODE_OK);

    /* Upload a payload */
    const char *payload = "{\"type\":\"user\",\"content\":\"test\"}";
    char b32[256];
    base32_encode((const uint8_t *)payload, strlen(payload), b32, sizeof(b32));
    for (char *p = b32; *p; p++)
        *p = (char)tolower((unsigned char)*p);

    char qname[1024], txt[DNS_MAX_TXT];
    snprintf(qname, sizeof(qname), "%s.0.up.1.%s.llm.local.", b32, sid);
    assert(query_and_get_txt(qname, txt, sizeof(txt)) == DNS_RCODE_OK);

    /* Finalize — spawns LLM thread (will fail without API key) */
    snprintf(qname, sizeof(qname), "fin.1.%s.llm.local.", sid);
    assert(query_and_get_txt(qname, txt, sizeof(txt)) == DNS_RCODE_OK);

    /* Poll download — wait for LLM thread to complete (it will fail) */
    snprintf(qname, sizeof(qname), "0.1.down.%s.llm.local.", sid);
    int got_result = 0;
    for (int i = 0; i < 50; i++) { /* up to 5 seconds */
        int rcode = query_and_get_txt(qname, txt, sizeof(txt));
        assert(rcode == DNS_RCODE_OK);
        if (strcmp(txt, "PENDING") != 0) {
            got_result = 1;
            break;
        }
        usleep(100000); /* 100ms */
    }
    assert(got_result);
    /* LLM call fails (no API key) → ERR:API_FAILED */
    assert(strcmp(txt, "ERR:API_FAILED") == 0);

    PASS("download_after_finalize");
}

static void test_invalid_session(void)
{
    /* Upload to non-existent session */
    char txt[DNS_MAX_TXT];
    char qname[512];
    snprintf(qname, sizeof(qname), "aaaa.0.up.0.nonexistent123456.llm.local.");
    assert(query_and_get_txt(qname, txt, sizeof(txt)) == DNS_RCODE_OK);
    assert(strcmp(txt, "ERR:NOSESSION") == 0);

    /* Download from non-existent session */
    snprintf(qname, sizeof(qname), "0.0.down.nonexistent123456.llm.local.");
    assert(query_and_get_txt(qname, txt, sizeof(txt)) == DNS_RCODE_OK);
    assert(strcmp(txt, "ERR:NOSESSION") == 0);

    /* Finalize on non-existent session */
    snprintf(qname, sizeof(qname), "fin.0.nonexistent123456.llm.local.");
    assert(query_and_get_txt(qname, txt, sizeof(txt)) == DNS_RCODE_OK);
    assert(strcmp(txt, "ERR:NOSESSION") == 0);

    PASS("invalid_session");
}

static void test_unknown_domain(void)
{
    char txt[DNS_MAX_TXT];
    int rcode = query_and_get_txt("hello.example.com.", txt, sizeof(txt));
    assert(rcode == DNS_RCODE_NXDOMAIN);
    PASS("unknown_domain");
}

static void test_encrypted_upload(void)
{
    /* Initialize encryption */
    assert(tunnel_crypto_init("test-integration-key") == 0);
    assert(tunnel_crypto_enabled());

    char sid[DNS_MAX_TXT];
    assert(query_and_get_txt("init.llm.local.", sid, sizeof(sid)) == DNS_RCODE_OK);

    /* Encrypt a payload */
    const char *payload = "{\"type\":\"user\",\"content\":\"encrypted test\"}";
    size_t plen = strlen(payload);
    uint8_t *enc = malloc(plen + CRYPTO_OVERHEAD);
    assert(enc);
    size_t enc_len;
    assert(tunnel_encrypt((const uint8_t *)payload, plen, enc, &enc_len) == 0);

    /* Upload in chunks */
    char qname[1024], txt[DNS_MAX_TXT], b32[256];
    int seq = 0;
    for (size_t i = 0; i < enc_len; i += UPLOAD_CHUNK_SZ) {
        size_t clen = enc_len - i;
        if (clen > UPLOAD_CHUNK_SZ)
            clen = UPLOAD_CHUNK_SZ;
        base32_encode(enc + i, clen, b32, sizeof(b32));
        for (char *p = b32; *p; p++)
            *p = (char)tolower((unsigned char)*p);
        snprintf(qname, sizeof(qname), "%s.%d.up.2.%s.llm.local.", b32, seq++, sid);
        assert(query_and_get_txt(qname, txt, sizeof(txt)) == DNS_RCODE_OK);
        assert(strcmp(txt, "ACK") == 0);
    }
    free(enc);

    /* Finalize */
    snprintf(qname, sizeof(qname), "fin.2.%s.llm.local.", sid);
    assert(query_and_get_txt(qname, txt, sizeof(txt)) == DNS_RCODE_OK);
    assert(strcmp(txt, "ACK") == 0);

    /* Disable encryption for remaining tests */
    tunnel_crypto_init(NULL);

    PASS("encrypted_upload");
}

static void test_auth_token_required(void)
{
    /* Set an auth token on the server */
    strncpy(g_config.auth_token, "secret42", sizeof(g_config.auth_token) - 1);

    char txt[DNS_MAX_TXT];

    /* Without token: should be REFUSED */
    int rcode = query_and_get_txt("init.llm.local.", txt, sizeof(txt));
    assert(rcode == DNS_RCODE_REFUSED);

    /* With wrong token: should be REFUSED */
    rcode = query_and_get_txt("init.wrongtoken.llm.local.", txt, sizeof(txt));
    assert(rcode == DNS_RCODE_REFUSED);

    /* With correct token: should succeed */
    rcode = query_and_get_txt("init.secret42.llm.local.", txt, sizeof(txt));
    assert(rcode == DNS_RCODE_OK);
    assert(strlen(txt) > 0);

    /* Upload with token */
    char sid[64];
    strncpy(sid, txt, sizeof(sid) - 1);
    sid[sizeof(sid) - 1] = '\0';

    char qname[512];
    snprintf(qname, sizeof(qname), "aaaa.0.up.0.%s.secret42.llm.local.", sid);
    rcode = query_and_get_txt(qname, txt, sizeof(txt));
    assert(rcode == DNS_RCODE_OK);
    assert(strcmp(txt, "ACK") == 0);

    /* Upload without token: REFUSED */
    snprintf(qname, sizeof(qname), "aaaa.0.up.0.%s.llm.local.", sid);
    rcode = query_and_get_txt(qname, txt, sizeof(txt));
    assert(rcode == DNS_RCODE_REFUSED);

    /* Clear auth token for remaining tests */
    g_config.auth_token[0] = '\0';

    PASS("auth_token_required");
}

static void test_malformed_queries(void)
{
    char txt[DNS_MAX_TXT];

    /* Bad seq number */
    assert(query_and_get_txt("aaaa.abc.up.0.xxx.llm.local.", txt, sizeof(txt)) ==
           DNS_RCODE_FORMERR);

    /* Bad msg_id */
    assert(query_and_get_txt("aaaa.0.up.abc.xxx.llm.local.", txt, sizeof(txt)) ==
           DNS_RCODE_FORMERR);

    /* Bad msg_id on finalize */
    assert(query_and_get_txt("fin.abc.xxx.llm.local.", txt, sizeof(txt)) == DNS_RCODE_FORMERR);

    /* Bad seq on download */
    assert(query_and_get_txt("abc.0.down.xxx.llm.local.", txt, sizeof(txt)) == DNS_RCODE_FORMERR);

    PASS("malformed_queries");
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("\n  === Integration Tests ===\n\n");

    /* Initialize server state */
    memset(&g_config, 0, sizeof(g_config));
    g_config.provider = PROVIDER_OPENAI;
    strncpy(g_config.api_key, "test-key-not-real", sizeof(g_config.api_key) - 1);
    strncpy(g_config.model, "test-model", sizeof(g_config.model) - 1);
    tunnel_crypto_init(NULL); /* no encryption by default */

    /* Initialize curl for LLM thread (will fail but needs init) */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    test_init_session();
    test_init_returns_unique_ids();
    test_upload_and_finalize();
    test_download_after_finalize();
    test_invalid_session();
    test_unknown_domain();
    test_encrypted_upload();
    test_auth_token_required();
    test_malformed_queries();

    curl_global_cleanup();

    printf("\n  ALL %d INTEGRATION TESTS PASSED\n\n", 9);
    return 0;
}
