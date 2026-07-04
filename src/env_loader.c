/*
 * env_loader.c — Load key=value pairs from a file into the process environment.
 *
 * Supports:
 *   - Lines of the form KEY=VALUE
 *   - Quoted values: KEY="value" or KEY='value'
 *   - Comments (#) and blank lines are skipped
 *   - Does NOT override already-set env vars (real env takes precedence)
 */
#include "env_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pj/log.h>

#define THIS_FILE "env_loader.c"
#define MAX_LINE  1024

static char *trim(char *s)
{
    char *end;

    while (isspace((unsigned char)*s))
        s++;

    if (*s == '\0')
        return s;

    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        *end-- = '\0';

    return s;
}

static void strip_quotes(char *value)
{
    size_t len = strlen(value);

    if (len >= 2 &&
        ((value[0] == '"' && value[len - 1] == '"') ||
         (value[0] == '\'' && value[len - 1] == '\'')))
    {
        memmove(value, value + 1, len - 2);
        value[len - 2] = '\0';
    }
}

int cc_load_env_file(const char *path)
{
    FILE *fp;
    char line[MAX_LINE];
    int loaded = 0;

    if (!path || path[0] == '\0')
        return 0;

    fp = fopen(path, "r");
    if (!fp)
        return -1;

    while (fgets(line, sizeof(line), fp)) {
        char *trimmed = trim(line);
        char *eq;
        char *key;
        char *value;

        if (trimmed[0] == '\0' || trimmed[0] == '#')
            continue;

        eq = strchr(trimmed, '=');
        if (!eq)
            continue;

        *eq = '\0';
        key = trim(trimmed);
        value = trim(eq + 1);

        if (key[0] == '\0')
            continue;

        strip_quotes(value);

        /* Do not override existing env vars */
        if (getenv(key) != NULL)
            continue;

        if (setenv(key, value, 0) == 0)
            loaded++;
    }

    fclose(fp);

    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] loaded %d env vars from %s",
               loaded, path));

    return loaded;
}
