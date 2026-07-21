/*
 * api_mapping.c - local API-style mapping logs for UDP stub results.
 */
#include "api_mapping.h"
#include "config.h"
#include "validation.h"
#include "utils.h"
#include "runtime_config.h"

#include <pj/log.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <arpa/inet.h>
#include <sys/socket.h>

#define THIS_FILE "api_mapping.c"

static const char *safe_str(const char *s)
{
    return s ? s : "";
}

static int json_escape(const char *src, char *dst, size_t dst_len)
{
    size_t used = 0;

    if (!src)
        src = "";
    if (!dst || dst_len == 0)
        return -1;

    while (*src) {
        const char *escaped = NULL;
        char unicode_escape[7];
        unsigned char c = (unsigned char)*src++;
        size_t escaped_len;

        switch (c) {
        case '"':  escaped = "\\\""; break;
        case '\\': escaped = "\\\\"; break;
        case '\b': escaped = "\\b"; break;
        case '\f': escaped = "\\f"; break;
        case '\n': escaped = "\\n"; break;
        case '\r': escaped = "\\r"; break;
        case '\t': escaped = "\\t"; break;
        default:
            if (c < 0x20) {
                snprintf(unicode_escape, sizeof(unicode_escape),
                         "\\u%04x", c);
                escaped = unicode_escape;
            }
            break;
        }

        if (escaped) {
            escaped_len = strlen(escaped);
            if (used + escaped_len >= dst_len)
                return -1;
            memcpy(dst + used, escaped, escaped_len);
            used += escaped_len;
        } else {
            if (used + 1 >= dst_len)
                return -1;
            dst[used++] = (char)c;
        }
    }

    dst[used] = '\0';
    return 0;
}

static int bind_end_local_port_if_enabled(int sockfd)
{
#if CC_CALL_END_UDP_BIND_LOCAL_PORT
    struct sockaddr_in local_addr;

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(CC_CALL_END_UDP_LOCAL_PORT);

    if (bind(sockfd,
             (struct sockaddr *)&local_addr,
             sizeof(local_addr)) < 0)
    {
        PJ_LOG(1, (THIS_FILE,
                   "[END-UDP] send failed: bind local port %d: %s",
                   CC_CALL_END_UDP_LOCAL_PORT,
                   strerror(errno)));
        return -1;
    }
#else
    (void)sockfd;
#endif

    return 0;
}

const char *cc_api_eligibility_status_from_udp(int udp_status)
{
    return udp_status == 0 ? "ELIGIBLE" : "INELIGIBLE";
}

const char *cc_api_status_code_from_udp(int udp_status)
{
    switch (udp_status) {
    case CC_VALIDATION_ALLOW:
        return "";
    case CC_VALIDATION_CALLER_BLACKLISTED:
        return "CALLER_BLACKLISTED";
    case CC_VALIDATION_SPONSOR_BALANCE_FAIL:
        return "SPONSOR_BALANCE_FAIL";
    case CC_VALIDATION_SPONSOR_DND_ACTIVE:
        return "SPONSOR_DND_ACTIVE";
    case CC_VALIDATION_SPONSOR_ROAMING:
        return "SPONSOR_ROAMING";
    case CC_VALIDATION_API_FAILURE:
    default:
        return "API_FAILURE";
    }
}

const char *cc_api_reason_description_from_udp(int udp_status)
{
    switch (udp_status) {
    case CC_VALIDATION_ALLOW:
        return "";
    case CC_VALIDATION_CALLER_BLACKLISTED:
        return "You are not allowed to use the Collect Call service. Please contact customer care for assistance.";
    case CC_VALIDATION_SPONSOR_BALANCE_FAIL:
        return "The person you are trying to call does not have sufficient balance to accept this Collect Call.";
    case CC_VALIDATION_SPONSOR_DND_ACTIVE:
        return "The sponsor has Do Not Disturb enabled.";
    case CC_VALIDATION_SPONSOR_ROAMING:
        return "The sponsor is roaming.";
    case CC_VALIDATION_API_FAILURE:
    default:
        return "The Collect Call service is temporarily unavailable. Please try again later.";
    }
}

const char *cc_api_details_from_udp(int udp_status)
{
    return udp_status == 0 ? "CALLER_IS_WHITELISTED" : "";
}

