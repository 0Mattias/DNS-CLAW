/*
 * client/protocol.c — DNS query wrapper, session init, message processing loop
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cJSON.h>

#include "base32.h"
#include "base64.h"
#include "crypto.h"
#include "dns_proto.h"
#include "protocol.h"
#include "client/protocol.h"
#include "client/render.h"
#include "client/spinner.h"

/* ── Path safety check ────────────────────────────────────────────────────── */

static void warn_suspicious_path(const char *path)
{
    if (path[0] == '/' || strstr(path, "..")) {
        set_fg_rgb(255, 200, 0);
        printf("  │ ⚠ Path contains '%s' — review carefully\n" ANSI_RESET,
               path[0] == '/' ? "absolute path" : "..");
    }
}

/* ── Global session state ─────────────────────────────────────────────────── */

char       g_session_id[64];
int        g_msg_id = 0;
int        g_turn   = 0;
atomic_int g_interrupted = 0;

/* ── DNS Query Wrapper ────────────────────────────────────────────────────── */

int do_dns_query(const char *qname, char *txt_out, size_t txt_out_len)
{
    if (g_interrupted) return -1;

    if (g_cfg.use_doh) {
        return dns_query_doh(g_cfg.server_addr, qname, g_cfg.insecure,
                             txt_out, txt_out_len);
    } else if (g_cfg.use_dot) {
        return dns_query_dot(g_cfg.server_addr, (uint16_t)g_cfg.port,
                             qname, g_cfg.insecure, txt_out, txt_out_len);
    } else {
        return dns_query_udp(g_cfg.server_addr, (uint16_t)g_cfg.port,
                             qname, txt_out, txt_out_len);
    }
}

/* ── Session Init ─────────────────────────────────────────────────────────── */

int init_session(int show_msg)
{
    spinner_t sp;
    spinner_start(&sp, "Initializing session...");

    char txt[DNS_MAX_TXT];
    int rc = do_dns_query("init.llm.local.", txt, sizeof(txt));

    spinner_stop(&sp);

    if (rc < 0) {
        set_fg_rgb(255, 80, 80);
        printf("  ✗ Failed to initialize session.\n" ANSI_RESET);
        return -1;
    }

    char *p = txt;
    while (*p == ' ' || *p == '\n' || *p == '\r') p++;
    strncpy(g_session_id, p, sizeof(g_session_id) - 1);
    size_t slen = strlen(g_session_id);
    while (slen > 0 && (g_session_id[slen-1] == ' ' ||
           g_session_id[slen-1] == '\n' || g_session_id[slen-1] == '\r'))
        g_session_id[--slen] = '\0';

    g_msg_id = 0;
    g_turn   = 0;

    if (show_msg) {
        set_fg_rgb(THEME_R2);
        printf("  ✓ " ANSI_RESET);
        printf(ANSI_DIM "Session: %s\n" ANSI_RESET, g_session_id);
    }

    return 0;
}

/* ── Message Processing Loop ──────────────────────────────────────────────── */

