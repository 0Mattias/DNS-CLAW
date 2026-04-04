/*
 * server/session.c — Session state management and reaper
 */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cJSON.h>

#include "protocol.h"
#include "server/session.h"
#include "server/log.h"
#include "server/transport.h" /* g_running, g_config */

/* ── Global session state ─────────────────────────────────────────────────── */

session_t g_sessions[MAX_SESSIONS];
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── Functions ────────────────────────────────────────────────────────────── */

void session_free_responses(session_t *sess)
{
    for (int m = 0; m < MAX_MSG_IDS; m++) {
        msg_response_t *mr = &sess->responses[m];
        for (int c = 0; c < mr->chunk_count; c++) {
            free(mr->chunks[c]);
            mr->chunks[c] = NULL;
        }
        mr->chunk_count = 0;
    }
}

void session_destroy(session_t *sess)
{
    session_free_responses(sess);
    if (sess->history) {
        cJSON_Delete(sess->history);
        sess->history = NULL;
    }
    sess->active = 0;
}

void *session_reaper_thread(void *arg)
{
    (void)arg;
    while (g_running) {
        sleep(60);
        time_t now = time(NULL);
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (g_sessions[i].active && g_sessions[i].busy == 0 &&
                difftime(now, g_sessions[i].last_active) > SESSION_TIMEOUT_SEC) {
                log_warn("reaper", "Expiring session %s (idle %ds)", g_sessions[i].id,
                         (int)difftime(now, g_sessions[i].last_active));
                session_destroy(&g_sessions[i]);
            }
        }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

session_t *find_session(const char *sid)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].active && strcmp(g_sessions[i].id, sid) == 0)
            return &g_sessions[i];
    }
    return NULL;
}

/* ── Persistence ─────────────────────────────────────────────────────────── */

static const char *sessions_dir(void)
{
    static char path[512];
    const char *home = getenv("HOME");
    if (!home) {
        /* Under sudo, HOME may be /root — try SUDO_USER */
        const char *sudo_user = getenv("SUDO_USER");
        if (sudo_user) {
            struct passwd *pw = getpwnam(sudo_user);
            if (pw)
                home = pw->pw_dir;
        }
    }
    if (!home)
        return NULL;
    snprintf(path, sizeof(path), "%s/.config/dnsclaw/sessions", home);
    return path;
}

int session_save(session_t *sess)
{
    if (!g_config.session_persist)
        return 0;

    const char *dir = sessions_dir();
    if (!dir)
        return -1;

    /* Ensure directory exists */
    char mkdir_path[512];
    snprintf(mkdir_path, sizeof(mkdir_path), "%s", dir);
    mkdir(mkdir_path, 0700);

    /* Also ensure parent exists */
    char *last_slash = strrchr(mkdir_path, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkdir(mkdir_path, 0700);
        *last_slash = '/';
        mkdir(mkdir_path, 0700);
    }

    /* Write to temp file, then rename (atomic) */
    char tmp_path[576], final_path[576];
    snprintf(final_path, sizeof(final_path), "%s/%s.json", dir, sess->id);
    snprintf(tmp_path, sizeof(tmp_path), "%s/.%s.tmp", dir, sess->id);

    char *json = cJSON_PrintUnformatted(sess->history);
    if (!json)
        return -1;

    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        free(json);
        return -1;
    }
    fputs(json, f);
    fclose(f);
    free(json);
    chmod(tmp_path, 0600);

    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    return 0;
}

int session_load(const char *sid, session_t *sess)
{
    const char *dir = sessions_dir();
    if (!dir)
        return -1;

    /* Validate session ID (hex only, prevent path traversal) */
    for (const char *p = sid; *p; p++) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
            return -1;
    }

    char path[576];
    snprintf(path, sizeof(path), "%s/%s.json", dir, sid);

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0 || fsize > 10 * 1024 * 1024) { /* 10MB limit */
        fclose(f);
        return -1;
    }
    fseek(f, 0, SEEK_SET);

    char *json = malloc((size_t)fsize + 1);
    if (!json) {
        fclose(f);
        return -1;
    }
    size_t nread = fread(json, 1, (size_t)fsize, f);
    fclose(f);
    json[nread] = '\0';

    cJSON *history = cJSON_Parse(json);
    free(json);

    if (!history || !cJSON_IsArray(history)) {
        cJSON_Delete(history);
        return -1;
    }

    /* Replace session history */
    if (sess->history)
        cJSON_Delete(sess->history);
    sess->history = history;
    return 0;
}

int session_list_saved(char ids[][32], time_t dates[], int max)
{
    const char *dir = sessions_dir();
    if (!dir)
        return 0;

    DIR *d = opendir(dir);
    if (!d)
        return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6 || strcmp(ent->d_name + nlen - 5, ".json") != 0)
            continue;
        if (ent->d_name[0] == '.')
            continue;

        /* Extract session ID (filename without .json) */
        size_t id_len = nlen - 5;
        if (id_len >= 32)
            id_len = 31;
        memcpy(ids[count], ent->d_name, id_len);
        ids[count][id_len] = '\0';

        /* Get modification time */
        char fpath[576];
        snprintf(fpath, sizeof(fpath), "%s/%s", dir, ent->d_name);
        struct stat st;
        dates[count] = (stat(fpath, &st) == 0) ? st.st_mtime : 0;

        count++;
    }
    closedir(d);
    return count;
}
