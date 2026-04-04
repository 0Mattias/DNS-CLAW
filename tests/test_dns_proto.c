/*
 * test_dns_proto.c — Unit tests for DNS wire-format builder/parser.
 */
#undef NDEBUG /* ensure assert() is always active in tests */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "dns_proto.h"

#define PASS(name) printf("  PASS  %s\n", name)

static void test_build_parse_query(void)
{
    uint8_t buf[DNS_MAX_MSG];
    int len = dns_build_query(0x1234, "init.llm.local.", buf, sizeof(buf));
    assert(len > 12);

    uint16_t id;
    char qname[256];
    assert(dns_parse_query(buf, (size_t)len, &id, qname, sizeof(qname)) == 0);
    assert(id == 0x1234);
    assert(strcmp(qname, "init.llm.local.") == 0);
    PASS("build_parse_query");
}

static void test_build_parse_response_with_txt(void)
{
    uint8_t buf[DNS_MAX_MSG];
    int len = dns_build_response(0xABCD, "test.llm.local.", DNS_RCODE_OK, "Hello World", buf,
                                 sizeof(buf));
    assert(len > 0);

    int rcode;
    char txt[DNS_MAX_TXT];
    assert(dns_parse_txt_response(buf, (size_t)len, &rcode, txt, sizeof(txt)) == 0);
    assert(rcode == DNS_RCODE_OK);
    assert(strcmp(txt, "Hello World") == 0);
    PASS("build_parse_response_with_txt");
}

static void test_response_no_answer(void)
{
    uint8_t buf[DNS_MAX_MSG];
    int len =
        dns_build_response(0x0001, "x.llm.local.", DNS_RCODE_NXDOMAIN, NULL, buf, sizeof(buf));
    assert(len > 0);

    int rcode;
    char txt[DNS_MAX_TXT];
    assert(dns_parse_txt_response(buf, (size_t)len, &rcode, txt, sizeof(txt)) == 0);
    assert(rcode == DNS_RCODE_NXDOMAIN);
    assert(txt[0] == '\0');
    PASS("response_no_answer");
}

static void test_long_txt(void)
{
    /* TXT records > 255 bytes require multiple character-strings */
    char long_txt[600];
    memset(long_txt, 'A', sizeof(long_txt) - 1);
    long_txt[sizeof(long_txt) - 1] = '\0';

    uint8_t buf[DNS_MAX_MSG];
    int len = dns_build_response(0x0002, "t.llm.local.", DNS_RCODE_OK, long_txt, buf, sizeof(buf));
    assert(len > 0);

    int rcode;
    char txt[DNS_MAX_TXT];
    assert(dns_parse_txt_response(buf, (size_t)len, &rcode, txt, sizeof(txt)) == 0);
    assert(strcmp(txt, long_txt) == 0);
    PASS("long_txt");
}

static void test_roundtrip_session_id(void)
{
    /* Simulate the session init flow: server sends session ID as TXT */
    const char *sid = "a1b2c3d4e5f67890";
    uint8_t buf[DNS_MAX_MSG];
    int len = dns_build_response(0x0003, "init.llm.local.", DNS_RCODE_OK, sid, buf, sizeof(buf));
    assert(len > 0);

    int rcode;
    char txt[DNS_MAX_TXT];
    assert(dns_parse_txt_response(buf, (size_t)len, &rcode, txt, sizeof(txt)) == 0);
    assert(strcmp(txt, sid) == 0);
    PASS("roundtrip_session_id");
}

static void test_truncated_message_rejected(void)
{
    uint8_t buf[8] = {0}; /* too short for DNS header */
    int rcode;
    char txt[64];
    assert(dns_parse_txt_response(buf, sizeof(buf), &rcode, txt, sizeof(txt)) == -1);
    PASS("truncated_message_rejected");
}

static void test_buffer_too_small(void)
{
    uint8_t buf[8]; /* way too small */
    int len = dns_build_query(0x1234, "init.llm.local.", buf, sizeof(buf));
    assert(len == -1);
    PASS("buffer_too_small");
}

static void test_label_too_long(void)
{
    /* DNS labels are limited to 63 chars */
    char long_label[128];
    memset(long_label, 'a', 64);
    long_label[64] = '.';
    strcpy(long_label + 65, "llm.local.");

    uint8_t buf[DNS_MAX_MSG];
    int len = dns_build_query(0x0001, long_label, buf, sizeof(buf));
    assert(len == -1);
    PASS("label_too_long");
}

