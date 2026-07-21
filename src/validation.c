#include "validation.h"
#include "config.h"
#include "runtime_config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <pj/log.h>

#define THIS_FILE "validation.c"

static void copy_string(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0)
        return;

    snprintf(dst, dst_len, "%s", src ? src : "");
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

static int extract_json_string(const char *json,
                               const char *key,
                               char *value,
                               size_t value_len)
{
    char pattern[128];
    const char *p;
    size_t used = 0;

    if (!json || !key || !value || value_len == 0)
        return 0;

    value[0] = '\0';
    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) >=
        (int)sizeof(pattern))
    {
        return 0;
    }

    p = strstr(json, pattern);
    if (!p)
        return 0;

    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p++ != ':')
        return 0;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p++ != '"')
        return 0;

    while (*p && *p != '"') {
        char c = *p++;

        if (c == '\\') {
            c = *p++;
            if (c == '\0')
                return 0;

            switch (c) {
            case '"': case '\\': case '/': break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            default:
                return 0;
            }
        }

        if (used + 1 >= value_len)
            return 0;
        value[used++] = c;
    }

    if (*p != '"')
        return 0;

    value[used] = '\0';
    return 1;
}

static int extract_json_number(const char *json,
                               const char *key,
                               char *value,
                               size_t value_len)
{
    char pattern[128];
    const char *p;
    size_t used = 0;

    if (!json || !key || !value || value_len == 0)
        return 0;

    value[0] = '\0';
    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) >=
        (int)sizeof(pattern))
    {
        return 0;
    }

    p = strstr(json, pattern);
    if (!p)
        return 0;

    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p++ != ':')
        return 0;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;

    while (*p >= '0' && *p <= '9') {
        if (used + 1 >= value_len)
            return 0;
        value[used++] = *p++;
    }

    if (used == 0)
        return 0;

    value[used] = '\0';
    return 1;
}

static int extract_json_string_or_number(const char *json,
                                         const char *key,
                                         char *value,
                                         size_t value_len)
{
    if (extract_json_string(json, key, value, value_len))
        return 1;

    return extract_json_number(json, key, value, value_len);
}

static int extract_legacy_status(const char *response, int *status)
{
    const char *p = strstr(response, "\"status\"");
    char *end;
    long parsed;

    if (!p || !status)
        return 0;

    p = strchr(p, ':');
    if (!p)
        return 0;
    p++;

    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '"')
        p++;

    errno = 0;
    parsed = strtol(p, &end, 10);
    if (errno != 0 || end == p)
        return 0;

    *status = (int)parsed;
    return 1;
}

static int status_code_to_internal(const char *status_code)
{
    if (strcmp(status_code, "CALLER_BLACKLISTED") == 0)
        return CC_VALIDATION_CALLER_BLACKLISTED;
    if (strcmp(status_code, "SPONSOR_BALANCE_FAIL") == 0)
        return CC_VALIDATION_SPONSOR_BALANCE_FAIL;
    if (strcmp(status_code, "SPONSOR_DND_ACTIVE") == 0)
        return CC_VALIDATION_SPONSOR_DND_ACTIVE;
    if (strcmp(status_code, "SPONSOR_ROAMING") == 0)
        return CC_VALIDATION_SPONSOR_ROAMING;

    return CC_VALIDATION_API_FAILURE;
}

static int bind_local_port_if_enabled(int sockfd)
{
#if CC_VALIDATION_UDP_BIND_LOCAL_PORT
    struct sockaddr_in local_addr;

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(CC_VALIDATION_UDP_LOCAL_PORT);

    if (bind(sockfd,
             (struct sockaddr *)&local_addr,
             sizeof(local_addr)) < 0)
    {
        PJ_LOG(1, (THIS_FILE,
                   "[INITIATE-UDP] bind local port %d failed: %s",
                   CC_VALIDATION_UDP_LOCAL_PORT,
                   strerror(errno)));
        return -1;
    }
#else
    (void)sockfd;
#endif

    return 0;
}

