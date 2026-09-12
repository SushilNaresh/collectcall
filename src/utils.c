/*
 * utils.c — SDP, SIP header, URI and media helpers
 * RTP bypass watchdog posted to worker pool.
 */
#include "utils.h"
#include "config.h"
#include <pthread.h>
#include <limits.h>
#include "api_mapping.h"
#include "runtime_config.h"
#include "rtpengine.h"
#include "worker.h"
#include "prompt_mapping.h"

#include <pjsua-lib/pjsua.h>
#include <pjsua-lib/pjsua_internal.h>  /* struct pjsua_data/pjsua_call/
                                        * pjsua_call_media, extern pjsua_var —
                                        * needed for cc_get_call_aud_stream().
                                        * Requires building against pjproject
                                        * source tree (not just installed
                                        * pjsua-lib headers/.so), since this
                                        * header isn't part of the public API. */
#include <pjmedia/sdp.h>
#include <pjmedia/stream.h>
#include <pjmedia/echo.h>
#include <pjmedia/mem_port.h>
#include <pjmedia/jbuf.h>
#include <pjmedia/wav_port.h>
#include <pjmedia/port.h>
#include <pjsip/sip_msg.h>
#include <pjsip/sip_uri.h>
#include <pjsip/sip_util.h>
#include <pj/string.h>
#include <pj/log.h>

#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define THIS_FILE "utils.c"

static const char *cc_strcasestr_local(const char *haystack,
                                       const char *needle)
{
    size_t needle_len;

    if (!haystack || !needle || needle[0] == '\0')
        return haystack;

    needle_len = strlen(needle);
    while (*haystack) {
        if (strncasecmp(haystack, needle, needle_len) == 0)
            return haystack;
        haystack++;
    }

    return NULL;
}

/* ── URI / number ─────────────────────────────────────────────────────────── */

static int cc_prefix_is_valid(const char *prefix)
{
    size_t i;
    size_t len;

    if (!prefix || prefix[0] == '\0')
        return 0;

    len = strlen(prefix);
    if (len >= 32)
        return 0;

    for (i = 0; i < len; i++) {
        if (prefix[i] < '0' || prefix[i] > '9')
            return 0;
    }

    return 1;
}

static char *cc_trim_in_place(char *s)
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

static int cc_copy_digits(const char *src, char *dst, pj_size_t dst_len)
{
    pj_size_t used = 0;

    if (!src || !dst || dst_len == 0)
        return 0;

    while (*src) {
        unsigned char c = (unsigned char)*src++;
        if (isdigit(c)) {
            if (used + 1 >= dst_len)
                return 0;
            dst[used++] = (char)c;
        }
    }

    dst[used] = '\0';
    return used > 0;
}

static int cc_digits_start_with(const char *value, const char *prefix)
{
    size_t plen;

    if (!value || !prefix || prefix[0] == '\0')
        return 0;

    plen = strlen(prefix);
    return strncmp(value, prefix, plen) == 0;
}

const char *cc_collect_prefix(void)
{
    const char *prefixes = cc_cfg_collect_prefixes();
    static char first_prefix[32];
    char copy[256];
    char *token;
    char *saveptr = NULL;

    first_prefix[0] = '\0';

    if (!prefixes || prefixes[0] == '\0')
        return CC_COLLECT_PREFIX;

    snprintf(copy, sizeof(copy), "%s", prefixes);
    token = strtok_r(copy, ",", &saveptr);
    while (token) {
        token = cc_trim_in_place(token);
        if (cc_prefix_is_valid(token)) {
            snprintf(first_prefix, sizeof(first_prefix), "%s", token);
            return first_prefix;
        }
        token = strtok_r(NULL, ",", &saveptr);
    }

    return CC_COLLECT_PREFIX;
}

pj_bool_t cc_collect_prefix_is_env_override(void)
{
    return (getenv("CC_COLLECT_PREFIXES") || getenv("CC_COLLECT_PREFIX")) ?
           PJ_TRUE : PJ_FALSE;
}

pj_status_t cc_extract_uri_user(const char *identity,
                                char *user,
                                pj_size_t user_len)
{
    const char *start;
    const char *end;
    const char *scheme;
    pj_size_t len;

    if (!identity || !user || user_len == 0)
        return PJ_EINVAL;

    user[0] = '\0';

    scheme = cc_strcasestr_local(identity, "sips:");
    if (scheme) {
        start = scheme + 5;
    } else {
        scheme = cc_strcasestr_local(identity, "sip:");
        if (scheme) {
            start = scheme + 4;
        } else {
            scheme = cc_strcasestr_local(identity, "tel:");
            start = scheme ? scheme + 4 : identity;
        }
    }

    while (*start == ' ' || *start == '\t' || *start == '<' ||
           *start == '"' || *start == '\'')
    {
        start++;
    }

    end = start;
    while (*end != '\0' &&
           *end != '@' &&
           *end != ';' &&
           *end != '>' &&
           *end != '?' &&
           *end != ',' &&
           *end != ' ' &&
           *end != '\t' &&
           *end != '\r' &&
           *end != '\n' &&
           *end != '"')
    {
        end++;
    }

    len = (pj_size_t)(end - start);
    if (len == 0)
        return PJ_ENOTFOUND;
    if (len >= user_len)
        return PJ_ETOOSMALL;

    memcpy(user, start, len);
    user[len] = '\0';
    return PJ_SUCCESS;
}

pj_status_t cc_extract_request_uri_user(pjsip_rx_data *rdata,
                                        char *raw_uri,
                                        pj_size_t raw_uri_len,
                                        char *user,
                                        pj_size_t user_len)
{
    pjsip_msg *msg;
    int printed;

    if (raw_uri && raw_uri_len > 0)
        raw_uri[0] = '\0';
    if (user && user_len > 0)
        user[0] = '\0';

    if (!rdata || !rdata->msg_info.msg || !raw_uri || raw_uri_len == 0 ||
        !user || user_len == 0)
    {
        return PJ_EINVAL;
    }

    msg = rdata->msg_info.msg;
    if (msg->type != PJSIP_REQUEST_MSG || !msg->line.req.uri)
        return PJ_ENOTFOUND;

    printed = pjsip_uri_print(PJSIP_URI_IN_REQ_URI,
                              msg->line.req.uri,
                              raw_uri,
                              raw_uri_len);
    if (printed < 1 || (pj_size_t)printed >= raw_uri_len) {
        raw_uri[0] = '\0';
        return PJ_ETOOSMALL;
    }

    raw_uri[printed] = '\0';
    return cc_extract_uri_user(raw_uri, user, user_len);
}

pj_status_t cc_extract_to_header_user(pjsip_rx_data *rdata,
                                      char *user,
                                      pj_size_t user_len)
{
    pjsip_msg *msg;
    pjsip_to_hdr *to;
    char uri_buf[512];
    int printed;

    if (user && user_len > 0)
        user[0] = '\0';

    if (!rdata || !rdata->msg_info.msg || !user || user_len == 0)
        return PJ_EINVAL;

    msg = rdata->msg_info.msg;
    to = (pjsip_to_hdr *)pjsip_msg_find_hdr(msg, PJSIP_H_TO, NULL);
    if (!to || !to->uri)
        return PJ_ENOTFOUND;

    printed = pjsip_uri_print(PJSIP_URI_IN_FROMTO_HDR,
                              to->uri,
                              uri_buf,
                              sizeof(uri_buf));
    if (printed < 1 || (size_t)printed >= sizeof(uri_buf))
        return PJ_ETOOSMALL;

    uri_buf[printed] = '\0';
    return cc_extract_uri_user(uri_buf, user, user_len);
}

pj_status_t cc_extract_diversion_user(pjsip_msg *msg,
                                      char *user,
                                      pj_size_t user_len)
{
    pj_str_t hdr_name = pj_str("Diversion");
    pjsip_generic_string_hdr *hdr;
    char value_buf[512];
    pj_size_t vlen;

    if (!msg || !user || user_len == 0)
        return PJ_EINVAL;

    user[0] = '\0';

    hdr = (pjsip_generic_string_hdr *)
          pjsip_msg_find_hdr_by_name(msg, &hdr_name, NULL);
    if (!hdr || hdr->hvalue.slen == 0)
        return PJ_ENOTFOUND;

    vlen = (pj_size_t)hdr->hvalue.slen;
    if (vlen >= sizeof(value_buf))
        vlen = sizeof(value_buf) - 1;

    memcpy(value_buf, hdr->hvalue.ptr, vlen);
    value_buf[vlen] = '\0';

    return cc_extract_uri_user(value_buf, user, user_len);
}

pj_status_t cc_extract_pj_uri_user(const pj_str_t *uri,
                                   char *user,
                                   pj_size_t user_len)
{
    char buf[512];
    pj_size_t len;

    if (!uri || !uri->ptr || !user || user_len == 0)
        return PJ_EINVAL;

    user[0] = '\0';
    len = (pj_size_t)uri->slen;
    if (len >= sizeof(buf))
        len = sizeof(buf) - 1;

    memcpy(buf, uri->ptr, len);
    buf[len] = '\0';
    return cc_extract_uri_user(buf, user, user_len);
}