void cc_log_initiate_api_mapping(const char *caller_msisdn,
                                 const char *sponsor_msisdn,
                                 const char *call_id,
                                 const char *source,
                                 const char *timestamp,
                                 int udp_status)
{
    PJ_LOG(3, (THIS_FILE,
               "[INITIATE-API-MAPPING] callerMsisdn=%s sponsorMsisdn=%s callId=%s source=%s timestamp=%s udpStatus=%d apiStatus=%s details=%s statusCode=%s reasonDescription=%s",
               safe_str(caller_msisdn),
               safe_str(sponsor_msisdn),
               safe_str(call_id),
               safe_str(source),
               safe_str(timestamp),
               udp_status,
               cc_api_eligibility_status_from_udp(udp_status),
               cc_api_details_from_udp(udp_status),
               cc_api_status_code_from_udp(udp_status),
               cc_api_reason_description_from_udp(udp_status)));
}

void cc_map_end_call_result(const char *internal_status,
                            const char *internal_reason,
                            const char **api_status,
                            const char **api_reason)
{
    const char *status = "FAILED";
    const char *reason = "SYSTEM_ERROR";

    (void)internal_status;

    if (internal_reason) {
        if (strcmp(internal_reason, "USER_ABANDONED") == 0) {
            status = "CANCELLED";
            reason = "USER_ABANDONED";
        } else if (strcmp(internal_reason, "REJECTED_BY_SPONSOR") == 0) {
            status = "CANCELLED";
            reason = "REJECTED_BY_SPONSOR";
        } else if (strcmp(internal_reason, "SYSTEM_ERROR") == 0) {
            status = "FAILED";
            reason = "SYSTEM_ERROR";
        } else if (strcmp(internal_reason, "SPONSOR_UNREACHABLE") == 0 ||
                   strcmp(internal_reason,
                          "SPONSOR_UNREACHABLE_NoMCA") == 0)
        {
            status = "FAILED";
            reason = "SPONSOR_UNREACHABLE_NoMCA";
        } else if (strcmp(internal_reason,
                          "SPONSOR_UNREACHABLE_MCA") == 0)
        {
            status = "FAILED";
            reason = "SPONSOR_UNREACHABLE_MCA";
        } else if (strcmp(internal_reason, "NO_ANSWER") == 0) {
            status = "FAILED";
            reason = "NO_ANSWER";
        } else if (strcmp(internal_reason, "ELIGIBILITY_TIMEOUT") == 0) {
            status = "FAILED";
            reason = "ELIGIBILITY_TIMEOUT";
        } else if (strcmp(internal_reason, "NORMAL_CLEARING") == 0) {
            status = "COMPLETED";
            reason = CC_END_API_COMPLETED_REASON;
        }
    }

    if (api_status)
        *api_status = status;
    if (api_reason)
        *api_reason = reason;
}

