#include "runtime_config.h"
#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *env_nonempty(const char *name)
{
    const char *value = getenv(name);
    return (value && value[0] != '\0') ? value : NULL;
}

static int parse_port_env(const char *name, int fallback)
{
    const char *value = env_nonempty(name);
    char *end = NULL;
    long parsed;

    if (!value)
        return fallback;

    parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed <= 0 || parsed > 65535)
        return fallback;

    return (int)parsed;
}

static int split_host_port(const char *input,
                           char *host,
                           size_t host_len,
                           int *port)
{
    const char *colon;
    size_t len;
    char *end = NULL;
    long parsed;

    if (!input || !host || host_len == 0 || !port)
        return 0;

    colon = strrchr(input, ':');
    if (!colon || colon == input || colon[1] == '\0')
        return 0;

    len = (size_t)(colon - input);
    if (len >= host_len)
        return 0;

    parsed = strtol(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || parsed <= 0 || parsed > 65535)
        return 0;

    memcpy(host, input, len);
    host[len] = '\0';
    *port = (int)parsed;
    return 1;
}

const char *cc_cfg_local_host(void)
{
    const char *value = env_nonempty("CC_LOCAL_HOST");
    return value ? value : CC_LOCAL_HOST;
}

int cc_cfg_local_sip_port(void)
{
    return parse_port_env("CC_LOCAL_SIP_PORT", CC_LOCAL_SIP_PORT);
}

const char *cc_cfg_sbc_host(void)
{
    const char *value = env_nonempty("CC_SBC_HOST");
    static char fallback_host[128];
    static int fallback_ready = 0;
    int fallback_port;

    if (value)
        return value;

    if (!fallback_ready) {
        if (!split_host_port(CC_SIP_DOMAIN,
                             fallback_host,
                             sizeof(fallback_host),
                             &fallback_port))
        {
            snprintf(fallback_host, sizeof(fallback_host), "%s", CC_LOCAL_HOST);
        }
        fallback_ready = 1;
    }

    return fallback_host;
}

int cc_cfg_sbc_port(void)
{
    int fallback_port = 5060;
    char fallback_host[128];

    (void)split_host_port(CC_SIP_DOMAIN,
                          fallback_host,
                          sizeof(fallback_host),
                          &fallback_port);

    return parse_port_env("CC_SBC_PORT", fallback_port);
}

const char *cc_cfg_sbc_next_hop(void)
{
    static char next_hop[192];

    snprintf(next_hop,
             sizeof(next_hop),
             "%s:%d",
             cc_cfg_sbc_host(),
             cc_cfg_sbc_port());
    return next_hop;
}

const char *cc_cfg_sbc_route2(void)
{
    return env_nonempty("CC_SBC_ROUTE2");
}

const char *cc_cfg_collect_prefixes(void)
{
    const char *value = env_nonempty("CC_COLLECT_PREFIXES");

    if (value)
        return value;

    value = env_nonempty("CC_COLLECT_PREFIX");
    return value ? value : CC_COLLECT_PREFIX;
}

const char *cc_cfg_default_country_code(void)
{
    const char *value = env_nonempty("CC_DEFAULT_COUNTRY_CODE");
    return value ? value : CC_DEFAULT_COUNTRY_CODE;
}