pj_status_t cc_split_collect_number(const char *dialed_raw,
                                    cc_collect_number_t *result)
{
    const char *prefixes = cc_cfg_collect_prefixes();
    char prefixes_copy[256];
    char dialed_digits[128];
    char *token;
    char *saveptr = NULL;
    char best_prefix[32] = "";
    size_t best_len = 0;

    if (!dialed_raw || !result)
        return PJ_EINVAL;

    memset(result, 0, sizeof(*result));

    if (!cc_copy_digits(dialed_raw,
                        dialed_digits,
                        sizeof(dialed_digits)))
    {
        return PJ_ENOTFOUND;
    }

    snprintf(result->dialed_digits,
             sizeof(result->dialed_digits),
             "%s",
             dialed_digits);

    snprintf(prefixes_copy,
             sizeof(prefixes_copy),
             "%s",
             prefixes ? prefixes : "");

    token = strtok_r(prefixes_copy, ",", &saveptr);
    while (token) {
        token = cc_trim_in_place(token);
        if (cc_prefix_is_valid(token) &&
            cc_digits_start_with(dialed_digits, token) &&
            strlen(token) > best_len)
        {
            best_len = strlen(token);
            snprintf(best_prefix, sizeof(best_prefix), "%s", token);
        }
        token = strtok_r(NULL, ",", &saveptr);
    }

    if (best_len > 0) {
        const char *sponsor = dialed_digits + best_len;

        if (sponsor[0] == '\0')
            return PJ_ENOTFOUND;

        snprintf(result->matched_prefix,
                 sizeof(result->matched_prefix),
                 "%s",
                 best_prefix);
        snprintf(result->sponsor_raw,
                 sizeof(result->sponsor_raw),
                 "%s",
                 sponsor);
        result->prefix_matched = 1;
        return PJ_SUCCESS;
    }

    if (cc_cfg_prefix_mode() == CC_PREFIX_MODE_ALLOW_ALREADY_STRIPPED) {
        snprintf(result->sponsor_raw,
                 sizeof(result->sponsor_raw),
                 "%s",
                 dialed_digits);
        result->already_stripped = 1;
        return PJ_SUCCESS;
    }

    return PJ_ENOTFOUND;
}

pj_status_t cc_normalize_msisdn(const char *input,
                                char *normalized,
                                pj_size_t normalized_len)
{
    char user[256];
    char digits[128];
    char country_digits[32];
    const char *local_digits;
    int len;

    if (!input || !normalized || normalized_len == 0)
        return PJ_EINVAL;

    normalized[0] = '\0';

    if (cc_extract_uri_user(input, user, sizeof(user)) != PJ_SUCCESS)
        snprintf(user, sizeof(user), "%s", input);

    if (!cc_copy_digits(user, digits, sizeof(digits)))
        return PJ_ENOTFOUND;

    if (!cc_copy_digits(cc_cfg_default_country_code(),
                        country_digits,
                        sizeof(country_digits)))
    {
        country_digits[0] = '\0';
    }

    if (country_digits[0] == '\0' ||
        cc_digits_start_with(digits, country_digits))
    {
        len = snprintf(normalized, normalized_len, "%s", digits);
    } else {
        local_digits = digits;
        while (*local_digits == '0' && local_digits[1] != '\0')
            local_digits++;

        len = snprintf(normalized,
                       normalized_len,
                       "%s%s",
                       country_digits,
                       local_digits);
    }

    if (len < 0 || (pj_size_t)len >= normalized_len) {
        normalized[0] = '\0';
        return PJ_ETOOSMALL;
    }

    return PJ_SUCCESS;
}

pj_status_t cc_validate_b_number(const char *normalized)
{
    const char *prefixes;
    size_t len;

    if (!normalized || normalized[0] == '\0')
        return PJ_EINVAL;

    len = strlen(normalized);

    if (len == 10)
        return PJ_SUCCESS;

    if (len < 10)
        return PJ_EINVAL;

    /* len > 10: check if a known prefix can be stripped to leave 10 digits */
    prefixes = cc_cfg_b_number_prefixes();
    {
        const char *p = prefixes;
        while (p && *p) {
            const char *comma = strchr(p, ',');
            size_t seg_len = comma ? (size_t)(comma - p) : strlen(p);

            if (seg_len > 0 &&
                seg_len < len &&
                strncmp(normalized, p, seg_len) == 0 &&
                (len - seg_len) == 10)
            {
                return PJ_SUCCESS;
            }

            p += seg_len;
            if (*p == ',')
                p++;
        }
    }

    return PJ_EINVAL;
}

pj_status_t cc_extract_b_number(const pj_str_t *local_uri,
                                 char *b_number, pj_size_t b_number_len)
{
    char user[128];
    cc_collect_number_t collect;

    if (!b_number || b_number_len == 0)
        return PJ_EINVAL;

    b_number[0] = '\0';

    if (cc_extract_pj_uri_user(local_uri, user, sizeof(user)) != PJ_SUCCESS)
        return PJ_ENOTFOUND;

    if (cc_split_collect_number(user, &collect) != PJ_SUCCESS)
        return PJ_ENOTFOUND;

    if (strlen(collect.sponsor_raw) >= b_number_len)
        return PJ_ETOOSMALL;

    snprintf(b_number, b_number_len, "%s", collect.sponsor_raw);
    return PJ_SUCCESS;
}

pj_status_t cc_build_b_uri(const char *b_number,
                           char *buf,
                           pj_size_t buf_len)
{
    int len;

    if (!b_number || !buf || buf_len == 0)
        return PJ_EINVAL;

    len = snprintf(buf,
                   buf_len,
                   "sip:%s@%s:%d;user=phone",
                   b_number,
                   cc_cfg_sbc_host(),
                   cc_cfg_sbc_port());
    if (len < 0 || (pj_size_t)len >= buf_len) {
        buf[0] = '\0';
        return PJ_ETOOSMALL;
    }

    return PJ_SUCCESS;
}

pj_status_t cc_build_b_from_uri(const char *b_number,
                                char *buf,
                                pj_size_t buf_len)
{
    int len;

    if (!b_number || !buf || buf_len == 0)
        return PJ_EINVAL;

    /* Format: <sip:+<number>@host:port;user=phone> */
    if (b_number[0] == '+')
        len = snprintf(buf, buf_len,
                       "<sip:%s@%s:%d;user=phone>",
                       b_number,
                       cc_cfg_local_host(),
                       cc_cfg_local_sip_port());
    else
        len = snprintf(buf, buf_len,
                       "<sip:+%s@%s:%d;user=phone>",
                       b_number,
                       cc_cfg_local_host(),
                       cc_cfg_local_sip_port());

    if (len < 0 || (pj_size_t)len >= buf_len) {
        buf[0] = '\0';
        return PJ_ETOOSMALL;
    }

    return PJ_SUCCESS;
}

pj_status_t cc_extract_identity_user(const char *identity,
                                     char *user,
                                     pj_size_t user_len)
{
    const char *start;
    const char *end;
    const char *scheme;
    pj_size_t len;

    if (!identity || !user || user_len == 0)
        return PJ_EINVAL;

    user[0] = '\0';

    scheme = cc_strcasestr_local(identity, "sips:");
    if (scheme) {
        start = scheme + 5;
    } else {
        scheme = cc_strcasestr_local(identity, "sip:");
        if (scheme) {
            start = scheme + 4;
        } else {
            scheme = cc_strcasestr_local(identity, "tel:");
            if (!scheme)
                return PJ_ENOTFOUND;
            start = scheme + 4;
        }
    }

    while (*start == ' ' || *start == '\t')
        start++;
    if (*start == '+')
        start++;

    end = start;
    while (*end != '\0' &&
           *end != '@' &&
           *end != ';' &&
           *end != '>' &&
           *end != '?' &&
           *end != ',' &&
           *end != ' ' &&
           *end != '\t' &&
           *end != '\r' &&
           *end != '\n')
    {
        end++;
    }

    len = (pj_size_t)(end - start);
    if (len == 0)
        return PJ_ENOTFOUND;
    if (len >= user_len)
        return PJ_ETOOSMALL;

    memcpy(user, start, len);
    user[len] = '\0';
    return PJ_SUCCESS;
}

pj_status_t cc_extract_pcv_icid(const char *pcv,
                                char *icid,
                                pj_size_t icid_len)
{
    const char *p;
    const char *end;
    char quote = '\0';
    pj_size_t len;

    if (!pcv || !icid || icid_len == 0)
        return PJ_EINVAL;

    icid[0] = '\0';
    p = cc_strcasestr_local(pcv, "icid-value");
    if (!p)
        return PJ_ENOTFOUND;

    p += strlen("icid-value");
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '=')
        return PJ_ENOTFOUND;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;

    if (*p == '"' || *p == '\'')
        quote = *p++;

    end = p;
    while (*end != '\0') {
        if (quote) {
            if (*end == quote)
                break;
        } else if (*end == ';' ||
                   *end == ',' ||
                   *end == ' ' ||
                   *end == '\t' ||
                   *end == '\r' ||
                   *end == '\n') {
            break;
        }
        end++;
    }

    len = (pj_size_t)(end - p);
    if (len == 0)
        return PJ_ENOTFOUND;
    if (len >= icid_len)
        return PJ_ETOOSMALL;

    memcpy(icid, p, len);
    icid[len] = '\0';
    return PJ_SUCCESS;
}

pj_status_t cc_format_nigeria_time(time_t timestamp,
                                   char *buf,
                                   size_t buf_len)
{
    time_t nigeria_time;
    struct tm nigeria_tm;
    char local_time[32];
    size_t formatted_len;
    int output_len;

    if (!buf || buf_len == 0)
        return PJ_EINVAL;

    buf[0] = '\0';
    nigeria_time = timestamp + 3600;

    if (gmtime_r(&nigeria_time, &nigeria_tm) == NULL)
        return PJ_EUNKNOWN;

    formatted_len = strftime(local_time,
                             sizeof(local_time),
                             "%Y-%m-%dT%H:%M:%S",
                             &nigeria_tm);
    if (formatted_len == 0)
        return PJ_EUNKNOWN;

    output_len = snprintf(buf,
                          buf_len,
                          "%s+01:00",
                          local_time);
    if (output_len < 0 || (size_t)output_len >= buf_len) {
        buf[0] = '\0';
        return PJ_ETOOSMALL;
    }

    return PJ_SUCCESS;
}

/* ── SDP helpers ──────────────────────────────────────────────────────────── */

pj_status_t cc_sdp_extract_rtp(const pjmedia_sdp_session *sdp,
                                cc_rtp_ep_t *ep)
{
    const pjmedia_sdp_media *m;
    const pjmedia_sdp_conn  *conn;
    pj_size_t i;

    if (!sdp || !ep) return PJ_EINVAL;

    /* Find first audio media line */
    m = NULL;
    for (i = 0; i < sdp->media_count; i++) {
        if (pj_strcmp2(&sdp->media[i]->desc.media, "audio") == 0) {
            m = sdp->media[i];
            break;
        }
    }
    if (!m) return PJ_ENOTFOUND;

    /* Connection: prefer media-level, fall back to session-level */
    conn = m->conn ? m->conn : sdp->conn;
    if (!conn) return PJ_ENOTFOUND;

    /* Extract IP */
    if (conn->addr.slen >= (pj_ssize_t)sizeof(ep->ip))
        return PJ_ETOOSMALL;
    memcpy(ep->ip, conn->addr.ptr, conn->addr.slen);
    ep->ip[conn->addr.slen] = '\0';

    ep->port  = m->desc.port;
    ep->valid = 1;

    PJ_LOG(5, (THIS_FILE, "SDP RTP endpoint: %s:%d", ep->ip, ep->port));
    return PJ_SUCCESS;
}

