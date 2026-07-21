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
    case CC_MEDIA_MODE_UPDATE:
    default:
        return "update";
    }
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
