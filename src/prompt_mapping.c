#include "prompt_mapping.h"
#include "runtime_config.h"

#include <pj/log.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    { CC_PROMPT_FUNDLESS, "FUNDLESS", "wav/0.1.wav", "" },
    { CC_PROMPT_DIAL_TONE, "DIAL_TONE", "wav/dial_tone.wav", "" },
    { CC_PROMPT_MOH, "MOH_PROMPT", "wav/4.wav", "" },
    { CC_PROMPT_B_CONNECTED, "B_CONNECTED_PROMPT", "wav/4.1.wav", "" },
    { CC_PROMPT_INCOMPLETE_NUMBER, "INCOMPLETE_NUMBER", "wav/4.2.wav", "" }
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

#define CC_WAV_CACHE_MAX CC_PROMPT_COUNT

static cc_wav_pcm_t g_wav_cache[CC_WAV_CACHE_MAX];
static char g_wav_cache_path[CC_WAV_CACHE_MAX][CC_PROMPT_PATH_LEN];
static int g_wav_cache_count;

static uint16_t read_le16(const unsigned char *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int cache_find_path(const char *path)
{
    int i;

    for (i = 0; i < g_wav_cache_count; i++) {
        if (strcmp(g_wav_cache_path[i], path) == 0)
            return i;
    }
    return -1;
}

static int load_wav_file(const char *path, cc_wav_pcm_t *out)
{
    FILE *fp;
    unsigned char riff[12];
    unsigned char chunk_hdr[8];
    unsigned char fmt[16];
    uint16_t audio_format = 0, channels = 0, bits = 0;
    uint32_t sample_rate = 0;
    uint32_t data_bytes = 0;
    unsigned char *data = NULL;
    int have_fmt = 0;
    int have_data = 0;

    memset(out, 0, sizeof(*out));

    fp = fopen(path, "rb");
    if (!fp)
        return -1;

    if (fread(riff, 1, 12, fp) != 12 ||
        memcmp(riff, "RIFF", 4) != 0 ||
        memcmp(riff + 8, "WAVE", 4) != 0)
    {
        fclose(fp);
        return -1;
    }

    while (fread(chunk_hdr, 1, 8, fp) == 8) {
        uint32_t chunk_size = read_le32(chunk_hdr + 4);
        uint32_t skip;

        if (memcmp(chunk_hdr, "fmt ", 4) == 0) {
            if (chunk_size < 16 || fread(fmt, 1, 16, fp) != 16) {
                fclose(fp);
                return -1;
            }
            audio_format = read_le16(fmt);
            channels = read_le16(fmt + 2);
            sample_rate = read_le32(fmt + 4);
            bits = read_le16(fmt + 14);
            skip = chunk_size - 16;
            have_fmt = 1;
        } else if (memcmp(chunk_hdr, "data", 4) == 0) {
            data_bytes = chunk_size;
            data = (unsigned char *)malloc(data_bytes ? data_bytes : 1);
            if (!data || (data_bytes && fread(data, 1, data_bytes, fp) != data_bytes)) {
                free(data);
                fclose(fp);
                return -1;
            }
            skip = (chunk_size & 1) ? 1 : 0;
            have_data = 1;
        } else {
            skip = chunk_size;
        }

        if (skip) {
            if ((skip & 1) && skip != UINT32_MAX)
                skip++;
            if (fseek(fp, (long)skip, SEEK_CUR) != 0) {
                free(data);
                fclose(fp);
                return -1;
            }
        }

        if (have_fmt && have_data)
            break;
    }

    fclose(fp);

    if (!have_fmt || !have_data || audio_format != 1 ||
        channels == 0 || sample_rate == 0 ||
        (bits != 8 && bits != 16))
    {
        free(data);
        return -1;
    }

    if (bits == 8) {
        unsigned i;
        int16_t *pcm16 = (int16_t *)malloc(data_bytes * 2);
        if (!pcm16) {
            free(data);
            return -1;
        }
        for (i = 0; i < data_bytes; i++)
            pcm16[i] = (int16_t)(((int)data[i] - 128) << 8);
        free(data);
        out->pcm = pcm16;
        out->nbytes = data_bytes * 2;
        out->bits_per_sample = 16;
    } else {
        out->pcm = data;
        out->nbytes = data_bytes;
        out->bits_per_sample = 16;
    }

    out->clock_rate = sample_rate;
    out->channel_count = channels;
    {
        unsigned bytes_per_sample = (out->bits_per_sample / 8) * channels;
        unsigned samples;

        if (bytes_per_sample == 0) {
            free((void *)out->pcm);
            memset(out, 0, sizeof(*out));
            return -1;
        }
        samples = out->nbytes / bytes_per_sample;
        out->duration_ms = (int)((long long)samples * 1000 / sample_rate);
        if (out->duration_ms <= 0)
            out->duration_ms = 4000;
    }
    return 0;
}

static void cc_prompt_cache_load_files(void)
{
    int i;
    int loaded = 0;
    size_t bytes = 0;

    for (i = 0; i < CC_PROMPT_COUNT && g_wav_cache_count < CC_WAV_CACHE_MAX; i++) {
        const char *path = g_prompts[i].path;
        cc_wav_pcm_t pcm;

        if (!path || path[0] == '\0')
            continue;
        if (cache_find_path(path) >= 0)
            continue;
        if (load_wav_file(path, &pcm) != 0) {
            PJ_LOG(2, (THIS_FILE,
                       "[PROMPT-CACHE] skip (not PCM or missing): %s", path));
            continue;
        }
        snprintf(g_wav_cache_path[g_wav_cache_count],
                 sizeof(g_wav_cache_path[g_wav_cache_count]),
                 "%s", path);
        g_wav_cache[g_wav_cache_count] = pcm;
        g_wav_cache_count++;
        loaded++;
        bytes += pcm.nbytes;
    }

    PJ_LOG(3, (THIS_FILE,
               "[PROMPT-CACHE] loaded %d unique WAV(s) into memory (%zu KB)",
               loaded, bytes / 1024));
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
        if (cc_cfg_media_mode() != CC_MEDIA_MODE_RTPENGINE)
            cc_prompt_cache_load_files();
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
    {
        int i;
        const char *media_dir = cc_cfg_rtpengine_media_dir();
        for (i = 0; i < CC_PROMPT_COUNT; i++) {
            char check[CC_PROMPT_PATH_LEN];
            const char *p = g_prompts[i].path;
            const char *base;

            if (!p || p[0] == '\0')
                continue;
            if (media_dir && media_dir[0] != '\0' &&
                cc_cfg_media_mode() == CC_MEDIA_MODE_RTPENGINE)
            {
                base = strrchr(p, '/');
                base = base ? base + 1 : p;
                snprintf(check, sizeof(check), "%s/%s", media_dir, base);
            } else {
                snprintf(check, sizeof(check), "%s", p);
            }
            if (access(check, R_OK) != 0) {
                PJ_LOG(1, (THIS_FILE,
                           "[PROMPT-MAP] MISSING file for %s → %s "
                           "(fix before MCA_SENT/MCA_NOT_SENT can play)",
                           g_prompts[i].name, check));
            }
        }
    }
    if (cc_cfg_media_mode() == CC_MEDIA_MODE_RTPENGINE)
        PJ_LOG(3, (THIS_FILE,
                   "[PROMPT-MAP] skip PCM cache — RTPengine plays files"));
    else
        cc_prompt_cache_load_files();
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

const cc_wav_pcm_t *cc_prompt_cache_get(const char *path)
{
    int idx;

    if (!path || path[0] == '\0')
        return NULL;

    idx = cache_find_path(path);
    if (idx < 0)
        return NULL;

    return &g_wav_cache[idx];
}