cc_bypass_mode_t cc_sdp_detect_bypass(const pjmedia_sdp_session *sdp)
{
    cc_rtp_ep_t ep;
    int i;

    if (cc_sdp_extract_rtp(sdp, &ep) != PJ_SUCCESS)
        return BYPASS_NONE;

    for (i = 0; i < CC_MGW_SUBNET_COUNT; i++) {
        if (strncmp(ep.ip, CC_MGW_SUBNETS[i],
                    strlen(CC_MGW_SUBNETS[i])) == 0) {
            PJ_LOG(4, (THIS_FILE, "Bypass mode: MGW (ip=%s)", ep.ip));
            return BYPASS_MGW;
        }
    }

    PJ_LOG(4, (THIS_FILE, "Bypass mode: DIRECT (ip=%s)", ep.ip));
    return BYPASS_DIRECT;
}

pjmedia_sdp_session *cc_sdp_rewrite_rtp(pj_pool_t *pool,
                                          const pjmedia_sdp_session *orig,
                                          const char *new_ip, int new_port)
{
    pjmedia_sdp_session *sdp;
    pjmedia_sdp_media   *m;
    pjmedia_sdp_conn    *conn;
    pj_size_t i;

    /* Deep-clone the SDP session */
    sdp = pjmedia_sdp_session_clone(pool, orig);
    if (!sdp) return NULL;

    /* Rewrite session-level connection */
    if (sdp->conn) {
        sdp->conn->addr = pj_str((char *)new_ip);  /* safe: pool-copied below */
        sdp->conn->addr = pj_strdup3(pool, new_ip);
    }

    /* Rewrite each audio media connection and port */
    for (i = 0; i < sdp->media_count; i++) {
        m = sdp->media[i];
        if (pj_strcmp2(&m->desc.media, "audio") != 0) continue;

        m->desc.port = (pj_uint16_t)new_port;

        conn = m->conn ? m->conn : sdp->conn;
        if (conn) {
            conn->addr = pj_strdup3(pool, new_ip);
        } else {
            /* Add media-level c= line */
            m->conn = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_conn);
            m->conn->net_type  = pj_str("IN");
            m->conn->addr_type = pj_str("IP4");
            m->conn->addr      = pj_strdup3(pool, new_ip);
        }
    }

    return sdp;
}

/* ── SIP header helpers ───────────────────────────────────────────────────── */

void cc_capture_fwd_headers(pjsip_msg *msg, cc_session_t *session)
{
    int i;
    session->fwd_hdr_count = 0;

    for (i = 0; i < CC_FWD_HDR_COUNT && 
                session->fwd_hdr_count < CC_MAX_FWD_HDRS; i++) {
        pj_str_t          hdr_name = pj_str((char *)CC_FWD_HEADERS[i]);
        pjsip_generic_string_hdr *hdr;

#if CC_BLEG_STATIC_PANI_ENABLE && CC_BLEG_REPLACE_COPIED_PANI
        /*
         * The B-leg uses one configured static PANI. Do not inspect or store
         * the parsed A-leg PANI as a generic string header: PJSIP may represent
         * this extension header with a different header type.
         */
        if (pj_stricmp2(&hdr_name, "P-Access-Network-Info") == 0) {
            pjsip_hdr *pani = pjsip_msg_find_hdr_by_name(msg,
                                                          &hdr_name,
                                                          NULL);
            if (pani) {
                PJ_LOG(3, (THIS_FILE,
                           "[B-LEG-HDR] copied A-leg PANI skipped due to static replacement"));
            }
            continue;
        }
#endif

        hdr = (pjsip_generic_string_hdr *)
              pjsip_msg_find_hdr_by_name(msg, &hdr_name, NULL);
        if (!hdr) continue;

        /* Copy the complete header value into the session-owned pool. */
        int idx = session->fwd_hdr_count++;
        strncpy(session->fwd_hdrs[idx].name,
                CC_FWD_HEADERS[i],
                sizeof(session->fwd_hdrs[idx].name) - 1);
        session->fwd_hdrs[idx].name[
            sizeof(session->fwd_hdrs[idx].name) - 1] = '\0';

        pj_size_t vlen = (pj_size_t)hdr->hvalue.slen;
        session->fwd_hdrs[idx].value =
            (char *)pj_pool_alloc(session->pool, vlen + 1);
        if (!session->fwd_hdrs[idx].value) {
            session->fwd_hdr_count--;
            PJ_LOG(1, (THIS_FILE,
                       "[ERROR] Could not retain forwarded header %s",
                       CC_FWD_HEADERS[i]));
            continue;
        }
        memcpy(session->fwd_hdrs[idx].value, hdr->hvalue.ptr, vlen);
        session->fwd_hdrs[idx].value[vlen] = '\0';

        if (strcmp(session->fwd_hdrs[idx].name,
                   "P-Asserted-Identity") == 0 &&
            session->caller_msisdn[0] == '\0')
        {
            (void)cc_extract_identity_user(session->fwd_hdrs[idx].value,
                                           session->caller_msisdn,
                                           sizeof(session->caller_msisdn));
        } else if (strcmp(session->fwd_hdrs[idx].name,
                          "P-Charging-Vector") == 0 &&
                   session->icid[0] == '\0')
        {
            (void)cc_extract_pcv_icid(session->fwd_hdrs[idx].value,
                                      session->icid,
                                      sizeof(session->icid));
        }

        PJ_LOG(5, (THIS_FILE, "Captured header: %s: %s",
                   session->fwd_hdrs[idx].name,
                   session->fwd_hdrs[idx].value));
    }
}

void cc_append_fwd_headers(pjsip_tx_data *tdata, const cc_session_t *session)
{
    int i;
    for (i = 0; i < session->fwd_hdr_count; i++) {
        cc_append_header(tdata, tdata->pool,
                         session->fwd_hdrs[i].name,
                         session->fwd_hdrs[i].value);
    }
}

void cc_append_header(pjsip_tx_data *tdata,
                      pj_pool_t *pool,
                      const char *name,
                      const char *value)
{
    pjsip_generic_string_hdr *hdr;
    pj_str_t hname  = pj_str((char *)name);
    pj_str_t hvalue = pj_str((char *)value);

    hdr = pjsip_generic_string_hdr_create(pool, &hname, &hvalue);
    if (hdr)
        pjsip_msg_add_hdr(tdata->msg, (pjsip_hdr *)hdr);
}

pj_status_t cc_log_call_rtp_info(pjsua_call_id call_id, const char *tag)
{
    pjmedia_transport_info ti;
    char local_rtp[128] = "invalid";
    char remote_rtp[128] = "invalid";
    int local_port = 0;
    int remote_port = 0;
    pj_status_t status;

    pj_bzero(&ti, sizeof(ti));

    status = pjsua_call_get_med_transport_info(call_id, 0, &ti);
    if (status != PJ_SUCCESS) {
        PJ_LOG(2, (THIS_FILE, "[%s] get_med_transport_info failed call=%d status=%d",
                   tag ? tag : "RTP", call_id, status));
        return status;
    }

    if (ti.sock_info.rtp_addr_name.addr.sa_family == PJ_AF_INET ||
        ti.sock_info.rtp_addr_name.addr.sa_family == PJ_AF_INET6)
    {
        pj_sockaddr_print(&ti.sock_info.rtp_addr_name,
                          local_rtp, sizeof(local_rtp), 0);
        local_port = pj_sockaddr_get_port(&ti.sock_info.rtp_addr_name);
    }

    if (ti.src_rtp_name.addr.sa_family == PJ_AF_INET ||
        ti.src_rtp_name.addr.sa_family == PJ_AF_INET6)
    {
        pj_sockaddr_print(&ti.src_rtp_name,
                          remote_rtp, sizeof(remote_rtp), 0);
        remote_port = pj_sockaddr_get_port(&ti.src_rtp_name);
    }

    PJ_LOG(4, (THIS_FILE,
               "[%s] RTP local=%s:%d remote/src=%s:%d",
               tag ? tag : "RTP",
               local_rtp,
               local_port,
               remote_rtp,
               remote_port));

    return PJ_SUCCESS;
}


pj_status_t cc_get_call_remote_rtp(pjsua_call_id call_id, cc_rtp_ep_t *ep)
{
    pjmedia_transport_info ti;
    char remote_rtp[128] = "";
    int remote_port = 0;
    pj_status_t status;

    if (!ep)
        return PJ_EINVAL;

    memset(ep, 0, sizeof(*ep));

    pj_bzero(&ti, sizeof(ti));

    status = pjsua_call_get_med_transport_info(call_id, 0, &ti);
    if (status != PJ_SUCCESS)
        return status;

    if (!(ti.src_rtp_name.addr.sa_family == PJ_AF_INET ||
          ti.src_rtp_name.addr.sa_family == PJ_AF_INET6))
    {
        return PJ_ENOTFOUND;
    }

    pj_sockaddr_print(&ti.src_rtp_name, remote_rtp, sizeof(remote_rtp), 0);
    remote_port = pj_sockaddr_get_port(&ti.src_rtp_name);

    if (remote_port <= 0 || remote_rtp[0] == '\0')
        return PJ_ENOTFOUND;

    snprintf(ep->ip, sizeof(ep->ip), "%s", remote_rtp);
    ep->port = remote_port;
    ep->valid = 1;

    PJ_LOG(3, (THIS_FILE, "Remote RTP learned for call=%d: %s:%d",
               call_id, ep->ip, ep->port));

    return PJ_SUCCESS;
}



/* ── Media helpers ────────────────────────────────────────────────────────── */