static int str_eq_ci(const char *a, const char *b)
{
    if (!a || !b)
        return 0;

    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

cc_prefix_mode_t cc_cfg_prefix_mode(void)
{
    const char *value = env_nonempty("CC_PREFIX_MODE");

    if (str_eq_ci(value, "allow_already_stripped"))
        return CC_PREFIX_MODE_ALLOW_ALREADY_STRIPPED;

    return CC_PREFIX_MODE_STRIP_REQUIRED;
}

const char *cc_cfg_prefix_mode_name(void)
{
    return cc_cfg_prefix_mode() == CC_PREFIX_MODE_ALLOW_ALREADY_STRIPPED
           ? "allow_already_stripped"
           : "strip_required";
}

cc_service_key_mode_t cc_cfg_service_key_mode(void)
{
    const char *value = env_nonempty("CC_SERVICE_KEY_MODE");

    if (str_eq_ci(value, "from_only"))
        return CC_SERVICE_KEY_MODE_FROM_ONLY;
    if (str_eq_ci(value, "request_uri"))
        return CC_SERVICE_KEY_MODE_REQUEST_URI;
    if (str_eq_ci(value, "request_uri_and_from"))
        return CC_SERVICE_KEY_MODE_REQUEST_URI_AND_FROM;

#if CC_SERVICE_KEY_PREPEND_ENABLE
    return CC_SERVICE_KEY_MODE_REQUEST_URI_AND_FROM;
#else
    return CC_SERVICE_KEY_MODE_DISABLED;
#endif
}

const char *cc_cfg_service_key_mode_name(void)
{
    switch (cc_cfg_service_key_mode()) {
    case CC_SERVICE_KEY_MODE_FROM_ONLY:
        return "from_only";
    case CC_SERVICE_KEY_MODE_REQUEST_URI:
        return "request_uri";
    case CC_SERVICE_KEY_MODE_REQUEST_URI_AND_FROM:
        return "request_uri_and_from";
    case CC_SERVICE_KEY_MODE_DISABLED:
    default:
        return "disabled";
    }
}

const char *cc_cfg_service_key_placeholder(void)
{
    const char *value = env_nonempty("CC_SERVICE_KEY_PLACEHOLDER");
    return value ? value : CC_SERVICE_KEY_PLACEHOLDER;
}

const char *cc_cfg_validation_host(void)
{
    const char *value = env_nonempty("CC_VALIDATION_HOST");
    return value ? value : CC_VALIDATION_UDP_HOST;
}

int cc_cfg_validation_port(void)
{
    return parse_port_env("CC_VALIDATION_PORT", CC_VALIDATION_UDP_PORT);
}

const char *cc_cfg_endcall_host(void)
{
    const char *value = env_nonempty("CC_ENDCALL_HOST");
    return value ? value : CC_CALL_END_UDP_HOST;
}

int cc_cfg_endcall_port(void)
{
    return parse_port_env("CC_ENDCALL_PORT", CC_CALL_END_UDP_PORT);
}

const char *cc_cfg_pani_value(void)
{
    const char *value = env_nonempty("CC_PANI_VALUE");
    return value ? value : CC_BLEG_STATIC_PANI;
}

const char *cc_cfg_user_agent(void)
{
    const char *value = env_nonempty("CC_USER_AGENT");
    return value ? value : CC_USER_AGENT;
}

cc_media_mode_t cc_cfg_media_mode(void)
{
    const char *value = env_nonempty("CC_MEDIA_MODE");

    if (str_eq_ci(value, "local_bridge"))
        return CC_MEDIA_MODE_LOCAL_BRIDGE;
    if (str_eq_ci(value, "update"))
        return CC_MEDIA_MODE_UPDATE;
    if (str_eq_ci(value, "reinvite") || str_eq_ci(value, "re-invite"))
        return CC_MEDIA_MODE_REINVITE;
    if (str_eq_ci(value, "rtpengine"))
        return CC_MEDIA_MODE_RTPENGINE;

#if CC_MEDIA_CHANGE_USE_REINVITE
    return CC_MEDIA_MODE_REINVITE;
#else
    return CC_MEDIA_MODE_UPDATE;
#endif
}

const char *cc_cfg_media_mode_name(void)
{
    switch (cc_cfg_media_mode()) {
    case CC_MEDIA_MODE_LOCAL_BRIDGE:
        return "local_bridge";
    case CC_MEDIA_MODE_REINVITE:
        return "reinvite";
    case CC_MEDIA_MODE_RTPENGINE:
        return "rtpengine";
    case CC_MEDIA_MODE_UPDATE:
    default:
        return "update";
    }
}

int cc_cfg_media_uses_update(void)
{
    /* rtpengine does not use post-accept SIP UPDATE hairpin bypass. */
    return cc_cfg_media_mode() == CC_MEDIA_MODE_UPDATE;
}

const char *cc_cfg_rtpengine_host(void)
{
    const char *value = env_nonempty("CC_RTPENGINE_HOST");
    return value ? value : CC_RTPENGINE_HOST;
}

int cc_cfg_rtpengine_port(void)
{
    return parse_port_env("CC_RTPENGINE_PORT", CC_RTPENGINE_PORT);
}

int cc_cfg_rtpengine_timeout_ms(void)
{
    const char *value = env_nonempty("CC_RTPENGINE_TIMEOUT_MS");
    char *end = NULL;
    long parsed;

    if (!value)
        return CC_RTPENGINE_TIMEOUT_MS;

    parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 50 || parsed > 5000)
        return CC_RTPENGINE_TIMEOUT_MS;
    return (int)parsed;
}

