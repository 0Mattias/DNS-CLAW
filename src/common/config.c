/*
 * config.c — Shared .env file loader.
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void load_dotenv(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        char *ke = eq - 1;
        while (ke > key && (*ke == ' ' || *ke == '\t')) *ke-- = '\0';
        while (*val == ' ' || *val == '\t' || *val == '"') val++;
        /* Strip inline comments (unquoted # after value) */
        {
            char *hash = strchr(val, '#');
            if (hash && hash > val && (hash[-1] == ' ' || hash[-1] == '\t'))
                *hash = '\0';
        }
        size_t vlen = strlen(val);
        while (vlen > 0 && (val[vlen-1] == '\n' || val[vlen-1] == '\r' ||
               val[vlen-1] == '"' || val[vlen-1] == ' '))
            val[--vlen] = '\0';
        setenv(key, val, 0); /* Don't overwrite existing */
    }
    fclose(f);
}