int cc_udp_validate_call(const char *caller_msisdn,
                         const char *sponsor_msisdn,
                         const char *call_id,
                         const char *source,
                         const char *timestamp,
                         cc_validation_result_t *result)
{
    int sockfd;
    struct sockaddr_in server_addr;
    struct sockaddr_in peer_addr;
    struct timeval tv;
    char caller_json[256];
    char sponsor_json[256];
    char call_id_json[512];
    char source_json[128];
    char timestamp_json[128];
    char request[1536];
    char response[2048];
    char api_status[64];
    char status_code[128];
    socklen_t addr_len;
    ssize_t sent;
    ssize_t received;
    int status;
    int request_len;
    const char *validation_host;
    int validation_port;

    if (!result)
        return -1;

    memset(result, 0, sizeof(*result));
    result->status = -1;

    if (!caller_msisdn) caller_msisdn = "";
    if (!sponsor_msisdn) sponsor_msisdn = "";
    if (!call_id) call_id = "";
    if (!source) source = "";
    if (!timestamp) timestamp = "";
    validation_host = cc_cfg_validation_host();
    validation_port = cc_cfg_validation_port();

    if (json_escape(caller_msisdn, caller_json, sizeof(caller_json)) != 0 ||
        json_escape(sponsor_msisdn, sponsor_json, sizeof(sponsor_json)) != 0 ||
        json_escape(call_id, call_id_json, sizeof(call_id_json)) != 0 ||
        json_escape(source, source_json, sizeof(source_json)) != 0 ||
        json_escape(timestamp, timestamp_json, sizeof(timestamp_json)) != 0)
    {
        copy_string(result->reason, sizeof(result->reason), "SYSTEM_ERROR");
        PJ_LOG(1, (THIS_FILE,
                   "[INITIATE-UDP] request field too long to JSON encode"));
        return -1;
    }

    request_len = snprintf(
        request, sizeof(request),
        "{\"callerMsisdn\":\"%s\",\"sponsorMsisdn\":\"%s\","
        "\"callId\":\"%s\",\"source\":\"%s\",\"timestamp\":\"%s\"}",
        caller_json,
        sponsor_json,
        call_id_json,
        source_json,
        timestamp_json);
    if (request_len < 0 || (size_t)request_len >= sizeof(request)) {
        copy_string(result->reason, sizeof(result->reason), "SYSTEM_ERROR");
        PJ_LOG(1, (THIS_FILE,
                   "[INITIATE-UDP] request exceeds UDP JSON buffer"));
        return -1;
    }

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        copy_string(result->reason, sizeof(result->reason), "SYSTEM_ERROR");
        PJ_LOG(1, (THIS_FILE,
                   "[INITIATE-UDP] socket() failed: %s",
                   strerror(errno)));
        return -1;
    }

    if (bind_local_port_if_enabled(sockfd) != 0) {
        copy_string(result->reason, sizeof(result->reason), "SYSTEM_ERROR");
        close(sockfd);
        return -1;
    }

    tv.tv_sec = cc_cfg_validation_timeout_ms() / 1000;
    tv.tv_usec = (cc_cfg_validation_timeout_ms() % 1000) * 1000;

    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        copy_string(result->reason, sizeof(result->reason), "SYSTEM_ERROR");
        PJ_LOG(1, (THIS_FILE,
                   "[INITIATE-UDP] setsockopt() failed: %s",
                   strerror(errno)));
        close(sockfd);
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(validation_port);

    if (inet_pton(AF_INET,
                  validation_host,
                  &server_addr.sin_addr) <= 0)
    {
        copy_string(result->reason, sizeof(result->reason), "SYSTEM_ERROR");
        PJ_LOG(1, (THIS_FILE,
                   "[INITIATE-UDP] invalid host: %s",
                   validation_host));
        close(sockfd);
        return -1;
    }

    PJ_LOG(3, (THIS_FILE,
               "[INITIATE-UDP] sending to %s:%d: %s",
               validation_host,
               validation_port,
               request));

    sent = sendto(sockfd,
                  request,
                  (size_t)request_len,
                  0,
                  (struct sockaddr *)&server_addr,
                  sizeof(server_addr));
    if (sent < 0) {
        copy_string(result->reason, sizeof(result->reason), "SYSTEM_ERROR");
        PJ_LOG(1, (THIS_FILE,
                   "[INITIATE-UDP] sendto() failed: %s",
                   strerror(errno)));
        close(sockfd);
        return -1;
    }

    memset(response, 0, sizeof(response));
    memset(&peer_addr, 0, sizeof(peer_addr));
    addr_len = sizeof(peer_addr);

    received = recvfrom(sockfd,
                        response,
                        sizeof(response) - 1,
                        0,
                        (struct sockaddr *)&peer_addr,
                        &addr_len);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            copy_string(result->reason,
                        sizeof(result->reason),
                        "ELIGIBILITY_TIMEOUT");
        else
            copy_string(result->reason,
                        sizeof(result->reason),
                        "SYSTEM_ERROR");

        PJ_LOG(1, (THIS_FILE,
                   "[INITIATE-UDP] recvfrom() timeout/error: %s",
                   strerror(errno)));
        close(sockfd);
        return -1;
    }

    response[received] = '\0';
    close(sockfd);

    PJ_LOG(3, (THIS_FILE,
               "[INITIATE-UDP] response: %s",
               response));

    extract_json_string_or_number(response,
                                  "serviceKey",
                                  result->service_key,
                                  sizeof(result->service_key));

    if (extract_json_string(response,
                            "status",
                            api_status,
                            sizeof(api_status)))
    {
        if (strcmp(api_status, "ELIGIBLE") == 0) {
            result->status = CC_VALIDATION_ALLOW;
            copy_string(result->reason, sizeof(result->reason), "ALLOWED");
            extract_json_string(response,
                                "details",
                                result->details,
                                sizeof(result->details));
            PJ_LOG(3, (THIS_FILE,
                       "[INITIATE-API] status=ELIGIBLE details=%s",
                       result->details));
            return result->status;
        }

        if (strcmp(api_status, "INELIGIBLE") == 0) {
            if (!extract_json_string(response,
                                     "statusCode",
                                     status_code,
                                     sizeof(status_code)))
            {
                copy_string(status_code,
                            sizeof(status_code),
                            "API_FAILURE");
            }
            extract_json_string(response,
                                "reasonDescription",
                                result->reason_description,
                                sizeof(result->reason_description));

            result->status = status_code_to_internal(status_code);
            if (result->status == CC_VALIDATION_API_FAILURE)
                copy_string(result->reason,
                            sizeof(result->reason),
                            "API_FAILURE");
            else
                copy_string(result->reason,
                            sizeof(result->reason),
                            status_code);

            PJ_LOG(3, (THIS_FILE,
                       "[INITIATE-API] status=INELIGIBLE statusCode=%s reasonDescription=%s",
                       status_code,
                       result->reason_description));
            return result->status;
        }
    }

    /*
     * Isolated compatibility for the old local stub:
     * {"status":0,"reason":"ALLOWED","serviceKey":"1234"}
     */
    if (extract_legacy_status(response, &status)) {
        result->status = status;
        if (!extract_json_string(response,
                                 "reason",
                                 result->reason,
                                 sizeof(result->reason)))
        {
            copy_string(result->reason,
                        sizeof(result->reason),
                        status == 0 ? "ALLOWED" : "API_FAILURE");
        }
        PJ_LOG(2, (THIS_FILE,
                   "[INITIATE-API] legacy numeric status=%d reason=%s",
                   result->status,
                   result->reason));
        return result->status;
    }

    result->status = CC_VALIDATION_API_FAILURE;
    copy_string(result->reason, sizeof(result->reason), "API_FAILURE");
    PJ_LOG(1, (THIS_FILE,
               "[INITIATE-API] invalid response; treating as API_FAILURE"));
    return result->status;
}