const char *cc_cfg_rtpengine_flags(void)
{
    const char *value = env_nonempty("CC_RTPENGINE_FLAGS");
    return value ? value : CC_RTPENGINE_FLAGS;
}

int cc_cfg_rtpengine_dtmf_port(void)
{
    const char *full = env_nonempty("CC_RTPENGINE_DTMF_DEST");
    int port;
    const char *colon;

    /* Full override "host:port" wins over PORT alone. */
    if (full) {
        colon = strrchr(full, ':');
        if (colon && colon[1]) {
            char *end = NULL;
            long parsed = strtol(colon + 1, &end, 10);
            if (end != colon + 1 && *end == '\0' &&
                parsed > 0 && parsed <= 65535)
                return (int)parsed;
        }
    }

    port = parse_port_env("CC_RTPENGINE_DTMF_PORT", CC_RTPENGINE_DTMF_PORT);

    /*
     * Only rewrite when DTMF port equals THIS instance's SIP port or the
     * ng control port. Per-instance DTMF 8061 with SIP 9061 is fine.
     */
    if (port == cc_cfg_local_sip_port() || port == cc_cfg_rtpengine_port()) {
        fprintf(stderr,
                "[CONFIG] CC_RTPENGINE_DTMF_PORT=%d collides with SIP/ng — "
                "using default %d\n",
                port, CC_RTPENGINE_DTMF_PORT);
        return CC_RTPENGINE_DTMF_PORT;
    }
    return port;
}

const char *cc_cfg_rtpengine_dtmf_dest(void)
{
    static char dest[160];
    static int logged = 0;
    const char *full = env_nonempty("CC_RTPENGINE_DTMF_DEST");
    const char *host = env_nonempty("CC_RTPENGINE_DTMF_HOST");
    int port = cc_cfg_rtpengine_dtmf_port();
    const char *re_host;

    if (full && full[0] != '\0') {
        const char *colon = strrchr(full, ':');
        if (colon && colon > full) {
            size_t hlen = (size_t)(colon - full);
            if (hlen >= sizeof(dest))
                hlen = sizeof(dest) - 1;
            memcpy(dest, full, hlen);
            dest[hlen] = '\0';
            snprintf(dest + hlen, sizeof(dest) - hlen, ":%d", port);
        } else {
            snprintf(dest, sizeof(dest), "%s", full);
        }
    } else {
        /*
         * When RTPengine is local (127.0.0.1), DTMF UDP must also target
         * loopback so notifies reach this process's DTMF listener
         * (e.g. 8061 for SIP instance 9061).
         */
        if (!host) {
            re_host = cc_cfg_rtpengine_host();
            if (re_host &&
                (strcmp(re_host, "127.0.0.1") == 0 ||
                 strcmp(re_host, "localhost") == 0 ||
                 strcmp(re_host, "::1") == 0))
                host = "127.0.0.1";
            else
                host = cc_cfg_local_host();
        }
        snprintf(dest, sizeof(dest), "%s:%d", host, port);
    }

    if (!logged) {
        logged = 1;
        fprintf(stderr,
                "[CONFIG] rtpengine DTMF notify dest=%s "
                "(listener must bind this port; unique per instance)\n",
                dest);
    }
    return dest;
}

const char *cc_cfg_rtpengine_media_dir(void)
{
    const char *value = env_nonempty("CC_RTPENGINE_MEDIA_DIR");
    if (value)
        return value;
    return CC_RTPENGINE_MEDIA_DIR;
}

int cc_cfg_free_period_ms(void)
{
    const char *value = env_nonempty("CC_FREE_PERIOD_MS");
    char *end = NULL;
    long parsed;

    if (!value)
        return CC_FREE_PERIOD_MS;

    parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 0 || parsed > 300000)
        return CC_FREE_PERIOD_MS;

    return (int)parsed;
}

const char *cc_cfg_fundless_prefixes(void)
{
    const char *value = env_nonempty("CC_FUNDLESS_PREFIXES");
    return value ? value : "";
}

int cc_cfg_is_fundless_prefix(const char *prefix)
{
    const char *list = cc_cfg_fundless_prefixes();
    size_t plen;

    if (!prefix || prefix[0] == '\0' || list[0] == '\0')
        return 0;

    plen = strlen(prefix);

    const char *p = list;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t seg_len = comma ? (size_t)(comma - p) : strlen(p);

        if (seg_len == plen && strncmp(p, prefix, plen) == 0)
            return 1;

        p += seg_len;
        if (*p == ',')
            p++;
    }

    return 0;
}