#ifndef PJSUA_MAX_PLAYERS
#define CC_PLAYER_SLOTS 256
#else
#define CC_PLAYER_SLOTS PJSUA_MAX_PLAYERS
#endif

typedef struct {
    int                 in_use;
    int                 is_mem;
    int                 is_rtpengine;
    int                 rtpengine_for_a;
    cc_session_t       *rtpengine_session;
    pjsua_player_id     file_id;
    pjsua_conf_port_id  conf_slot;
    pjmedia_port       *port;
    pj_pool_t          *pool;
    int                 duration_ms;
} cc_player_slot_t;

static cc_player_slot_t g_players[CC_PLAYER_SLOTS];
static pthread_mutex_t g_player_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    pjmedia_port          base;
    pjmedia_port         *child;
    pjmedia_echo_state   *echo;
    pjmedia_echo_state   *deferred_echo;
    pj_pool_t            *pool;
    pthread_mutex_t       lock;
    int                   dead;
    int                   in_flight;
    int                   lock_ready;
} cc_ec_wrap_t;

static cc_ec_wrap_t **g_ec_wrap;
static int g_ec_wrap_max;
static pthread_mutex_t g_ec_wrap_lock = PTHREAD_MUTEX_INITIALIZER;

static int player_alloc_slot(void)
{
    int i;

    pthread_mutex_lock(&g_player_lock);
    for (i = 0; i < CC_PLAYER_SLOTS; i++) {
        if (!g_players[i].in_use) {
            memset(&g_players[i], 0, sizeof(g_players[i]));
            g_players[i].in_use = 1;
            g_players[i].file_id = PJSUA_INVALID_ID;
            g_players[i].conf_slot = PJSUA_INVALID_ID;
            pthread_mutex_unlock(&g_player_lock);
            return i;
        }
    }
    pthread_mutex_unlock(&g_player_lock);
    return -1;
}

static void player_free_slot(int idx)
{
    if (idx < 0 || idx >= CC_PLAYER_SLOTS)
        return;
    pthread_mutex_lock(&g_player_lock);
    memset(&g_players[idx], 0, sizeof(g_players[idx]));
    g_players[idx].file_id = PJSUA_INVALID_ID;
    g_players[idx].conf_slot = PJSUA_INVALID_ID;
    pthread_mutex_unlock(&g_player_lock);
}

/*
 * First active audio conference slot, or PJSUA_INVALID_ID (-1).
 * MEDIA_ACTIVE alone is not enough: conf_slot can still be -1 while the
 * stream is coming up or tearing down, and audio may not be media[0].
 * pjsua_conf_connect2() asserts source >= 0 && sink >= 0.
 */
static pjsua_conf_port_id cc_call_conf_slot(const pjsua_call_info *ci)
{
    unsigned i;

    if (!ci)
        return PJSUA_INVALID_ID;

    for (i = 0; i < ci->media_cnt; i++) {
        if (ci->media[i].type != PJMEDIA_TYPE_AUDIO)
            continue;
        if (ci->media[i].status != PJSUA_CALL_MEDIA_ACTIVE)
            continue;
        if (ci->media[i].stream.aud.conf_slot >= 0)
            return ci->media[i].stream.aud.conf_slot;
    }
    return PJSUA_INVALID_ID;
}

static pjsua_conf_port_id cc_live_call_conf_slot(pjsua_call_id call_id,
                                                const pjsua_call_info *fallback)
{
    pjsua_call_info live;
    pjsua_conf_port_id slot = PJSUA_INVALID_ID;

    if (call_id != PJSUA_INVALID_ID &&
        pjsua_call_get_info(call_id, &live) == PJ_SUCCESS)
        slot = cc_call_conf_slot(&live);
    if (slot < 0 && fallback)
        slot = cc_call_conf_slot(fallback);
    return slot;
}

static pj_status_t cc_conf_connect_checked(pjsua_conf_port_id source,
                                           pjsua_conf_port_id sink,
                                           const char *what)
{
    if (source < 0 || sink < 0) {
        PJ_LOG(1, (THIS_FILE,
                   "[CONF] skip connect %s source=%d sink=%d",
                   what ? what : "", (int)source, (int)sink));
        return PJ_EINVAL;
    }
    return pjsua_conf_connect(source, sink);
}

static int wav_file_duration_ms(pjsua_player_id pid)
{
    pjmedia_port *port = NULL;
    pj_ssize_t data_len;
    const pjmedia_port_info *info;
    int bps, dur;

    if (pid == PJSUA_INVALID_ID) return 4000;
    if (pjsua_player_get_port(pid, &port) != PJ_SUCCESS || !port) return 4000;
    data_len = pjmedia_wav_player_get_len(port);
    if (data_len <= 0) return 4000;
    info = &port->info;
    bps  = (info->fmt.det.aud.bits_per_sample / 8) *
            info->fmt.det.aud.channel_count;
    if (bps <= 0 || info->fmt.det.aud.clock_rate == 0) return 4000;
    dur = (int)((long long)data_len * 1000 /
                (info->fmt.det.aud.clock_rate * bps));
    return dur > 0 ? dur : 4000;
}

static int call_media_ready(pjsua_call_id call_id, pjsua_call_info *ci)
{
    pj_status_t status = pjsua_call_get_info(call_id, ci);
    pjsua_conf_port_id slot = PJSUA_INVALID_ID;

    if (status == PJ_SUCCESS)
        slot = cc_call_conf_slot(ci);

    if (status != PJ_SUCCESS ||
        ci->state < PJSIP_INV_STATE_EARLY ||
        ci->state >= PJSIP_INV_STATE_DISCONNECTED ||
        slot < 0)
    {
        PJ_LOG(3, (THIS_FILE,
                   "[VOICE] cc_start_wav: call %d not ready (state=%d media=%d slot=%d) — skip",
                   call_id,
                   status == PJ_SUCCESS ? (int)ci->state : -1,
                   status == PJ_SUCCESS && ci->media_cnt > 0
                       ? (int)ci->media[0].status : -1,
                   (int)slot));
        return 0;
    }
    return 1;
}

static pjsua_player_id start_wav_mem(pjsua_call_id call_id,
                                     const pjsua_call_info *ci,
                                     const char *wav_path,
                                     const cc_wav_pcm_t *pcm,
                                     pj_bool_t loop)
{
    int idx;
    unsigned flags = loop ? 0 : PJMEDIA_MEM_NO_LOOP;
    unsigned spf;
    pj_status_t status;
    pj_pool_t *pool;
    pjmedia_port *port = NULL;
    pjsua_conf_port_id slot = PJSUA_INVALID_ID;

    idx = player_alloc_slot();
    if (idx < 0) {
        PJ_LOG(1, (THIS_FILE, "[VOICE] player slot exhausted"));
        return PJSUA_INVALID_ID;
    }

    pool = pjsua_pool_create("cc-wav", 512, 512);
    if (!pool) {
        player_free_slot(idx);
        return PJSUA_INVALID_ID;
    }

    spf = pcm->clock_rate * pcm->channel_count * CC_AUDIO_PTIME_MS / 1000;
    if (spf == 0)
        spf = pcm->clock_rate / 50;

    status = pjmedia_mem_player_create(pool, pcm->pcm, pcm->nbytes,
                                       pcm->clock_rate, pcm->channel_count,
                                       spf, pcm->bits_per_sample, flags, &port);
    if (status != PJ_SUCCESS) {
        pj_pool_release(pool);
        player_free_slot(idx);
        PJ_LOG(2, (THIS_FILE,
                   "[VOICE] mem player failed for %s status=%d — file fallback",
                   wav_path, status));
        return PJSUA_INVALID_ID;
    }

    status = pjsua_conf_add_port(pool, port, &slot);
    if (status != PJ_SUCCESS || slot < 0) {
        if (port)
            pjmedia_port_destroy(port);
        pj_pool_release(pool);
        player_free_slot(idx);
        PJ_LOG(1, (THIS_FILE,
                   "[VOICE] mem player add_port failed call=%d status=%d slot=%d",
                   call_id, status, (int)slot));
        return PJSUA_INVALID_ID;
    }

    /*
     * Cached PCM is shared read-only; each play still adds a conf port.
     * Re-read the call slot immediately before connect: MEDIA_ACTIVE in
     * the snapshot can race with conf_slot == -1 (load abort in
     * pjsua_conf_connect2).
     */
    {
        pjsua_conf_port_id call_slot = cc_live_call_conf_slot(call_id, ci);

        status = cc_conf_connect_checked(slot, call_slot, "mem-player");
        if (status != PJ_SUCCESS) {
            pjsua_conf_remove_port(slot);
            pjmedia_port_destroy(port);
            pj_pool_release(pool);
            player_free_slot(idx);
            PJ_LOG(1, (THIS_FILE,
                       "[VOICE] mem player connect failed call=%d "
                       "player_slot=%d call_slot=%d status=%d",
                       call_id, (int)slot, (int)call_slot, status));
            return PJSUA_INVALID_ID;
        }
    }

    g_players[idx].is_mem = 1;
    g_players[idx].port = port;
    g_players[idx].pool = pool;
    g_players[idx].conf_slot = slot;
    g_players[idx].duration_ms = pcm->duration_ms > 0 ? pcm->duration_ms : 4000;
    PJ_LOG(4, (THIS_FILE, "WAV mem player %d connected to call %d (%s)",
               idx, call_id, wav_path));
    return (pjsua_player_id)idx;
}

