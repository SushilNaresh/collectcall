#include "prompt_mapping.h"

#include <pj/log.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define THIS_FILE "prompt_mapping.c"
#define CC_PROMPT_PATH_LEN 256

typedef struct {
    cc_prompt_tag_t tag;
    const char *name;
    const char *fallback_path;
    char path[CC_PROMPT_PATH_LEN];
} cc_prompt_entry_t;

static cc_prompt_entry_t g_prompts[CC_PROMPT_COUNT] = {
    { CC_PROMPT_WAITING, "WAITING", "wav/waiting.wav", "" },
    { CC_PROMPT_COLLECT_PROMPT, "COLLECT_PROMPT", "wav/collect_prompt.wav", "" },
    { CC_PROMPT_REJECTED, "REJECTED", "wav/rejected.wav", "" },
    { CC_PROMPT_UNAVAILABLE, "UNAVAILABLE", "wav/unavailable.wav", "" },
    { CC_PROMPT_LOW_BALANCE, "LOW_BALANCE", "wav/waiting.wav", "" },
    { CC_PROMPT_BUSY, "BUSY", "wav/rejected.wav", "" },
    { CC_PROMPT_NOT_AVAILABLE_TO_PAY, "NOT_AVAILABLE_TO_PAY", "wav/unavailable.wav", "" },
    { CC_PROMPT_MCA_SENT, "MCA_SENT", "wav/unavailable.wav", "" },
    { CC_PROMPT_MCA_NOT_SENT, "MCA_NOT_SENT", "wav/rejected.wav", "" },
    { CC_PROMPT_FUNDLESS, "FUNDLESS", "wav/0.1.wav", "" }
};

static void set_fallback_paths(void)
{
    int i;

    for (i = 0; i < CC_PROMPT_COUNT; i++) {
        snprintf(g_prompts[i].path,
                 sizeof(g_prompts[i].path),
                 "%s",
                 g_prompts[i].fallback_path);
    }
}

static char *trim(char *s)
{
    char *end;

    if (!s)
        return s;

    while (*s && isspace((unsigned char)*s))
        s++;

    if (*s == '\0')
        return s;

    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    return s;
}

static int find_prompt_index(const char *name)
{
    int i;

    if (!name)
        return -1;

    for (i = 0; i < CC_PROMPT_COUNT; i++) {
        if (strcmp(g_prompts[i].name, name) == 0)
            return i;
    }

    return -1;
}

static void set_prompt_path(int idx, const char *value)
{
    if (idx < 0 || idx >= CC_PROMPT_COUNT || !value || value[0] == '\0')
        return;

    if (strchr(value, '/')) {
        snprintf(g_prompts[idx].path,
                 sizeof(g_prompts[idx].path),
                 "%s",
                 value);
    } else {
        snprintf(g_prompts[idx].path,
                 sizeof(g_prompts[idx].path),
                 "wav/%s",
                 value);
    }
}

int cc_prompt_mapping_load(const char *path)
{
    FILE *fp;
    char line[512];
    int loaded = 0;

    if (!path || path[0] == '\0')
        path = "wav/wav_mapping.conf";

    set_fallback_paths();

    fp = fopen(path, "r");
    if (!fp) {
        PJ_LOG(2, (THIS_FILE,
                   "[PROMPT-MAP] Mapping file missing/unreadable, using fallback WAV files"));
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *p = trim(line);
        char *eq;
        char *key;
        char *value;
        int idx;

        if (*p == '\0' || *p == '#')
            continue;

        eq = strchr(p, '=');
        if (!eq)
            continue;

        *eq = '\0';
        key = trim(p);
        value = trim(eq + 1);

        if (*key == '\0' || *value == '\0')
            continue;

        idx = find_prompt_index(key);
        if (idx < 0) {
            PJ_LOG(4, (THIS_FILE,
                       "[PROMPT-MAP] Ignoring unknown prompt tag: %s",
                       key));
            continue;
        }

        set_prompt_path(idx, value);
        loaded++;
    }

    fclose(fp);

    PJ_LOG(3, (THIS_FILE,
               "[PROMPT-MAP] Loaded WAV mapping from %s",
               path));
    return loaded;
}

const char *cc_prompt_get_path(cc_prompt_tag_t tag)
{
    if (tag < 0 || tag >= CC_PROMPT_COUNT)
        tag = CC_PROMPT_WAITING;

    if (g_prompts[tag].path[0] == '\0')
        set_fallback_paths();

    return g_prompts[tag].path;
}

const char *cc_prompt_tag_name(cc_prompt_tag_t tag)
{
    if (tag < 0 || tag >= CC_PROMPT_COUNT)
        return "UNKNOWN";

    return g_prompts[tag].name;
}