void cc_send_end_call_udp(cc_session_t *session)
{
    char call_id[128];
    char final_status[32];
    char final_reason[64];
    char icid[128];
    char caller_msisdn[64];
    char sponsor_msisdn[64];
    char start_date[32];
    char connected_date[32];
    char end_date[32];
    char call_id_json[512];
    char status_json[128];
    char reason_json[256];
    char icid_json[512];
    char caller_json[128];
    char sponsor_json[128];
    char payload[2048];
    time_t start_ts;
    time_t connected_ts;
    time_t b_answer_ts;
    time_t end_ts;
    long duration;
    int payload_len;
    int sockfd;
    struct sockaddr_in server_addr;
    ssize_t sent;
    const char *api_status;
    const char *api_reason;
    const char *endcall_host;
    int endcall_port;

    if (!session)
        return;

#if !CC_CALL_END_UDP_ENABLE
    return;
#endif

    CC_SESSION_LOCK(session);

    snprintf(call_id, sizeof(call_id), "%s", session->call_id);
    snprintf(final_status, sizeof(final_status), "%s", session->final_status);
    snprintf(final_reason, sizeof(final_reason), "%s", session->final_reason);
    snprintf(icid, sizeof(icid), "%s", session->icid);
    snprintf(caller_msisdn, sizeof(caller_msisdn), "%s", session->caller_msisdn);
    snprintf(sponsor_msisdn, sizeof(sponsor_msisdn), "%s", session->sponsor_msisdn_normalized);
    start_ts = session->call_start_ts;
    b_answer_ts = session->b_answer_ts;
    connected_ts = b_answer_ts;   /* MTN: duration from B answer (toll-free + charged) */
    end_ts = session->call_end_ts;

    CC_SESSION_UNLOCK(session);
    endcall_host = cc_cfg_endcall_host();
    endcall_port = cc_cfg_endcall_port();

    cc_map_end_call_result(final_status,
                           final_reason,
                           &api_status,
                           &api_reason);

    PJ_LOG(3, (THIS_FILE,
               "[END-API-MAP] internal_status=%s internal_reason=%s api_status=%s api_reason=%s",
               final_status,
               final_reason,
               api_status,
               api_reason));

    if (strcmp(api_reason, "SYSTEM_ERROR") == 0 &&
        strcmp(final_reason, "SYSTEM_ERROR") != 0)
    {
        PJ_LOG(2, (THIS_FILE,
                   "[END-API-MAP] unsupported internal reason=%s; using FAILED/SYSTEM_ERROR",
                   final_reason));
    }

#if !CC_END_API_COMPLETED_REASON_CONFIRMED
    if (strcmp(api_status, "COMPLETED") == 0) {
        PJ_LOG(2, (THIS_FILE,
                   "[END-API-MAP] completed reason=%s is not confirmed in the current API accepted-values list",
                   api_reason));
    }
#endif

    if (connected_ts > 0 && end_ts >= connected_ts)
        duration = (long)(end_ts - connected_ts);
    else
        duration = 0;

    if (cc_format_nigeria_time(start_ts,
                               start_date,
                               sizeof(start_date)) != PJ_SUCCESS ||
        cc_format_nigeria_time(end_ts,
                               end_date,
                               sizeof(end_date)) != PJ_SUCCESS)
    {
        PJ_LOG(1, (THIS_FILE,
                   "[END-UDP] send failed: could not format API dates"));
        return;
    }

    connected_date[0] = '\0';
    if (b_answer_ts > 0)
        cc_format_nigeria_time(b_answer_ts, connected_date, sizeof(connected_date));

    PJ_LOG(3, (THIS_FILE,
               "[API-TIME] startDate=%s connectedDate=%s endDate=%s",
               start_date,
               connected_date[0] ? connected_date : "<not-connected>",
               end_date));

    if (json_escape(call_id, call_id_json, sizeof(call_id_json)) != 0 ||
        json_escape(api_status, status_json, sizeof(status_json)) != 0 ||
        json_escape(api_reason, reason_json, sizeof(reason_json)) != 0 ||
        json_escape(icid, icid_json, sizeof(icid_json)) != 0 ||
        json_escape(caller_msisdn, caller_json, sizeof(caller_json)) != 0 ||
        json_escape(sponsor_msisdn, sponsor_json, sizeof(sponsor_json)) != 0)
    {
        PJ_LOG(1, (THIS_FILE,
                   "[END-UDP] send failed: payload field too long"));
        return;
    }

    payload_len = snprintf(
        payload, sizeof(payload),
        "{\"callId\":\"%s\",\"callerMsisdn\":\"%s\",\"sponsorMsisdn\":\"%s\","
        "\"callDuration\":%ld,\"status\":\"%s\",\"reason\":\"%s\","
        "\"startDate\":\"%s\",\"connectedDate\":\"%s\",\"endDate\":\"%s\",\"ICID\":\"%s\"}",
        call_id_json,
        caller_json,
        sponsor_json,
        duration,
        status_json,
        reason_json,
        start_date,
        connected_date,
        end_date,
        icid_json);
    if (payload_len < 0 || (size_t)payload_len >= sizeof(payload)) {
        PJ_LOG(1, (THIS_FILE,
                   "[END-UDP] send failed: payload exceeds buffer"));
        return;
    }

    PJ_LOG(3, (THIS_FILE,
               "[ENDCALL] target=%s:%d",
               endcall_host,
               endcall_port));
    PJ_LOG(3, (THIS_FILE,
               "[END-UDP] sending to %s:%d: %s",
               endcall_host,
               endcall_port,
               payload));

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        PJ_LOG(1, (THIS_FILE,
                   "[END-UDP] send failed: socket(): %s",
                   strerror(errno)));
        return;
    }

    if (bind_end_local_port_if_enabled(sockfd) != 0) {
        close(sockfd);
        return;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(endcall_port);
    if (inet_pton(AF_INET,
                  endcall_host,
                  &server_addr.sin_addr) <= 0)
    {
        PJ_LOG(1, (THIS_FILE,
                   "[END-UDP] send failed: invalid host %s",
                   endcall_host));
        close(sockfd);
        return;
    }

    sent = sendto(sockfd,
                  payload,
                  (size_t)payload_len,
                  0,
                  (struct sockaddr *)&server_addr,
                  sizeof(server_addr));
    if (sent < 0) {
        PJ_LOG(1, (THIS_FILE,
                   "[END-UDP] send failed: %s",
                   strerror(errno)));
    } else if (sent != payload_len) {
        PJ_LOG(1, (THIS_FILE,
                   "[END-UDP] send failed: partial datagram bytes=%ld expected=%d",
                   (long)sent,
                   payload_len));
    } else {
        PJ_LOG(3, (THIS_FILE,
                   "[END-UDP] sent ok bytes=%ld",
                   (long)sent));
    }

    close(sockfd);
}