static pjsua_player_id start_wav_file(pjsua_call_id call_id,
                                      const pjsua_call_info *ci,
                                      const char *wav_path,
                                      pj_bool_t loop)
{
    pjsua_player_id file_id = PJSUA_INVALID_ID;
    pj_str_t path;
    unsigned flags = 0;
    pj_status_t status;
    int idx;

    if (!loop)
        flags |= PJMEDIA_FILE_NO_LOOP;

    idx = player_alloc_slot();
    if (idx < 0) {
        PJ_LOG(1, (THIS_FILE, "[VOICE] player slot exhausted"));
        return PJSUA_INVALID_ID;
    }

    path = pj_str((char *)wav_path);
    status = pjsua_player_create(&path, flags, &file_id);
    if (status != PJ_SUCCESS) {
        player_free_slot(idx);
        PJ_LOG(1, (THIS_FILE, "[VOICE] failed to create player for %s: %d",
                   wav_path, status));
        return PJSUA_INVALID_ID;
    }

    {
        pjsua_conf_port_id player_port = pjsua_player_get_conf_port(file_id);
        pjsua_conf_port_id call_slot = cc_live_call_conf_slot(call_id, ci);

        status = cc_conf_connect_checked(player_port, call_slot, "file-player");
        if (status != PJ_SUCCESS) {
            PJ_LOG(1, (THIS_FILE,
                       "[VOICE] player connect failed player=%d call=%d "
                       "player_port=%d call_slot=%d status=%d",
                       file_id, call_id, (int)player_port, (int)call_slot,
                       status));
            pjsua_player_destroy(file_id);
            player_free_slot(idx);
            return PJSUA_INVALID_ID;
        }
    }

    g_players[idx].is_mem = 0;
    g_players[idx].file_id = file_id;
    g_players[idx].duration_ms = wav_file_duration_ms(file_id);
    PJ_LOG(4, (THIS_FILE, "WAV file player %d connected to call %d",
               idx, call_id));
    return (pjsua_player_id)idx;
}

pjsua_player_id cc_start_wav(pjsua_call_id call_id,
                              const char *wav_path,
                              pj_bool_t loop)
{
    pjsua_call_info ci;
    const cc_wav_pcm_t *pcm;
    pjsua_player_id pid;

    if (cc_rtpengine_enabled()) {
        cc_session_t *s = (cc_session_t *)pjsua_call_get_user_data(call_id);
        int for_a;
        int dur = 0;
        int idx;

        if (!s || !wav_path)
            return PJSUA_INVALID_ID;
        for_a = (s->call_a == call_id);
        if (cc_prompt_cache_get(wav_path) &&
            cc_prompt_cache_get(wav_path)->duration_ms > 0)
            dur = cc_prompt_cache_get(wav_path)->duration_ms;

        if (cc_rtpengine_play(s, for_a, wav_path, loop ? 1 : 0, &dur) != PJ_SUCCESS)
            return PJSUA_INVALID_ID;

        if (for_a) {
            CC_SESSION_LOCK(s);
            snprintf(s->rtpengine_a_play_file, sizeof(s->rtpengine_a_play_file),
                     "%s", wav_path);
            s->rtpengine_a_play_loop = loop ? 1 : 0;
            CC_SESSION_UNLOCK(s);
        }

        idx = player_alloc_slot();
        if (idx < 0) {
            cc_rtpengine_stop_play(s, for_a);
            return PJSUA_INVALID_ID;
        }
        pthread_mutex_lock(&g_player_lock);
        g_players[idx].is_rtpengine = 1;
        g_players[idx].rtpengine_for_a = for_a;
        g_players[idx].rtpengine_session = s;
        g_players[idx].duration_ms = dur > 0 ? dur : 4000;
        pthread_mutex_unlock(&g_player_lock);
        PJ_LOG(3, (THIS_FILE,
                   "[VOICE] RTPengine play call=%d file=%s duration=%dms loop=%d",
                   call_id, wav_path, dur > 0 ? dur : 4000, (int)loop));
        return (pjsua_player_id)idx;
    }

    if (!call_media_ready(call_id, &ci))
        return PJSUA_INVALID_ID;

    pcm = cc_prompt_cache_get(wav_path);
    if (pcm && pcm->pcm && pcm->nbytes > 0) {
        pid = start_wav_mem(call_id, &ci, wav_path, pcm, loop);
        if (pid != PJSUA_INVALID_ID)
            return pid;
        /*
         * Shared PCM is unchanged on failure. Skip file fallback when the
         * call still has no conf slot: the file path would hit the same
         * connect and only add disk I/O under load.
         */
        if (cc_live_call_conf_slot(call_id, &ci) < 0)
            return PJSUA_INVALID_ID;
    }

    return start_wav_file(call_id, &ci, wav_path, loop);
}

void cc_stop_wav(pjsua_player_id player_id, pjsua_call_id call_id)
{
    cc_player_slot_t slot;
    (void)call_id;

    if (player_id == PJSUA_INVALID_ID)
        return;
    if (player_id < 0 || player_id >= CC_PLAYER_SLOTS)
        return;

    pthread_mutex_lock(&g_player_lock);
    slot = g_players[player_id];
    pthread_mutex_unlock(&g_player_lock);

    if (!slot.in_use)
        return;

    if (slot.is_rtpengine) {
        if (slot.rtpengine_session)
            cc_rtpengine_stop_play(slot.rtpengine_session, slot.rtpengine_for_a);
        player_free_slot(player_id);
        PJ_LOG(4, (THIS_FILE, "RTPengine player %d stopped", player_id));
        return;
    }

    if (slot.is_mem) {
        if (slot.conf_slot >= 0)
            pjsua_conf_remove_port(slot.conf_slot);
        if (slot.port)
            pjmedia_port_destroy(slot.port);
        if (slot.pool)
            pj_pool_release(slot.pool);
        PJ_LOG(4, (THIS_FILE, "WAV mem player %d destroyed", player_id));
    } else if (slot.file_id != PJSUA_INVALID_ID) {
        pj_status_t status = pjsua_player_destroy(slot.file_id);
        if (status == PJ_SUCCESS)
            PJ_LOG(4, (THIS_FILE, "WAV player %d destroyed", player_id));
        else
            PJ_LOG(1, (THIS_FILE,
                       "[VOICE] player destroy failed player=%d status=%d",
                       player_id, status));
    }

    player_free_slot(player_id);
}

int cc_wav_player_duration_ms(pjsua_player_id player_id)
{
    int dur = 4000;

    if (player_id == PJSUA_INVALID_ID ||
        player_id < 0 || player_id >= CC_PLAYER_SLOTS)
        return 4000;

    pthread_mutex_lock(&g_player_lock);
    if (g_players[player_id].in_use && g_players[player_id].duration_ms > 0)
        dur = g_players[player_id].duration_ms;
    pthread_mutex_unlock(&g_player_lock);
    return dur;
}

int cc_line_echo_enabled(void)
{
    const char *e = getenv("CC_LINE_ECHO");

    if (e && e[0] != '\0') {
        if (e[0] == '0' || strcasecmp(e, "off") == 0 ||
            strcasecmp(e, "no") == 0)
            return 0;
        if (e[0] == '1' || strcasecmp(e, "on") == 0 ||
            strcasecmp(e, "yes") == 0)
            return 1;
    }
    return CC_LINE_ECHO_ENABLE;
}

static void cc_ec_wrap_free(cc_ec_wrap_t *w, pjmedia_echo_state *echo)
{
    pj_pool_t *pool;

    if (echo)
        pjmedia_echo_destroy(echo);
    if (w->lock_ready) {
        pthread_mutex_destroy(&w->lock);
        w->lock_ready = 0;
    }
    pool = w->pool;
    w->pool = NULL;
    if (pool)
        pj_pool_release(pool);
}

static void cc_ec_wrap_release_in_flight(cc_ec_wrap_t *w)
{
    pjmedia_echo_state *echo = NULL;
    int do_free = 0;

    pthread_mutex_lock(&w->lock);
    w->in_flight--;
    if (w->dead && w->in_flight == 0) {
        echo = w->deferred_echo;
        w->deferred_echo = NULL;
        do_free = 1;
    }
    pthread_mutex_unlock(&w->lock);

    if (do_free)
        cc_ec_wrap_free(w, echo);
}

static int cc_ec_wrap_acquire(cc_ec_wrap_t *w,
                              pjmedia_echo_state **echo,
                              pjmedia_port **child)
{
    pthread_mutex_lock(&w->lock);
    if (w->dead) {
        pthread_mutex_unlock(&w->lock);
        return 0;
    }
    w->in_flight++;
    *echo = w->echo;
    *child = w->child;
    pthread_mutex_unlock(&w->lock);
    return 1;
}

static pj_status_t ec_put_frame(pjmedia_port *this_port, pjmedia_frame *frame)
{
    cc_ec_wrap_t *w;
    pjmedia_echo_state *echo = NULL;
    pjmedia_port *child = NULL;
    pj_status_t status;

    if (!this_port)
        return PJ_EINVAL;
    w = (cc_ec_wrap_t *)this_port->port_data.pdata;
    if (!w || !w->lock_ready)
        return PJ_EINVALIDOP;

    if (!cc_ec_wrap_acquire(w, &echo, &child))
        return PJ_SUCCESS;

    if (echo && frame && frame->type == PJMEDIA_FRAME_TYPE_AUDIO &&
        frame->buf && frame->size > 0)
        pjmedia_echo_playback(echo, (pj_int16_t *)frame->buf);

    status = child ? pjmedia_port_put_frame(child, frame) : PJ_SUCCESS;
    cc_ec_wrap_release_in_flight(w);
    return status;
}

static pj_status_t ec_get_frame(pjmedia_port *this_port, pjmedia_frame *frame)
{
    cc_ec_wrap_t *w;
    pjmedia_echo_state *echo = NULL;
    pjmedia_port *child = NULL;
    pj_status_t status;

    if (!this_port)
        return PJ_EINVAL;
    w = (cc_ec_wrap_t *)this_port->port_data.pdata;
    if (!w || !w->lock_ready)
        return PJ_EINVALIDOP;

    if (!cc_ec_wrap_acquire(w, &echo, &child)) {
        if (frame) {
            frame->type = PJMEDIA_FRAME_TYPE_NONE;
            frame->size = 0;
        }
        return PJ_SUCCESS;
    }

    if (!child) {
        if (frame) {
            frame->type = PJMEDIA_FRAME_TYPE_NONE;
            frame->size = 0;
        }
        cc_ec_wrap_release_in_flight(w);
        return PJ_SUCCESS;
    }

    status = pjmedia_port_get_frame(child, frame);
    if (status == PJ_SUCCESS && echo && frame &&
        frame->type == PJMEDIA_FRAME_TYPE_AUDIO &&
        frame->buf && frame->size > 0)
        pjmedia_echo_capture(echo, (pj_int16_t *)frame->buf, 0);

    cc_ec_wrap_release_in_flight(w);
    return status;
}