int cc_cfg_validation_timeout_ms(void)
{
    const char *value = env_nonempty("CC_VALIDATION_TIMEOUT_MS");
    char *end = NULL;
    long parsed;

    if (!value)
        return CC_VALIDATION_TIMEOUT_MS;

    parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 500 || parsed > 30000)
        return CC_VALIDATION_TIMEOUT_MS;

    return (int)parsed;
}

int cc_cfg_b_dtmf_timeout_sec(void)
{
    const char *value = env_nonempty("CC_B_DTMF_TIMEOUT_SEC");
    char *end = NULL;
    long parsed;

    if (!value)
        return CC_B_DTMF_TIMEOUT_SEC;

    parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 5 || parsed > 300)
        return CC_B_DTMF_TIMEOUT_SEC;

    return (int)parsed;
}

/* Comma-separated list of valid prefixes that may appear before the
 * 10-digit B number. Default: "0,234,313".
 * Override: export CC_B_NUMBER_PREFIXES=0,234,313
 */
const char *cc_cfg_b_number_prefixes(void)
{
    const char *value = env_nonempty("CC_B_NUMBER_PREFIXES");
    return value ? value : "0,234,313";
}

int cc_cfg_rtp_port_start(void)
{
    const char *value = env_nonempty("CC_RTP_PORT_START");
    char *end = NULL;
    long parsed;

    if (!value)
        return CC_RTP_PORT_START;

    parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 1024 || parsed > 65000)
        return CC_RTP_PORT_START;

    return (int)parsed;
}

int cc_cfg_rtp_port_count(void)
{
    const char *value = env_nonempty("CC_RTP_PORT_COUNT");
    char *end = NULL;
    long parsed;

    if (!value)
        return CC_RTP_PORT_COUNT;

    parsed = strtol(value, &end, 10);
    /* must be even (RTP+RTCP pairs), at least 2.
     * Upper bound: port_start + port_count must not exceed 65535.
     * Validated against port_start at runtime in main.c [CONFIG] log.
     * Allow up to 64000 to accommodate 54000 and similar large ranges. */
    if (end == value || *end != '\0' || parsed < 2 || parsed > 64000)
        return CC_RTP_PORT_COUNT;

    /* round down to even so every port has an RTCP partner */
    if (parsed % 2 != 0)
        parsed--;

    return (int)parsed;
}

int cc_cfg_max_calls(void)
{
    const char *value = env_nonempty("CC_MAX_CALLS");
    char *end = NULL;
    long parsed;

    if (!value)
        return CC_MAX_CALLS;

    parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 2 || parsed > PJSUA_MAX_CALLS)
        return CC_MAX_CALLS;

    return (int)parsed;
}

/* Soft cap: keep one instance under ~100 CPS with short A treatment hold. */
#ifndef CC_ADMISSION_MAX_CALLS_DEFAULT
#define CC_ADMISSION_MAX_CALLS_DEFAULT  600
#endif
#ifndef CC_ADMISSION_TIMER_HEAP_DEFAULT
#define CC_ADMISSION_TIMER_HEAP_DEFAULT 3000
#endif

int cc_cfg_admission_max_calls(void)
{
    const char *value = env_nonempty("CC_ADMISSION_MAX_CALLS");
    char *end = NULL;
    long parsed;

    if (!value)
        return CC_ADMISSION_MAX_CALLS_DEFAULT;

    parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 0 || parsed > PJSUA_MAX_CALLS)
        return CC_ADMISSION_MAX_CALLS_DEFAULT;

    return (int)parsed;
}

int cc_cfg_admission_timer_heap_max(void)
{
    const char *value = env_nonempty("CC_ADMISSION_TIMER_HEAP_MAX");
    char *end = NULL;
    long parsed;

    if (!value)
        return CC_ADMISSION_TIMER_HEAP_DEFAULT;

    parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 0 || parsed > 1000000)
        return CC_ADMISSION_TIMER_HEAP_DEFAULT;

    return (int)parsed;
}

int cc_cfg_log_level(void)
{
    const char *value = env_nonempty("CC_LOG_LEVEL");
    char *end = NULL;
    long parsed;

    if (!value)
        return CC_LOG_LEVEL;

    parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 0 || parsed > 6)
        return CC_LOG_LEVEL;

    return (int)parsed;
}