int process_message_loop(const char *type, const char *content,
                         const char *tool_name)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "type", type);
    cJSON_AddStringToObject(payload, "content", content);
    if (tool_name && tool_name[0])
        cJSON_AddStringToObject(payload, "tool_name", tool_name);

    double round_start = now_ms();

    while (1) {
        if (g_interrupted) {
            cJSON_Delete(payload);
            return -1;
        }

        g_msg_id = (g_msg_id + 1) % MAX_MSG_IDS;
        int current_mid = g_msg_id;

        char spin_msg[128];
        snprintf(spin_msg, sizeof(spin_msg),
                 "Transmitting [msg %d]...", current_mid);
        spinner_t sp;
        spinner_start(&sp, spin_msg);

        char *payload_str = cJSON_PrintUnformatted(payload);
        size_t payload_len = strlen(payload_str);

        /* Encrypt payload if PSK is configured */
        uint8_t *upload_data = (uint8_t *)payload_str;
        size_t upload_len = payload_len;
        uint8_t *encrypted_buf = NULL;
        if (tunnel_crypto_enabled()) {
            encrypted_buf = malloc(payload_len + CRYPTO_OVERHEAD);
            if (!encrypted_buf) {
                free(payload_str);
                spinner_stop(&sp);
                cJSON_Delete(payload);
                set_fg_rgb(255, 80, 80);
                printf("  ✗ Out of memory\n" ANSI_RESET);
                return -1;
            }
            if (tunnel_encrypt((uint8_t *)payload_str, payload_len,
                               encrypted_buf, &upload_len) < 0) {
                free(encrypted_buf);
                free(payload_str);
                spinner_stop(&sp);
                cJSON_Delete(payload);
                set_fg_rgb(255, 80, 80);
                printf("  ✗ Encryption failed\n" ANSI_RESET);
                return -1;
            }
            upload_data = encrypted_buf;
        }

        /* 1. Upload chunks (base32) */
        char b32_buf[256];
        char qname[1024];
        char txt[DNS_MAX_TXT];

        int seq = 0;
        int upload_ok = 1;
        for (size_t i = 0; i < upload_len && upload_ok; i += UPLOAD_CHUNK_SZ) {
            size_t clen = upload_len - i;
            if (clen > UPLOAD_CHUNK_SZ) clen = UPLOAD_CHUNK_SZ;

            base32_encode(upload_data + i, clen,
                          b32_buf, sizeof(b32_buf));

            for (char *cp = b32_buf; *cp; cp++)
                *cp = (char)tolower((unsigned char)*cp);

            snprintf(qname, sizeof(qname), "%s.%d.up.%d.%s.llm.local.",
                     b32_buf, seq, current_mid, g_session_id);

            if (do_dns_query(qname, txt, sizeof(txt)) < 0 ||
                strcmp(txt, "ACK") != 0) {
                upload_ok = 0;
            }
            seq++;
        }
        free(encrypted_buf);
        free(payload_str);

        if (!upload_ok) {
            spinner_stop(&sp);
            cJSON_Delete(payload);
            set_fg_rgb(255, 80, 80);
            printf("  ✗ Upload failed at chunk %d\n" ANSI_RESET, seq - 1);
            return -1;
        }

        /* 2. Send fin */
        snprintf(spin_msg, sizeof(spin_msg), "Waiting for agent...");
        spinner_set_message(&sp, spin_msg);

        snprintf(qname, sizeof(qname), "fin.%d.%s.llm.local.",
                 current_mid, g_session_id);
        if (do_dns_query(qname, txt, sizeof(txt)) < 0 ||
            strcmp(txt, "ACK") != 0) {
            spinner_stop(&sp);
            cJSON_Delete(payload);
            set_fg_rgb(255, 80, 80);
            printf("  ✗ Server did not ACK finalize\n" ANSI_RESET);
            return -1;
        }

        /* 3. Poll + download response */
        int down_seq = 0;
        char *full_response = calloc(RESP_BUF_SIZE, 1);
        if (!full_response) {
            spinner_stop(&sp);
            cJSON_Delete(payload);
            set_fg_rgb(255, 80, 80);
            printf("  ✗ Out of memory\n" ANSI_RESET);
            return -1;
        }
        size_t resp_pos = 0;
        int poll_count = 0;

        while (!g_interrupted) {
            snprintf(qname, sizeof(qname), "%d.%d.down.%s.llm.local.",
                     down_seq, current_mid, g_session_id);
            if (do_dns_query(qname, txt, sizeof(txt)) < 0) {
                spinner_stop(&sp);
                cJSON_Delete(payload);
                free(full_response);
                set_fg_rgb(255, 80, 80);
                printf("  ✗ Download failed at chunk %d\n" ANSI_RESET, down_seq);
                return -1;
            }

            if (strcmp(txt, "PENDING") == 0) {
                poll_count++;
                snprintf(spin_msg, sizeof(spin_msg),
                         "Agent is thinking... (poll %d)", poll_count);
                spinner_set_message(&sp, spin_msg);
                usleep(500000);
                continue;
            } else if (strcmp(txt, "EOF") == 0) {
                break;
            } else if (strncmp(txt, "ERR:", 4) == 0) {
                spinner_stop(&sp);
                cJSON_Delete(payload);
                free(full_response);
                set_fg_rgb(255, 80, 80);
                printf("  ✗ %s\n" ANSI_RESET, txt);
                return -1;
            }

            uint8_t decoded[4096];
            int dlen = base64_decode(txt, decoded, sizeof(decoded));
            if (dlen < 0) {
                spinner_stop(&sp);
                cJSON_Delete(payload);
                free(full_response);
                set_fg_rgb(255, 80, 80);
                printf("  ✗ Base64 decode error on chunk %d\n" ANSI_RESET, down_seq);
                return -1;
            }

            snprintf(spin_msg, sizeof(spin_msg),
                     "Receiving chunk %d...", down_seq);
            spinner_set_message(&sp, spin_msg);

            if (resp_pos + (size_t)dlen < RESP_BUF_SIZE) {
                memcpy(full_response + resp_pos, decoded, (size_t)dlen);
                resp_pos += (size_t)dlen;
            }
            down_seq++;
        }
        full_response[resp_pos] = '\0';

        spinner_stop(&sp);

        if (g_interrupted) {
            cJSON_Delete(payload);
            free(full_response);
            return -1;
        }

        /* Decrypt response if PSK is configured */
        if (tunnel_crypto_enabled() && resp_pos > CRYPTO_OVERHEAD) {
            uint8_t *decrypted = malloc(resp_pos);
            size_t dec_len = 0;
            if (!decrypted ||
                tunnel_decrypt((uint8_t *)full_response, resp_pos,
                               decrypted, &dec_len) != 0) {
                free(decrypted);
                free(full_response);
                cJSON_Delete(payload);
                set_fg_rgb(255, 80, 80);
                printf("  ✗ Decryption failed — PSK mismatch or corrupted data\n" ANSI_RESET);
                return -1;
            }
            memcpy(full_response, decrypted, dec_len);
            full_response[dec_len] = '\0';
            free(decrypted);
        }

        /* 4. Parse JSON response */
        cJSON *resp_json = cJSON_Parse(full_response);
        free(full_response);
        full_response = NULL;
        if (!resp_json) {
            cJSON_Delete(payload);
            set_fg_rgb(255, 80, 80);
            printf("  ✗ Malformed response JSON\n" ANSI_RESET);
            return -1;
        }

        const char *resp_type = cJSON_GetStringValue(
            cJSON_GetObjectItem(resp_json, "type"));

        if (resp_type && strcmp(resp_type, "tool_call") == 0) {
            /* ── Tool call ───────────────────────────────────────── */
            const char *fn = cJSON_GetStringValue(
                cJSON_GetObjectItem(resp_json, "tool_name"));
            cJSON *args = cJSON_GetObjectItem(resp_json, "tool_args");

            printf("\n");
            set_fg_rgb(THEME_R1);
            printf("  ┌ ");
            set_bg_rgb(THEME_R1);
            set_fg_rgb(0, 0, 0);
            printf(ANSI_BOLD " ⚡ Tool Call " ANSI_RESET);
            set_fg_rgb(THEME_R2);
            printf(" ── %s\n" ANSI_RESET, fn ? fn : "unknown");

            char tool_result[TOOL_RESULT_SIZE] = {0};

            if (fn && strcmp(fn, "client_execute_bash") == 0) {
                const char *cmd = cJSON_GetStringValue(
                    cJSON_GetObjectItem(args, "command"));
                if (!cmd) {
                    snprintf(tool_result, sizeof(tool_result),
                             "Error: Invalid command argument");
                } else {
                    set_fg_rgb(THEME_DIM);
                    printf("  │\n");
                    printf("  │ " ANSI_RESET);
                    set_fg_rgb(THEME_R4);
                    set_bg_rgb(50, 30, 30);
                    printf(" $ %s " ANSI_RESET, cmd);
                    printf("\n");
                    set_fg_rgb(THEME_DIM);
                    printf("  │\n" ANSI_RESET);

                    printf("  ");
                    set_fg_rgb(THEME_R4);
                    printf("  Allow? " ANSI_RESET);
                    set_fg_rgb(THEME_R2);
                    printf("[Y]es");
                    printf(ANSI_RESET " / ");
                    set_fg_rgb(THEME_DIM);
                    printf("[n]o" ANSI_RESET ": ");
                    fflush(stdout);

                    char confirm[32] = {0};
                    if (fgets(confirm, sizeof(confirm), stdin)) {
                        char *c = confirm;
                        while (*c == ' ') c++;

                        if (c[0] == 'y' || c[0] == 'Y' ||
                            c[0] == '\n' || c[0] == '\0') {
                            FILE *proc = popen(cmd, "r");
                            if (!proc) {
                                snprintf(tool_result, sizeof(tool_result),
                                         "Error: popen failed: %s", strerror(errno));
                            } else {
                                size_t total = 0;
                                while (total < sizeof(tool_result) - 1) {
                                    size_t n = fread(tool_result + total, 1,
                                                     sizeof(tool_result) - 1 - total,
                                                     proc);
                                    if (n == 0) break;
                                    total += n;
                                }
                                tool_result[total] = '\0';
                                int status = pclose(proc);
                                if (status != 0 && total == 0)
                                    snprintf(tool_result, sizeof(tool_result),
                                             "Command exited with status %d", status);
                                if (strlen(tool_result) == 0)
                                    snprintf(tool_result, sizeof(tool_result), "(no output)");
                            }
                        } else {
                            set_fg_rgb(THEME_R1);
                            printf("  ✗ Rejected\n" ANSI_RESET);
                            snprintf(tool_result, sizeof(tool_result),
                                "User rejected command execution. "
                                "Ask what they'd like instead.");
                        }
                    }
                    if (strlen(tool_result) > TOOL_RESULT_SIZE - 16) {
                        snprintf(tool_result + TOOL_RESULT_SIZE - 16, 16,
                                 "\n...[truncated]");
                    }
                }

            } else if (fn && strcmp(fn, "client_read_file") == 0) {
                const char *fpath = cJSON_GetStringValue(
                    cJSON_GetObjectItem(args, "filepath"));
                if (!fpath) {
                    snprintf(tool_result, sizeof(tool_result),
                             "Error: Invalid filepath");
                } else {
                    set_fg_rgb(THEME_DIM);
                    printf("  │ ");
                    set_fg_rgb(THEME_R3);
                    printf("📄 %s" ANSI_RESET, fpath);

                    FILE *fp = fopen(fpath, "r");
                    if (!fp) {
                        printf("\n");
                        snprintf(tool_result, sizeof(tool_result),
                                 "Error: %s", strerror(errno));
                    } else {
                        size_t total = fread(tool_result, 1,
                                             sizeof(tool_result) - 1, fp);
                        tool_result[total] = '\0';
                        fclose(fp);
                        set_fg_rgb(THEME_DIM);
                        printf(" (%zu bytes)\n" ANSI_RESET, total);
                        if (total > TOOL_RESULT_SIZE - 16) {
                            snprintf(tool_result + TOOL_RESULT_SIZE - 16, 16,
                                     "\n...[truncated]");
                        }
                    }
                }

            } else if (fn && strcmp(fn, "client_write_file") == 0) {
                const char *fpath = cJSON_GetStringValue(
                    cJSON_GetObjectItem(args, "filepath"));
                const char *fcontent = cJSON_GetStringValue(
                    cJSON_GetObjectItem(args, "content"));
                if (!fpath || !fcontent) {
                    snprintf(tool_result, sizeof(tool_result),
                             "Error: Missing filepath or content");
                } else {
                    set_fg_rgb(THEME_DIM);
                    printf("  │ ");
                    set_fg_rgb(THEME_R3);
                    printf("📝 Writing: %s (%zu bytes)\n" ANSI_RESET,
                           fpath, strlen(fcontent));
                    warn_suspicious_path(fpath);

                    printf("  ");
                    set_fg_rgb(THEME_R4);
                    printf("  Allow write? " ANSI_RESET);
                    set_fg_rgb(THEME_R2);
                    printf("[Y]es");
                    printf(ANSI_RESET " / ");
                    set_fg_rgb(THEME_DIM);
                    printf("[n]o" ANSI_RESET ": ");
                    fflush(stdout);

                    char confirm[32] = {0};
                    if (fgets(confirm, sizeof(confirm), stdin)) {
                        char *c = confirm;
                        while (*c == ' ') c++;
                        if (c[0] == 'y' || c[0] == 'Y' ||
                            c[0] == '\n' || c[0] == '\0') {
                            FILE *fp = fopen(fpath, "w");
                            if (!fp) {
                                snprintf(tool_result, sizeof(tool_result),
                                         "Error: %s", strerror(errno));
                            } else {
                                fwrite(fcontent, 1, strlen(fcontent), fp);
                                fclose(fp);
                                snprintf(tool_result, sizeof(tool_result),
                                         "Wrote %zu bytes to %s",
                                         strlen(fcontent), fpath);
                            }
                        } else {
                            snprintf(tool_result, sizeof(tool_result),
                                "User rejected file write.");
                        }
                    }
                }

            } else if (fn && strcmp(fn, "client_list_directory") == 0) {
                const char *dpath = cJSON_GetStringValue(
                    cJSON_GetObjectItem(args, "path"));
                if (!dpath || !dpath[0]) dpath = ".";

                set_fg_rgb(THEME_DIM);
                printf("  │ ");
                set_fg_rgb(THEME_R3);
                printf("📁 %s\n" ANSI_RESET, dpath);

                DIR *dir = opendir(dpath);
                if (!dir) {
                    snprintf(tool_result, sizeof(tool_result),
                             "Error: %s", strerror(errno));
                } else {
                    struct dirent *ent;
                    size_t pos = 0;
                    while ((ent = readdir(dir)) != NULL && pos < sizeof(tool_result) - 300) {
                        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                            continue;
                        char fullpath[1024];
                        snprintf(fullpath, sizeof(fullpath), "%s/%s", dpath, ent->d_name);
                        struct stat st;
                        const char *type_str = "";
                        if (stat(fullpath, &st) == 0) {
                            if (S_ISDIR(st.st_mode)) type_str = "dir  ";
                            else                     type_str = "file ";
                            pos += (size_t)snprintf(tool_result + pos,
                                sizeof(tool_result) - pos,
                                "%s %8lld  %s\n",
                                type_str, (long long)st.st_size, ent->d_name);
                        } else {
                            pos += (size_t)snprintf(tool_result + pos,
                                sizeof(tool_result) - pos,
                                "???   %s\n", ent->d_name);
                        }
                    }
                    closedir(dir);
                    if (pos == 0)
                        snprintf(tool_result, sizeof(tool_result), "(empty directory)");
                }

            } else if (fn && strcmp(fn, "client_edit_file") == 0) {
                const char *fpath = cJSON_GetStringValue(
                    cJSON_GetObjectItem(args, "filepath"));
                const char *old_str = cJSON_GetStringValue(
                    cJSON_GetObjectItem(args, "old_string"));
                const char *new_str = cJSON_GetStringValue(
                    cJSON_GetObjectItem(args, "new_string"));
                if (!fpath || !old_str || !new_str) {
                    snprintf(tool_result, sizeof(tool_result),
                             "Error: Missing filepath, old_string, or new_string");
                } else {
                    set_fg_rgb(THEME_DIM);
                    printf("  │ ");
                    set_fg_rgb(THEME_R3);
                    printf("✏️  Editing: %s\n" ANSI_RESET, fpath);
                    warn_suspicious_path(fpath);

                    printf("  ");
                    set_fg_rgb(THEME_R4);
                    printf("  Allow edit? " ANSI_RESET);
                    set_fg_rgb(THEME_R2);
                    printf("[Y]es");
                    printf(ANSI_RESET " / ");
                    set_fg_rgb(THEME_DIM);
                    printf("[n]o" ANSI_RESET ": ");
                    fflush(stdout);

                    char confirm[32] = {0};
                    if (fgets(confirm, sizeof(confirm), stdin)) {
                        char *c = confirm;
                        while (*c == ' ') c++;
                        if (c[0] == 'y' || c[0] == 'Y' ||
                            c[0] == '\n' || c[0] == '\0') {
                            /* Read file */
                            FILE *fp = fopen(fpath, "r");
                            if (!fp) {
                                snprintf(tool_result, sizeof(tool_result),
                                         "Error: %s", strerror(errno));
                            } else {
                                fseek(fp, 0, SEEK_END);
                                long fsize = ftell(fp);
                                fseek(fp, 0, SEEK_SET);
                                if (fsize < 0 || fsize > 10 * 1024 * 1024) {
                                    snprintf(tool_result, sizeof(tool_result),
                                             "Error: File too large (%ld bytes)", fsize);
                                    fclose(fp);
                                } else {
                                    char *content = malloc((size_t)fsize + 1);
                                    if (!content) {
                                        snprintf(tool_result, sizeof(tool_result),
                                                 "Error: Out of memory");
                                        fclose(fp);
                                    } else {
                                        size_t rd = fread(content, 1, (size_t)fsize, fp);
                                        content[rd] = '\0';
                                        fclose(fp);

                                        char *match = strstr(content, old_str);
                                        if (!match) {
                                            snprintf(tool_result, sizeof(tool_result),
                                                     "Error: old_string not found in file");
                                        } else {
                                            /* Check uniqueness */
                                            char *second = strstr(match + 1, old_str);
                                            if (second) {
                                                snprintf(tool_result, sizeof(tool_result),
                                                         "Error: old_string matches multiple "
                                                         "locations — provide more context to "
                                                         "make it unique");
                                            } else {
                                                size_t old_len = strlen(old_str);
                                                size_t new_len = strlen(new_str);
                                                size_t prefix_len = (size_t)(match - content);
                                                size_t suffix_len = rd - prefix_len - old_len;
                                                size_t new_size = prefix_len + new_len + suffix_len;
                                                char *result = malloc(new_size + 1);
                                                if (!result) {
                                                    snprintf(tool_result, sizeof(tool_result),
                                                             "Error: Out of memory");
                                                } else {
                                                    memcpy(result, content, prefix_len);
                                                    memcpy(result + prefix_len, new_str, new_len);
                                                    memcpy(result + prefix_len + new_len,
                                                           match + old_len, suffix_len);
                                                    result[new_size] = '\0';

                                                    FILE *wfp = fopen(fpath, "w");
                                                    if (!wfp) {
                                                        snprintf(tool_result, sizeof(tool_result),
                                                                 "Error: %s", strerror(errno));
                                                    } else {
                                                        fwrite(result, 1, new_size, wfp);
                                                        fclose(wfp);
                                                        snprintf(tool_result, sizeof(tool_result),
                                                                 "Applied edit to %s (%zu bytes → %zu bytes)",
                                                                 fpath, rd, new_size);
                                                    }
                                                    free(result);
                                                }
                                            }
                                        }
                                        free(content);
                                    }
                                }
                            }
                        } else {
                            snprintf(tool_result, sizeof(tool_result),
                                     "User rejected file edit.");
                        }
                    }
                }

            } else if (fn && strcmp(fn, "client_search_files") == 0) {
                const char *pattern = cJSON_GetStringValue(
                    cJSON_GetObjectItem(args, "pattern"));
                const char *spath = cJSON_GetStringValue(
                    cJSON_GetObjectItem(args, "path"));
                const char *include = cJSON_GetStringValue(
                    cJSON_GetObjectItem(args, "include"));
                if (!pattern) {
                    snprintf(tool_result, sizeof(tool_result),
                             "Error: Missing pattern");
                } else {
                    if (!spath || !spath[0]) spath = ".";
                    set_fg_rgb(THEME_DIM);
                    printf("  │ ");
                    set_fg_rgb(THEME_R3);
                    printf("🔍 grep '%s' in %s", pattern, spath);
                    if (include) printf(" (%s)", include);
                    printf("\n" ANSI_RESET);

                    char cmd[4096];
                    if (include) {
                        snprintf(cmd, sizeof(cmd),
                                 "grep -rn --include='%s' -- '%s' '%s' 2>&1 | head -200",
                                 include, pattern, spath);
                    } else {
                        snprintf(cmd, sizeof(cmd),
                                 "grep -rn -- '%s' '%s' 2>&1 | head -200",
                                 pattern, spath);
                    }
                    FILE *proc = popen(cmd, "r");
                    if (!proc) {
                        snprintf(tool_result, sizeof(tool_result),
                                 "Error: popen failed: %s", strerror(errno));
                    } else {
                        size_t total = 0;
                        while (total < sizeof(tool_result) - 1) {
                            size_t n = fread(tool_result + total, 1,
                                             sizeof(tool_result) - 1 - total, proc);
                            if (n == 0) break;
                            total += n;
                        }
                        tool_result[total] = '\0';
                        pclose(proc);
                        if (total == 0)
                            snprintf(tool_result, sizeof(tool_result),
                                     "(no matches)");
                    }
                }

            } else if (fn && strcmp(fn, "client_fetch_url") == 0) {
                const char *url = cJSON_GetStringValue(
                    cJSON_GetObjectItem(args, "url"));
                if (!url) {
                    snprintf(tool_result, sizeof(tool_result),
                             "Error: Missing url");
                } else {
                    set_fg_rgb(THEME_DIM);
                    printf("  │ ");
                    set_fg_rgb(THEME_R3);
                    printf("🌐 %s\n" ANSI_RESET, url);

                    char cmd[4096];
                    snprintf(cmd, sizeof(cmd),
                             "curl -sL --max-time 15 --max-filesize 1048576 '%s' 2>&1",
                             url);
                    FILE *proc = popen(cmd, "r");
                    if (!proc) {
                        snprintf(tool_result, sizeof(tool_result),
                                 "Error: popen failed: %s", strerror(errno));
                    } else {
                        size_t total = 0;
                        while (total < sizeof(tool_result) - 1) {
                            size_t n = fread(tool_result + total, 1,
                                             sizeof(tool_result) - 1 - total, proc);
                            if (n == 0) break;
                            total += n;
                        }
                        tool_result[total] = '\0';
                        pclose(proc);
                        if (total == 0)
                            snprintf(tool_result, sizeof(tool_result),
                                     "(empty response)");
                    }
                }

            } else {
                snprintf(tool_result, sizeof(tool_result),
                         "Error: Unknown tool '%s'", fn ? fn : "(null)");
            }

            /* Print result in bordered box */
            set_fg_rgb(THEME_DIM);
            printf("  │\n");
            int w = term_width() - 6;
            if (w > 120) w = 120;

            const char *rl = tool_result;
            while (*rl) {
                const char *nl = strchr(rl, '\n');
                size_t ll = nl ? (size_t)(nl - rl) : strlen(rl);
                if (ll > (size_t)w) ll = (size_t)w;
                set_fg_rgb(THEME_DIM);
                printf("  │ " ANSI_RESET ANSI_DIM);
                fwrite(rl, 1, ll, stdout);
                printf(ANSI_RESET "\n");
                rl = nl ? nl + 1 : rl + strlen(rl);
            }

            set_fg_rgb(THEME_DIM);
            printf("  └");
            for (int i = 0; i < w; i++) printf("─");
            printf("\n" ANSI_RESET);

            /* Continue loop with tool response */
            cJSON_Delete(payload);
            cJSON_Delete(resp_json);

            payload = cJSON_CreateObject();
            if (!payload) {
                set_fg_rgb(255, 80, 80);
                printf("  ✗ Out of memory\n" ANSI_RESET);
                return -1;
            }
            cJSON_AddStringToObject(payload, "type", "tool_response");
            cJSON_AddStringToObject(payload, "content", tool_result);
            if (fn) cJSON_AddStringToObject(payload, "tool_name", fn);
            continue;

        } else {
            /* ── Text response ───────────────────────────────────── */
            const char *text = cJSON_GetStringValue(
                cJSON_GetObjectItem(resp_json, "content"));

            double elapsed = (now_ms() - round_start) / 1000.0;

            printf("\n");
            set_bg_rgb(THEME_R1);
            set_fg_rgb(255, 255, 255);
            const char *prov_name = cJSON_GetStringValue(
                cJSON_GetObjectItem(resp_json, "provider"));
            if (!prov_name || !prov_name[0]) prov_name = "Agent";
            printf(ANSI_BOLD " ✦ %s " ANSI_RESET, prov_name);
            set_fg_rgb(THEME_DIM);
            printf(" %.1fs", elapsed);
            printf(ANSI_RESET "\n");

            if (text) {
                render_markdown(text);
            }

            cJSON_Delete(resp_json);
            cJSON_Delete(payload);
            break;
        }
    }

    return 0;
}