/* Echo and pool are owned by cc_ec_wrap_teardown(), not port destroy. */
static pj_status_t ec_on_destroy(pjmedia_port *this_port)
{
    (void)this_port;
    return PJ_SUCCESS;
}

/*
 * SIP thread must not wait on media. Mark dead and return; the last
 * in-flight get/put frees echo/pool. Immediate free only if idle.
 */
static void cc_ec_wrap_teardown(cc_ec_wrap_t *w)
{
    pjmedia_echo_state *echo;
    int defer = 0;

    if (!w)
        return;

    if (!w->lock_ready) {
        echo = w->echo;
        w->echo = NULL;
        w->child = NULL;
        cc_ec_wrap_free(w, echo);
        return;
    }

    pthread_mutex_lock(&w->lock);
    w->dead = 1;
    echo = w->echo;
    w->echo = NULL;
    w->child = NULL;
    if (w->in_flight > 0) {
        w->deferred_echo = echo;
        defer = 1;
    }
    pthread_mutex_unlock(&w->lock);

    if (!defer)
        cc_ec_wrap_free(w, echo);
}

void cc_media_qos_init(int max_calls)
{
    if (max_calls <= 0)
        max_calls = 1;
    g_ec_wrap_max = max_calls;
    g_ec_wrap = (cc_ec_wrap_t **)calloc((size_t)max_calls, sizeof(*g_ec_wrap));
}

void cc_tune_audio_codecs(void)
{
    const char *names[] = { "PCMA/8000/1" };
    unsigned i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        pj_str_t id = pj_str((char *)names[i]);
        pjmedia_codec_param param;

        if (pjsua_codec_get_param(&id, &param) != PJ_SUCCESS)
            continue;
        param.setting.vad = 0;
        param.setting.plc = 1;
        param.setting.cng = 0;
        pjsua_codec_set_param(&id, &param);
    }
}

void cc_on_stream_precreate(pjsua_call_id call_id,
                            pjsua_on_stream_precreate_param *param)
{
    pjmedia_stream_info *ai;

    (void)call_id;
    if (!param || param->stream_info.type != PJMEDIA_TYPE_AUDIO)
        return;

    ai = &param->stream_info.info.aud;
    ai->jb_init = CC_JB_INIT_MS;
    ai->jb_min_pre = CC_JB_MIN_PRE_MS;
    ai->jb_max_pre = CC_JB_MAX_PRE_MS;
    ai->jb_max = CC_JB_MAX_MS;
    ai->jb_discard_algo = PJMEDIA_JB_DISCARD_PROGRESSIVE;
    /* rtpengine: media is on RE — skip stream RTCP SDES/BYE on local tp */
    if (cc_rtpengine_enabled())
        ai->rtcp_sdes_bye_disabled = PJ_TRUE;
}

void cc_on_stream_created2(pjsua_call_id call_id,
                           pjsua_on_stream_created_param *param)
{
    pjmedia_port *child;
    cc_ec_wrap_t *w;
    pj_pool_t *pool;
    unsigned clock, ccnt, bits, spf;
    pj_str_t name;
    pj_status_t status;

    /*
     * RTPengine owns media. Pause PJSUA TX+RX as soon as the stream exists
     * (before on_call_media_state) so local sockets never emit stray RTP
     * toward the MGW / SIPp while RE is relaying.
     */
    if (cc_rtpengine_enabled() && param && param->stream) {
        pj_status_t ps = pjmedia_stream_pause(param->stream,
                                              PJMEDIA_DIR_ENCODING_DECODING);
        if (ps == PJ_SUCCESS) {
            PJ_LOG(4, (THIS_FILE,
                       "[RTPENGINE] call %d stream TX+RX paused at create",
                       call_id));
        } else {
            PJ_LOG(2, (THIS_FILE,
                       "[RTPENGINE] call %d early stream pause failed status=%d",
                       call_id, ps));
        }
    }

    if (!cc_line_echo_enabled())
        return;
    if (!param || !param->port || !g_ec_wrap)
        return;
    if (call_id < 0 || call_id >= g_ec_wrap_max)
        return;

    child = param->port;
    clock = PJMEDIA_PIA_SRATE(&child->info);
    ccnt  = PJMEDIA_PIA_CCNT(&child->info);
    bits  = PJMEDIA_PIA_BITS(&child->info);
    spf   = PJMEDIA_PIA_SPF(&child->info);
    if (clock == 0 || spf == 0)
        return;

    pool = pjsua_pool_create("cc-ec", 1024, 1024);
    if (!pool)
        return;

    w = PJ_POOL_ZALLOC_T(pool, cc_ec_wrap_t);
    w->pool = pool;
    w->child = child;

    if (pthread_mutex_init(&w->lock, NULL) != 0) {
        pj_pool_release(pool);
        return;
    }
    w->lock_ready = 1;

    name = pj_str("cc-ec");
    pjmedia_port_info_init(&w->base.info, &name, 0, clock, ccnt, bits, spf);
    w->base.get_frame = &ec_get_frame;
    w->base.put_frame = &ec_put_frame;
    w->base.on_destroy = &ec_on_destroy;
    w->base.port_data.pdata = w;

    status = pjmedia_echo_create2(pool, clock, ccnt, spf, CC_EC_TAIL_MS,
                                  CC_JB_INIT_MS,
                                  PJMEDIA_ECHO_SIMPLE | PJMEDIA_ECHO_USE_SW_ECHO,
                                  &w->echo);
    if (status != PJ_SUCCESS) {
        PJ_LOG(2, (THIS_FILE,
                   "[MEDIA] echo create failed call=%d status=%d — stream unwrapped",
                   call_id, status));
        cc_ec_wrap_teardown(w);
        return;
    }

    {
        cc_ec_wrap_t *old;

        pthread_mutex_lock(&g_ec_wrap_lock);
        old = g_ec_wrap[call_id];
        g_ec_wrap[call_id] = w;
        pthread_mutex_unlock(&g_ec_wrap_lock);

        if (old) {
            PJ_LOG(2, (THIS_FILE,
                       "[MEDIA] replacing leftover echo wrapper call=%d",
                       call_id));
            cc_ec_wrap_teardown(old);
        }
    }
    param->port = &w->base;
    param->destroy_port = PJ_FALSE;
    PJ_LOG(4, (THIS_FILE, "[MEDIA] line-echo wrapper on call=%d spf=%u",
               call_id, spf));
}

void cc_on_stream_destroyed(pjsua_call_id call_id,
                            pjmedia_stream *strm,
                            unsigned stream_idx)
{
    cc_ec_wrap_t *w;

    (void)strm;
    (void)stream_idx;
    if (!g_ec_wrap || call_id < 0 || call_id >= g_ec_wrap_max)
        return;

    pthread_mutex_lock(&g_ec_wrap_lock);
    w = g_ec_wrap[call_id];
    g_ec_wrap[call_id] = NULL;
    pthread_mutex_unlock(&g_ec_wrap_lock);

    cc_ec_wrap_teardown(w);
}

void cc_isolate_call_from_master(pjsua_call_id call_id)
{
    pjsua_call_info ci;
    pjsua_conf_port_id call_slot;

    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS)
        return;

    call_slot = cc_call_conf_slot(&ci);
    if (call_slot < 0)
        return;

    /* Disconnect master (slot 0) -> call and call -> master (slot 0) */
    pjsua_conf_disconnect(0, call_slot);
    pjsua_conf_disconnect(call_slot, 0);

    PJ_LOG(4, (THIS_FILE,
               "[BRIDGE] call %d (slot %d) isolated from master",
               call_id, call_slot));
}

pj_status_t cc_bridge_calls(pjsua_call_id call_a, pjsua_call_id call_b)
{
    pjsua_call_info ci_a, ci_b;
    pj_status_t     status;

    status = pjsua_call_get_info(call_a, &ci_a);
    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE,
                   "[BRIDGE] call info failed for A=%d status=%d",
                   call_a, status));
        return status;
    }

    status = pjsua_call_get_info(call_b, &ci_b);
    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE,
                   "[BRIDGE] call info failed for B=%d status=%d",
                   call_b, status));
        return status;
    }

    {
        pjsua_conf_port_id port_a = cc_call_conf_slot(&ci_a);
        pjsua_conf_port_id port_b = cc_call_conf_slot(&ci_b);

        if (port_a < 0 || port_b < 0) {
            PJ_LOG(1, (THIS_FILE,
                       "[BRIDGE] cannot connect: invalid conf slot A=%d (slot=%d) "
                       "B=%d (slot=%d) mediaA=%d mediaB=%d",
                       call_a, (int)port_a, call_b, (int)port_b,
                       ci_a.media_cnt > 0 ? (int)ci_a.media[0].status : -1,
                       ci_b.media_cnt > 0 ? (int)ci_b.media[0].status : -1));
            return PJ_EINVALIDOP;
        }

        status = cc_conf_connect_checked(port_a, port_b, "bridge-A-B");
        if (status != PJ_SUCCESS) {
            PJ_LOG(1, (THIS_FILE,
                       "[BRIDGE] A->B connect failed A=%d B=%d slot %d->%d status=%d",
                       call_a, call_b, (int)port_a, (int)port_b, status));
            return status;
        }

        status = cc_conf_connect_checked(port_b, port_a, "bridge-B-A");
        if (status != PJ_SUCCESS) {
            pj_status_t rollback = pjsua_conf_disconnect(port_a, port_b);
            PJ_LOG(1, (THIS_FILE,
                       "[BRIDGE] B->A connect failed A=%d B=%d slot %d->%d "
                       "status=%d rollback=%d",
                       call_a, call_b, (int)port_b, (int)port_a, status,
                       rollback));
            return status;
        }
    }

    PJ_LOG(3, (THIS_FILE,
               "[BRIDGE] connected call %d <-> call %d", call_a, call_b));
    return PJ_SUCCESS;
}