static void test_empty_message_rejected(void)
{
    int rcode;
    char txt[64];
    assert(dns_parse_txt_response(NULL, 0, &rcode, txt, sizeof(txt)) == -1);
    PASS("empty_message_rejected");
}

static void test_query_parse_truncated(void)
{
    uint16_t id;
    char qname[256];
    /* 11 bytes — too short for DNS header (need 12) */
    uint8_t buf[11] = {0};
    assert(dns_parse_query(buf, sizeof(buf), &id, qname, sizeof(qname)) == -1);
    PASS("query_parse_truncated");
}

static void test_empty_txt_roundtrip(void)
{
    uint8_t buf[DNS_MAX_MSG];
    int len = dns_build_response(0x0042, "t.llm.local.", DNS_RCODE_OK, "", buf, sizeof(buf));
    assert(len > 0);

    int rcode;
    char txt[DNS_MAX_TXT];
    assert(dns_parse_txt_response(buf, (size_t)len, &rcode, txt, sizeof(txt)) == 0);
    assert(rcode == DNS_RCODE_OK);
    assert(txt[0] == '\0');
    PASS("empty_txt_roundtrip");
}

static void test_max_label_length(void)
{
    /* 63 chars is the maximum DNS label length — should succeed */
    char label[128];
    memset(label, 'a', 63);
    label[63] = '.';
    strcpy(label + 64, "llm.local.");

    uint8_t buf[DNS_MAX_MSG];
    int len = dns_build_query(0x0001, label, buf, sizeof(buf));
    assert(len > 0);

    uint16_t id;
    char qname[256];
    assert(dns_parse_query(buf, (size_t)len, &id, qname, sizeof(qname)) == 0);
    assert(id == 0x0001);
    PASS("max_label_length");
}

static void test_multiple_labels(void)
{
    /* Typical tunnel query name with many labels */
    const char *qname = "data.0.up.1.abcdef12.llm.local.";
    uint8_t buf[DNS_MAX_MSG];
    int len = dns_build_query(0x5678, qname, buf, sizeof(buf));
    assert(len > 0);

    uint16_t id;
    char parsed[256];
    assert(dns_parse_query(buf, (size_t)len, &id, parsed, sizeof(parsed)) == 0);
    assert(id == 0x5678);
    assert(strcmp(parsed, qname) == 0);
    PASS("multiple_labels");
}

static void test_response_rcode_preserved(void)
{
    int rcodes[] = {DNS_RCODE_OK, DNS_RCODE_FORMERR, DNS_RCODE_SERVFAIL, DNS_RCODE_NXDOMAIN};
    for (int i = 0; i < 4; i++) {
        uint8_t buf[DNS_MAX_MSG];
        int len = dns_build_response(0x0001, "t.llm.local.", rcodes[i], NULL, buf, sizeof(buf));
        assert(len > 0);

        int rcode;
        char txt[DNS_MAX_TXT];
        assert(dns_parse_txt_response(buf, (size_t)len, &rcode, txt, sizeof(txt)) == 0);
        assert(rcode == rcodes[i]);
    }
    PASS("response_rcode_preserved");
}

static void test_query_id_full_range(void)
{
    /* Test query ID at boundaries: 0, 0xFFFF, and a mid value */
    uint16_t ids[] = {0x0000, 0xFFFF, 0x8000};
    for (int i = 0; i < 3; i++) {
        uint8_t buf[DNS_MAX_MSG];
        int len = dns_build_query(ids[i], "t.llm.local.", buf, sizeof(buf));
        assert(len > 0);

        uint16_t id;
        char qname[256];
        assert(dns_parse_query(buf, (size_t)len, &id, qname, sizeof(qname)) == 0);
        assert(id == ids[i]);
    }
    PASS("query_id_full_range");
}

int main(void)
{
    printf("dns_proto:\n");
    test_build_parse_query();
    test_build_parse_response_with_txt();
    test_response_no_answer();
    test_long_txt();
    test_roundtrip_session_id();
    test_truncated_message_rejected();
    test_buffer_too_small();
    test_label_too_long();
    test_empty_message_rejected();
    test_query_parse_truncated();
    test_empty_txt_roundtrip();
    test_max_label_length();
    test_multiple_labels();
    test_response_rcode_preserved();
    test_query_id_full_range();
    printf("  ALL PASSED\n");
    return 0;
}
