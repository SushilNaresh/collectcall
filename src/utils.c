/*
 * utils.c — SDP, SIP header, URI and media helpers
 */
#include "utils.h"
#include "config.h"
#include "api_mapping.h"
#include "runtime_config.h"

#include <pjsua-lib/pjsua.h>
#include <pjmedia/sdp.h>
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

    PJ_LOG(3, (THIS_FILE,
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

pjsua_player_id cc_start_wav(pjsua_call_id call_id,
                              const char *wav_path,
                              pj_bool_t loop)
{
    pjsua_player_id     player_id = PJSUA_INVALID_ID;
    pjsua_call_info     ci;
    pj_str_t            path;
    unsigned            flags = 0;
    pj_status_t         status;

    if (!loop)
        flags |= PJMEDIA_FILE_NO_LOOP;

    path = pj_str((char *)wav_path);
    status = pjsua_player_create(&path, flags, &player_id);
    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE, "[VOICE] failed to create player for %s: %d",
                   wav_path, status));
        return PJSUA_INVALID_ID;
    }

    /* Connect player to call's conference port */
    status = pjsua_call_get_info(call_id, &ci);
    if (status == PJ_SUCCESS && ci.media_cnt > 0 &&
        ci.media[0].status == PJSUA_CALL_MEDIA_ACTIVE) {
        pjsua_conf_port_id player_port = pjsua_player_get_conf_port(player_id);
        status = pjsua_conf_connect(player_port,
                                    ci.media[0].stream.aud.conf_slot);
        if (status != PJ_SUCCESS) {
            pj_status_t destroy_status;
            PJ_LOG(1, (THIS_FILE,
                       "[VOICE] player connect failed player=%d call=%d status=%d",
                       player_id, call_id, status));
            destroy_status = pjsua_player_destroy(player_id);
            if (destroy_status != PJ_SUCCESS) {
                PJ_LOG(1, (THIS_FILE,
                           "[VOICE] player cleanup failed player=%d status=%d",
                           player_id, destroy_status));
            }
            return PJSUA_INVALID_ID;
        }
        PJ_LOG(4, (THIS_FILE, "WAV player %d connected to call %d",
                   player_id, call_id));
    } else {
        PJ_LOG(2, (THIS_FILE, "Call %d media not active — player not connected",
                   call_id));
    }

    if (status != PJ_SUCCESS || ci.media_cnt == 0 ||
        ci.media[0].status != PJSUA_CALL_MEDIA_ACTIVE)
    {
        status = pjsua_player_destroy(player_id);
        if (status != PJ_SUCCESS) {
            PJ_LOG(1, (THIS_FILE,
                       "[VOICE] failed destroying unconnected player=%d status=%d",
                       player_id, status));
        }
        return PJSUA_INVALID_ID;
    }

    return player_id;
}

void cc_stop_wav(pjsua_player_id player_id, pjsua_call_id call_id)
{
    pjsua_call_info ci;
    pj_status_t status;
    if (player_id == PJSUA_INVALID_ID) return;

    /* Disconnect from call's conference port */
    if (call_id != PJSUA_INVALID_ID &&
        pjsua_call_get_info(call_id, &ci) == PJ_SUCCESS &&
        ci.media_cnt > 0) {
        pjsua_conf_port_id pp = pjsua_player_get_conf_port(player_id);
        status = pjsua_conf_disconnect(pp, ci.media[0].stream.aud.conf_slot);
        if (status != PJ_SUCCESS) {
            PJ_LOG(2, (THIS_FILE,
                       "[VOICE] player disconnect failed player=%d call=%d status=%d",
                       player_id, call_id, status));
        }
    }

    status = pjsua_player_destroy(player_id);
    if (status == PJ_SUCCESS) {
        PJ_LOG(4, (THIS_FILE, "WAV player %d destroyed", player_id));
    } else {
        PJ_LOG(1, (THIS_FILE,
                   "[VOICE] player destroy failed player=%d status=%d",
                   player_id, status));
    }
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

    if (ci_a.media_cnt == 0 || ci_b.media_cnt == 0 ||
        ci_a.media[0].status != PJSUA_CALL_MEDIA_ACTIVE ||
        ci_b.media[0].status != PJSUA_CALL_MEDIA_ACTIVE) {
        PJ_LOG(1, (THIS_FILE, "[BRIDGE] cannot connect: media not active"));
        return PJ_EINVALIDOP;
    }

    pjsua_conf_port_id port_a = ci_a.media[0].stream.aud.conf_slot;
    pjsua_conf_port_id port_b = ci_b.media[0].stream.aud.conf_slot;

    status = pjsua_conf_connect(port_a, port_b);
    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE,
                   "[BRIDGE] A->B connect failed A=%d B=%d status=%d",
                   call_a, call_b, status));
        return status;
    }

    status = pjsua_conf_connect(port_b, port_a);
    if (status != PJ_SUCCESS) {
        pj_status_t rollback = pjsua_conf_disconnect(port_a, port_b);
        PJ_LOG(1, (THIS_FILE,
                   "[BRIDGE] B->A connect failed A=%d B=%d status=%d rollback=%d",
                   call_a, call_b, status, rollback));
        return status;
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

    if (ci_a.media_cnt == 0 || ci_b.media_cnt == 0 ||
        ci_a.media[0].status != PJSUA_CALL_MEDIA_ACTIVE ||
        ci_b.media[0].status != PJSUA_CALL_MEDIA_ACTIVE) {
        PJ_LOG(2, (THIS_FILE, "[UNBRIDGE] Cannot unbridge: media not active"));
        return PJ_EINVALIDOP;
    }

    pjsua_conf_port_id port_a = ci_a.media[0].stream.aud.conf_slot;
    pjsua_conf_port_id port_b = ci_b.media[0].stream.aud.conf_slot;

    PJ_LOG(3, (THIS_FILE,
               "[UNBRIDGE] A conf slot=%d, B conf slot=%d",
               port_a, port_b));

    if (port_a < 0 || port_b < 0) {
        PJ_LOG(2, (THIS_FILE,
                   "[UNBRIDGE] Invalid conference slots A=%d B=%d",
                   port_a, port_b));
        return PJ_EINVALIDOP;
    }

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

    if (session->end_reported) {
        CC_SESSION_UNLOCK(session);
        return;
    }

    session->call_end_ts = time(NULL);

    if (status)
        cc_copy_cstr(session->final_status,
                     sizeof(session->final_status),
                     status);
    if (reason)
        cc_copy_cstr(session->final_reason,
                     sizeof(session->final_reason),
                     reason);

    if (session->b_answer_ts > 0 &&
        session->call_end_ts >= session->b_answer_ts)
    {
        duration = (long)(session->call_end_ts - session->b_answer_ts);
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

void cc_sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