pj_status_t cc_unbridge_calls(pjsua_call_id call_a, pjsua_call_id call_b)
{
    pjsua_call_info ci_a, ci_b;
    pj_status_t status;
    pj_status_t status_ab;
    pj_status_t status_ba;

    PJ_LOG(3, (THIS_FILE,
               "[UNBRIDGE] Disconnecting local media bridge A<->B"));

    if (call_a == PJSUA_INVALID_ID || call_b == PJSUA_INVALID_ID) {
        PJ_LOG(2, (THIS_FILE,
                   "[UNBRIDGE] Invalid call id(s), cannot unbridge"));
        return PJ_EINVAL;
    }

    status = pjsua_call_get_info(call_a, &ci_a);
    if (status != PJ_SUCCESS) {
        PJ_LOG(2, (THIS_FILE,
                   "[UNBRIDGE] Failed to get call info for A: %d",
                   status));
        return status;
    }

    status = pjsua_call_get_info(call_b, &ci_b);
    if (status != PJ_SUCCESS) {
        PJ_LOG(2, (THIS_FILE,
                   "[UNBRIDGE] Failed to get call info for B: %d",
                   status));
        return status;
    }

    {
        pjsua_conf_port_id port_a = cc_call_conf_slot(&ci_a);
        pjsua_conf_port_id port_b = cc_call_conf_slot(&ci_b);

        if (port_a < 0 || port_b < 0) {
            PJ_LOG(2, (THIS_FILE,
                       "[UNBRIDGE] Invalid conference slots A=%d B=%d",
                       (int)port_a, (int)port_b));
            return PJ_EINVALIDOP;
        }

        PJ_LOG(3, (THIS_FILE,
                   "[UNBRIDGE] A conf slot=%d, B conf slot=%d",
                   port_a, port_b));

        status_ab = pjsua_conf_disconnect(port_a, port_b);
        if (status_ab == PJ_SUCCESS) {
            PJ_LOG(3, (THIS_FILE, "[UNBRIDGE] A->B disconnected"));
        } else {
            PJ_LOG(2, (THIS_FILE,
                       "[UNBRIDGE] A->B disconnect failed: %d",
                       status_ab));
        }

        status_ba = pjsua_conf_disconnect(port_b, port_a);
        if (status_ba == PJ_SUCCESS) {
            PJ_LOG(3, (THIS_FILE, "[UNBRIDGE] B->A disconnected"));
        } else {
            PJ_LOG(2, (THIS_FILE,
                       "[UNBRIDGE] B->A disconnect failed: %d",
                       status_ba));
        }

        if (status_ab != PJ_SUCCESS)
            return status_ab;
        if (status_ba != PJ_SUCCESS)
            return status_ba;

        return PJ_SUCCESS;
    }
}

/*
 * Return the pjmedia_stream* backing a call's active audio media, or NULL.
 *
 * CONFIRMED against this build's actual pjsua_internal.h (pjproject 2.17-dev,
 * /usr/local/include/pjsua-lib/pjsua_internal.h on signaling-server2):
 *
 *   extern struct pjsua_data pjsua_var;          // line 709 — note: struct
 *                                                 // pjsua_data, not typedef
 *   struct pjsua_data { ... pjsua_call *calls; ... };   // line 629 — POINTER,
 *                                                 // dynamically allocated,
 *                                                 // indexes the same as an
 *                                                 // array via calls[call_id]
 *   struct pjsua_call {
 *       ...
 *       unsigned med_cnt;
 *       pjsua_call_media media[PJSUA_MAX_CALL_MEDIA];
 *       int audio_idx;    // first active audio media index
 *       ...
 *   };
 *   struct pjsua_call_media {
 *       ...
 *       struct {
 *           struct { pjmedia_stream *stream; ... } a;   // <-- what we want
 *           ...
 *       } strm;
 *       ...
 *   };
 *
 * LOCKING: PJSUA_LOCK()/PJSUA_UNLOCK() are compiled as EMPTY no-op macros in
 * this build (confirmed: only one #define exists, both bodies empty). That's
 * fine for pjsua-lib's own internal code, which only touches pjsua_var from
 * PJSIP's single event-processing thread. It is NOT fine for us: this
 * function is called both from PJSIP-callback context (cc_on_call_media_state
 * -> cc_silence_call) AND from the application's separate worker thread
 * (worker.c's ev_hold_propagate_a/ev_hold_propagate_b -> cc_resume_call_tx).
 * Reaching into this raw struct from a foreign thread with zero locking is a
 * genuine use-after-free risk if PJSIP's thread is concurrently tearing the
 * same call down. pjsua_var.mutex is a real, actively-used mutex in this
 * codebase (confirmed via grep — pj_mutex_lock(pjsua_var.mutex) appears in
 * pjsua_internal.h), so we lock it directly rather than rely on the no-op
 * PJSUA_LOCK() macro.
 */
static pjmedia_stream *cc_get_call_aud_stream(pjsua_call_id call_id)
{
    pjsua_call_info  ci;
    pjmedia_stream  *strm = NULL;
    unsigned         med_idx;

    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS ||
        ci.media_cnt == 0 ||
        ci.media[0].status != PJSUA_CALL_MEDIA_ACTIVE)
        return NULL;

    if (call_id < 0)
        return NULL;

    if (!pjsua_var.mutex) {
        PJ_LOG(1, (THIS_FILE,
                   "[BYPASS] pjsua_var.mutex is NULL — pjsua not initialised?"));
        return NULL;
    }

    pj_mutex_lock(pjsua_var.mutex);

    {
        struct pjsua_call *call = &pjsua_var.calls[call_id];

        med_idx = 0;
        if (call->audio_idx >= 0 && (unsigned)call->audio_idx < call->med_cnt)
            med_idx = (unsigned)call->audio_idx;

        if (med_idx < PJSUA_MAX_CALL_MEDIA &&
            med_idx < call->med_cnt &&
            call->media[med_idx].type == PJMEDIA_TYPE_AUDIO)
        {
            strm = call->media[med_idx].strm.a.stream;
        }
    }

    pj_mutex_unlock(pjsua_var.mutex);

    if (!strm) {
        PJ_LOG(2, (THIS_FILE,
                   "[BYPASS] call %d: no active audio stream at media[%u] "
                   "(med_cnt/audio_idx mismatch, or stream not yet created)",
                   call_id, med_idx));
    }

    return strm;
}

/*
 * Disconnect a call's conf slot from every other port (including master),
 * AND pause the underlying RTP encoder so the B2BUA physically stops
 * transmitting.
 *
 * NOTE ON PORTS: this does NOT close/release the local UDP socket — the
 * port stays open and bound for the life of the call (so it can still be
 * resumed instantly for hold/MOH — see cc_resume_call_tx below, and
 * cc_bridge_calls() callers that re-enter the media path). Actually
 * releasing the socket would require tearing down the call's media
 * transport entirely, which only happens at call teardown (BYE) and is
 * not compatible with keeping the SIP dialog alive for further
 * hold/resume/DTMF signaling.
 *
 * Called after UPDATE/re-INVITE bypass so B2BUA stops transmitting RTP.
 */
void cc_silence_call(pjsua_call_id call_id)
{
    pjsua_call_info ci;
    pjsua_conf_port_id slot = PJSUA_INVALID_ID;
    pjsua_conf_port_info *pi = NULL;
    unsigned i;
    pjmedia_stream *strm;
    int rtpengine = (cc_cfg_media_mode() == CC_MEDIA_MODE_RTPENGINE);

    if (call_id == PJSUA_INVALID_ID)
        return;
    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS)
        return;
    /* Empty / torn-down slot: get_info can succeed with NULL state. */
    if (ci.state == PJSIP_INV_STATE_NULL ||
        ci.state == PJSIP_INV_STATE_DISCONNECTED)
        return;

    /*
     * pjsua_conf_port_info embeds listeners[PJMEDIA_CONF_MAX_PORTS] and is
     * ~500KB+. Worker threads only have a 256KB stack — stack-allocating it
     * SIGSEGVs at function entry (seen: ev_accept_bridge -> cc_silence_call).
     *
     * In rtpengine mode media is not on the conf bridge — skip disconnect
     * work and only pause the PJSUA stream (stops stray local RTP).
     */
    slot = cc_call_conf_slot(&ci);
    if (!rtpengine && slot > 0) {
        pi = (pjsua_conf_port_info *)malloc(sizeof(*pi));
        if (pi && pjsua_conf_get_port_info(slot, pi) == PJ_SUCCESS) {
            for (i = 0; i < pi->listener_cnt; i++)
                pjsua_conf_disconnect(slot, pi->listeners[i]);
            /* Disconnect master (slot 0) -> this slot */
            pjsua_conf_disconnect(0, slot);
        }
        free(pi);
        pi = NULL;
    }

    /*
     * Disconnecting the conf bridge only stops audio CONTENT from being
     * mixed into this call's stream — it does NOT stop the stream itself
     * from transmitting. PJSUA's audio clock keeps calling get_frame() on
     * every active stream regardless of conf-bridge connectivity, so a
     * disconnected/isolated port still gets fed silence and the stream
     * keeps encoding + sending that silence as real RTP packets to the
     * network. Explicitly pause the encoder direction to actually stop
     * outbound RTP transmission.
     *
     * In RTPengine mode, also pause the decoder: stray RTP can still hit
     * PJSUA's local sockets (port collision with SIPp, hairpin, etc.) and
     * otherwise floods "RTP status" / "Jitter buffer reset" logs even though
     * we are not in the media path.
     */
    strm = cc_get_call_aud_stream(call_id);
    if (strm) {
        unsigned dir = PJMEDIA_DIR_ENCODING;
        const char *dir_label = "TX";

        if (rtpengine) {
            dir = PJMEDIA_DIR_ENCODING_DECODING;
            dir_label = "TX+RX";
        }

        pj_status_t pause_status = pjmedia_stream_pause(strm, dir);
        if (pause_status == PJ_SUCCESS) {
            PJ_LOG(4, (THIS_FILE,
                       "[BYPASS] call %d stream %s paused — RTP I/O stopped",
                       call_id, dir_label));
        } else {
            PJ_LOG(1, (THIS_FILE,
                       "[BYPASS] call %d stream %s pause failed status=%d — "
                       "B2BUA may keep processing stray RTP",
                       call_id, dir_label, pause_status));
        }
    } else if (!rtpengine && slot > 0) {
        PJ_LOG(1, (THIS_FILE,
                   "[BYPASS] call %d: could not resolve pjmedia_stream — "
                   "wire up cc_get_call_aud_stream() for your PJSIP build, "
                   "otherwise conf-bridge disconnect alone will NOT stop "
                   "outbound RTP (see pcap evidence)",
                   call_id));
    }

    PJ_LOG(4, (THIS_FILE,
               "[BYPASS] call %d (slot %d) silenced — B2BUA exited RTP path",
               call_id, slot));
}

/*
 * Resume RTP transmission on a call previously silenced by cc_silence_call().
 * Must be called BEFORE re-connecting the call into the conf bridge
 * (e.g. before cc_bridge_calls()) whenever the B2BUA needs to re-enter the
 * media path post-bypass — e.g. playing MOH to A while B is on hold.
 * Safe to call on a call that was never paused (pjmedia_stream_resume is a
 * no-op / returns success if the stream isn't currently paused in PJSIP).
 */
void cc_resume_call_tx(pjsua_call_id call_id)
{
    pjmedia_stream *strm;

    if (call_id == PJSUA_INVALID_ID)
        return;

    strm = cc_get_call_aud_stream(call_id);
    if (!strm) {
        PJ_LOG(1, (THIS_FILE,
                   "[BYPASS] call %d: could not resolve pjmedia_stream for resume",
                   call_id));
        return;
    }

    {
        unsigned dir = PJMEDIA_DIR_ENCODING;
        const char *dir_label = "TX";

        if (cc_cfg_media_mode() == CC_MEDIA_MODE_RTPENGINE) {
            dir = PJMEDIA_DIR_ENCODING_DECODING;
            dir_label = "TX+RX";
        }

        if (pjmedia_stream_resume(strm, dir) == PJ_SUCCESS) {
            PJ_LOG(3, (THIS_FILE,
                       "[BYPASS] call %d stream %s resumed", call_id, dir_label));
        } else {
            PJ_LOG(1, (THIS_FILE,
                       "[BYPASS] call %d stream %s resume failed",
                       call_id, dir_label));
        }
    }
}

/*
 * Watchdog: after both UPDATE 200 OKs, poll for incoming RTP on both legs.
 * If neither leg receives RTP within CC_BYPASS_RTP_WATCHDOG_MS, the MGWs
 * cannot reach each other directly — fall back to local bridge.
 */
#define CC_BYPASS_RTP_WATCHDOG_MS  2500
#define CC_BYPASS_RTP_POLL_MS        50


void cc_spawn_bypass_rtp_watchdog(cc_session_t *session,
                                   pjsua_call_id call_a,
                                   pjsua_call_id call_b)
{
    cc_event_t ev;

    CC_SESSION_LOCK(session);
    if (session->bypass_rtp_watchdog_started || session->torn_down) {
        CC_SESSION_UNLOCK(session);
        return;
    }
    session->bypass_rtp_watchdog_started = 1;
    CC_SESSION_UNLOCK(session);

    memset(&ev, 0, sizeof(ev));
    ev.type    = CC_EV_BYPASS_RTP_WATCHDOG;
    ev.session = session;
    ev.call_a  = call_a;
    ev.call_b  = call_b;
    snprintf(ev.reason, sizeof(ev.reason), "bypass-watchdog-worker");

    if (cc_worker_post(&ev) != 0) {
        CC_SESSION_LOCK(session);
        session->bypass_rtp_watchdog_started = 0;
        CC_SESSION_UNLOCK(session);
        return;
    }
    PJ_LOG(3, (THIS_FILE,
               "[BYPASS-WD] watchdog posted — fallback to bridge if no direct RTP in %dms",
               CC_BYPASS_RTP_WATCHDOG_MS));
}


/* ── Misc ─────────────────────────────────────────────────────────────────── */

static void cc_copy_cstr(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0)
        return;

    if (!src)
        src = "";

    snprintf(dst, dst_len, "%s", src);
}

static void cc_session_finish_end(cc_session_t *session,
                                  const char *status,
                                  const char *reason)
{
    char call_id[128];
    char final_status[32];
    char final_reason[64];
    time_t start_ts;
    time_t connected_ts;
    time_t end_ts;
    long duration;

    if (!session)
        return;

    CC_SESSION_LOCK(session);

    PJ_LOG(2, (THIS_FILE,
               "[CALL-END-ENTER] callId=%s end_reported=%d",
               session->call_id,
               session->end_reported));

    if (session->end_reported) {
        PJ_LOG(2, (THIS_FILE,
                   "[CALL-END-SKIP] callId=%s already reported — duplicate mark_end ignored",
                   session->call_id));
        CC_SESSION_UNLOCK(session);
        return;
    }

    /*
     * Keep an end stamp that was already taken at the decision point.
     *
     * The reject / DTMF-timeout paths deliberately defer mark_end until
     * after A's treatment prompt, because in rtpengine mode that prompt is
     * played by RTPengine and mark_end deletes the RE session. Stamping the
     * end here unconditionally therefore added the treatment length to
     * every duration — a 10 s no-DTMF window was reported as 16-18 s.
     */
    if (session->call_end_ts == 0)
        session->call_end_ts = time(NULL);

    if (status)
        cc_copy_cstr(session->final_status,
                     sizeof(session->final_status),
                     status);
    if (reason)
        cc_copy_cstr(session->final_reason,
                     sizeof(session->final_reason),
                     reason);

    /* call_connected_ts is set by run_accept_transition (accept path only).
     * For calls that end without accept (ELIGIBILITY_TIMEOUT, NO_ANSWER, etc.)
     * it is never set. Use b_prompt_start_ts as the same baseline — consistent
     * with how connected calls measure duration (from when B heard the prompt). */
    if (session->call_connected_ts == 0 && session->b_prompt_start_ts > 0)
        session->call_connected_ts = session->b_prompt_start_ts;

    /* Use call_connected_ts (billing start) as the duration baseline. */
    if (session->call_connected_ts > 0 &&
        session->call_end_ts >= session->call_connected_ts)
    {
        duration = (long)(session->call_end_ts - session->call_connected_ts);
    } else {
        duration = 0;
    }

    cc_copy_cstr(call_id, sizeof(call_id), session->call_id);
    cc_copy_cstr(final_status, sizeof(final_status), session->final_status);
    cc_copy_cstr(final_reason, sizeof(final_reason), session->final_reason);
    start_ts = session->call_start_ts;
    connected_ts = session->call_connected_ts;
    end_ts = session->call_end_ts;

    session->end_reported = 1;

    CC_SESSION_UNLOCK(session);

    if (cc_rtpengine_enabled()) {
        cc_rtpengine_query(session);
        cc_rtpengine_delete(session);
    }

    PJ_LOG(3, (THIS_FILE,
               "[CALL-END] callId=%s duration=%ld status=%s reason=%s start=%ld connected=%ld end=%ld",
               call_id,
               duration,
               final_status,
               final_reason,
               (long)start_ts,
               (long)connected_ts,
               (long)end_ts));

    cc_send_end_call_udp(session);
}

void cc_session_mark_end(cc_session_t *session,
                         const char *status,
                         const char *reason)
{
    cc_session_finish_end(session, status, reason);
}

void cc_session_log_end(cc_session_t *session)
{
    cc_session_finish_end(session, NULL, NULL);
}

pj_status_t cc_safe_hangup(pjsua_call_id call_id, pjsip_status_code code)
{
    pj_status_t status;

    if (call_id == PJSUA_INVALID_ID)
        return PJ_EINVAL;

    status = pjsua_call_hangup(call_id, code, NULL, NULL);
    if (status != PJ_SUCCESS) {
        PJ_LOG(2, (THIS_FILE,
                   "[ERROR] hangup failed call=%d code=%d status=%d",
                   call_id, code, status));
    }

    return status;
}

/* One answer2 at a time — protects SIP I/O from PJSUA mutex saturation. */
static pthread_mutex_t g_answer2_mutex = PTHREAD_MUTEX_INITIALIZER;

pj_status_t cc_call_answer2_serialized(pjsua_call_id call_id,
                                       const pjsua_call_setting *opt,
                                       unsigned code,
                                       const pj_str_t *reason,
                                       const pjsua_msg_data *msg_data,
                                       long long *lock_wait_ms_out)
{
    long long t0;
    long long waited;
    pj_status_t status;
    pjsua_call_info ci;

    if (call_id == PJSUA_INVALID_ID)
        return PJ_EINVAL;

    t0 = cc_monotonic_ms();
    pthread_mutex_lock(&g_answer2_mutex);
    waited = cc_monotonic_ms() - t0;
    if (lock_wait_ms_out)
        *lock_wait_ms_out = waited;

    if (waited > 0) {
        PJ_LOG(3, (THIS_FILE,
                   "[A-TIMING] call_a=%d phase=ANSWER2_LOCK "
                   "lock_wait_ms=%lld",
                   call_id, waited));
    }

    /*
     * Re-check under the answer2 lock: CANCEL/BYE on the SIP thread can
     * deinit media while we were waiting. Answering a DISCONNECTED call
     * races pjsua_media_channel_deinit → heap corruption / SIGABRT.
     */
    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS) {
        pthread_mutex_unlock(&g_answer2_mutex);
        return PJ_EINVALIDOP;
    }
    if (ci.state == PJSIP_INV_STATE_NULL ||
        ci.state == PJSIP_INV_STATE_DISCONNECTED ||
        ci.state == PJSIP_INV_STATE_CONFIRMED)
    {
        PJ_LOG(3, (THIS_FILE,
                   "[A-TIMING] call_a=%d phase=ANSWER2_SKIP state=%d",
                   call_id, (int)ci.state));
        pthread_mutex_unlock(&g_answer2_mutex);
        return PJ_EINVALIDOP;
    }

    status = pjsua_call_answer2(call_id, opt, code, reason, msg_data);
    pthread_mutex_unlock(&g_answer2_mutex);
    return status;
}

void cc_sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

long long cc_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

int cc_pthread_create(pthread_t *t, void *(*fn)(void *), void *arg)
{
    pthread_attr_t attr;
    size_t stack = 128 * 1024;
    if (stack < (size_t)PTHREAD_STACK_MIN)
        stack = (size_t)PTHREAD_STACK_MIN;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
    int rc = pthread_create(t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc;
}
