/*
 * validation_async.c
 *
 * Single UDP socket + epoll dispatcher thread.
 * Worker threads call cc_udp_validate_async() and return immediately.
 * The dispatcher thread reads responses and invokes the callback on a
 * worker thread (via cc_worker_post) so the callback runs off the
 * epoll thread.
 *
 * Capacity: CC_VASYNC_MAX_PENDING concurrent in-flight validations.
 * Each slot holds the request context and callback pointer.
 * Matching is by call_id embedded in the JSON response.
 */

#include "validation_async.h"
#include "worker.h"
#include "runtime_config.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <time.h>

#include <pj/log.h>
#include <pj/os.h>

#define THIS_FILE       "validation_async.c"
#define MAX_PENDING     512
#define RESPONSE_BUF    2048
#define REQUEST_BUF     1536
#define EPOLL_TIMEOUT_MS 200   /* wake up to sweep timeouts */

/* ── per-request slot ───────────────────────────────────────────────────── */

typedef struct {
    int                      in_use;
    char                     call_id[128];
    cc_vasync_callback_t     cb;
    void                    *cb_arg;
    struct timespec          deadline;   /* absolute monotonic deadline */
} vasync_slot_t;

/* ── module state ───────────────────────────────────────────────────────── */

static int              g_sockfd   = -1;
static int              g_epollfd  = -1;
static pthread_t        g_thread;
static int              g_running  = 0;

static vasync_slot_t    g_slots[MAX_PENDING];
static pthread_mutex_t  g_lock     = PTHREAD_MUTEX_INITIALIZER;

static struct sockaddr_in g_server_addr;

/* ── helpers ────────────────────────────────────────────────────────────── */

static int json_escape(const char *src, char *dst, size_t dst_len)
{
    size_t used = 0;
    if (!src) src = "";
    while (*src) {
        unsigned char c = (unsigned char)*src++;
        const char *esc = NULL;
        char uni[7];
        switch (c) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        default:
            if (c < 0x20) { snprintf(uni, sizeof(uni), "\\u%04x", c); esc = uni; }
            break;
        }
        if (esc) {
            size_t el = strlen(esc);
            if (used + el >= dst_len) return -1;
            memcpy(dst + used, esc, el);
            used += el;
        } else {
            if (used + 1 >= dst_len) return -1;
            dst[used++] = (char)c;
        }
    }
    dst[used] = '\0';
    return 0;
}

static int extract_json_string(const char *json, const char *key,
                                char *value, size_t vlen)
{
    char pat[128];
    const char *p;
    size_t used = 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != ':') return 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != '"') return 0;
    while (*p && *p != '"') {
        if (used + 1 >= vlen) return 0;
        value[used++] = *p++;
    }
    value[used] = '\0';
    return 1;
}

static int extract_json_array_first(const char *json, const char *key,
                                     char *value, size_t vlen)
{
    char pat[128];
    const char *p;
    size_t used = 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != ':') return 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != '[') return 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (used+1>=vlen) return 0; value[used++]=*p++; }
        if (*p != '"') return 0;
    } else {
        while (*p >= '0' && *p <= '9') { if (used+1>=vlen) return 0; value[used++]=*p++; }
    }
    if (!used) return 0;
    value[used] = '\0';
    return 1;
}

static int extract_json_string_or_number(const char *json, const char *key,
                                          char *value, size_t vlen)
{
    char pat[128];
    const char *p;
    size_t used = 0;
    if (extract_json_string(json, key, value, vlen)) return 1;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != ':') return 0;
    while (*p == ' ' || *p == '\t') p++;
    while (*p >= '0' && *p <= '9') { if (used+1>=vlen) return 0; value[used++]=*p++; }
    if (!used) return 0;
    value[used] = '\0';
    return 1;
}

static int status_code_to_internal(const char *sc)
{
    if (strcmp(sc, "CALLER_BLACKLISTED")   == 0) return CC_VALIDATION_CALLER_BLACKLISTED;
    if (strcmp(sc, "SPONSOR_BALANCE_FAIL") == 0) return CC_VALIDATION_SPONSOR_BALANCE_FAIL;
    if (strcmp(sc, "SPONSOR_DND_ACTIVE")   == 0) return CC_VALIDATION_SPONSOR_DND_ACTIVE;
    if (strcmp(sc, "SPONSOR_ROAMING")      == 0) return CC_VALIDATION_SPONSOR_ROAMING;
    return CC_VALIDATION_API_FAILURE;
}

static void parse_response(const char *buf, cc_validation_result_t *r)
{
    char api_status[64] = "";
    char status_code[128] = "";

    memset(r, 0, sizeof(*r));
    r->status = CC_VALIDATION_API_FAILURE;
    snprintf(r->reason, sizeof(r->reason), "API_FAILURE");

    if (!extract_json_array_first(buf, "serviceKeys", r->service_key, sizeof(r->service_key)))
        extract_json_string_or_number(buf, "serviceKey", r->service_key, sizeof(r->service_key));

    if (!extract_json_string(buf, "status", api_status, sizeof(api_status)))
        return;

    if (strcmp(api_status, "ELIGIBLE") == 0) {
        r->status = CC_VALIDATION_ALLOW;
        snprintf(r->reason, sizeof(r->reason), "ALLOWED");
        extract_json_string(buf, "details", r->details, sizeof(r->details));
        return;
    }

    if (strcmp(api_status, "INELIGIBLE") == 0 || strcmp(api_status, "FAILED") == 0) {
        if (!extract_json_string(buf, "statusCode", status_code, sizeof(status_code)))
            snprintf(status_code, sizeof(status_code), "API_FAILURE");
        extract_json_string(buf, "reasonDescription",
                            r->reason_description, sizeof(r->reason_description));
        r->status = status_code_to_internal(status_code);
        snprintf(r->reason, sizeof(r->reason), "%s",
                 r->status == CC_VALIDATION_API_FAILURE ? "API_FAILURE" : status_code);
    }
}

/* ── slot management ────────────────────────────────────────────────────── */

static int slot_alloc(const char *call_id,
                      cc_vasync_callback_t cb, void *cb_arg,
                      int timeout_ms)
{
    int i;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    pthread_mutex_lock(&g_lock);
    for (i = 0; i < MAX_PENDING; i++) {
        if (!g_slots[i].in_use) {
            g_slots[i].in_use   = 1;
            snprintf(g_slots[i].call_id, sizeof(g_slots[i].call_id), "%s", call_id);
            g_slots[i].cb       = cb;
            g_slots[i].cb_arg   = cb_arg;
            g_slots[i].deadline.tv_sec  = now.tv_sec + timeout_ms / 1000;
            g_slots[i].deadline.tv_nsec = now.tv_nsec +
                                          (long)(timeout_ms % 1000) * 1000000L;
            if (g_slots[i].deadline.tv_nsec >= 1000000000L) {
                g_slots[i].deadline.tv_sec++;
                g_slots[i].deadline.tv_nsec -= 1000000000L;
            }
            pthread_mutex_unlock(&g_lock);
            return i;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return -1;
}

static void slot_free(int i)
{
    g_slots[i].in_use = 0;
}

/* ── callback event posted to worker pool ───────────────────────────────── */

static void fire_callback(cc_vasync_callback_t cb, void *cb_arg,
                          cc_validation_result_t *result)
{
    vasync_cb_event_t *e = malloc(sizeof(*e));
    if (!e) {
        /* last resort: call inline (blocks epoll thread briefly) */
        cb(cb_arg, result);
        return;
    }
    e->cb     = cb;
    e->cb_arg = cb_arg;
    e->result = *result;

    cc_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type    = CC_EV_VASYNC_CB;
    ev.data    = e;
    ev.session = NULL;
    snprintf(ev.reason, sizeof(ev.reason), "vasync-cb");
    if (cc_worker_post(&ev) != 0) {
        cb(cb_arg, result);
        free(e);
    }
}

/* ── timeout sweep ──────────────────────────────────────────────────────── */

static void sweep_timeouts(void)
{
    struct timespec now;
    int i;
    clock_gettime(CLOCK_MONOTONIC, &now);

    pthread_mutex_lock(&g_lock);
    for (i = 0; i < MAX_PENDING; i++) {
        if (!g_slots[i].in_use) continue;
        if (now.tv_sec > g_slots[i].deadline.tv_sec ||
            (now.tv_sec == g_slots[i].deadline.tv_sec &&
             now.tv_nsec >= g_slots[i].deadline.tv_nsec))
        {
            cc_vasync_callback_t cb     = g_slots[i].cb;
            void                *cb_arg = g_slots[i].cb_arg;
            slot_free(i);
            pthread_mutex_unlock(&g_lock);

            cc_validation_result_t r;
            memset(&r, 0, sizeof(r));
            r.status = -1;
            snprintf(r.reason, sizeof(r.reason), "ELIGIBILITY_TIMEOUT");
            PJ_LOG(2, (THIS_FILE, "[VASYNC] slot %d timed out", i));
            fire_callback(cb, cb_arg, &r);

            pthread_mutex_lock(&g_lock);
        }
    }
    pthread_mutex_unlock(&g_lock);
}

/* ── dispatcher thread ──────────────────────────────────────────────────── */

static void *dispatcher_thread(void *arg)
{
    pj_thread_desc  td;
    pj_thread_t    *pj_thread;
    struct epoll_event events[16];
    char buf[RESPONSE_BUF];
    (void)arg;

    pj_thread_register("vasync-disp", td, &pj_thread);

    while (g_running) {
        int n = epoll_wait(g_epollfd, events, 16, EPOLL_TIMEOUT_MS);

        if (n < 0) {
            if (errno == EINTR) continue;
            PJ_LOG(1, (THIS_FILE, "[VASYNC] epoll_wait error: %s", strerror(errno)));
            break;
        }

        if (n > 0) {
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            ssize_t received;

            /* drain all available datagrams */
            while ((received = recvfrom(g_sockfd, buf, sizeof(buf) - 1, 0,
                                        (struct sockaddr *)&peer, &plen)) > 0)
            {
                buf[received] = '\0';
                PJ_LOG(3, (THIS_FILE, "[VASYNC] response: %s", buf));

                /* match by callId in response */
                char resp_call_id[128] = "";
                extract_json_string(buf, "callId", resp_call_id, sizeof(resp_call_id));

                int matched = -1;
                cc_vasync_callback_t cb = NULL;
                void *cb_arg = NULL;

                pthread_mutex_lock(&g_lock);
                if (resp_call_id[0] != '\0') {
                    int i;
                    for (i = 0; i < MAX_PENDING; i++) {
                        if (g_slots[i].in_use &&
                            strcmp(g_slots[i].call_id, resp_call_id) == 0)
                        {
                            matched = i;
                            cb      = g_slots[i].cb;
                            cb_arg  = g_slots[i].cb_arg;
                            slot_free(i);
                            break;
                        }
                    }
                }
                pthread_mutex_unlock(&g_lock);

                if (matched >= 0) {
                    cc_validation_result_t r;
                    parse_response(buf, &r);
                    fire_callback(cb, cb_arg, &r);
                } else {
                    PJ_LOG(2, (THIS_FILE,
                               "[VASYNC] unmatched response callId='%s'",
                               resp_call_id));
                }
            }
        }

        sweep_timeouts();
    }
    return NULL;
}

/* ── public API ─────────────────────────────────────────────────────────── */

int cc_vasync_init(void)
{
    struct epoll_event ev;
    const char *host = cc_cfg_validation_host();
    int port         = cc_cfg_validation_port();

    memset(g_slots, 0, sizeof(g_slots));

    g_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sockfd < 0) {
        PJ_LOG(1, (THIS_FILE, "[VASYNC] socket() failed: %s", strerror(errno)));
        return -1;
    }

    /* non-blocking */
    int flags = fcntl(g_sockfd, F_GETFL, 0);
    fcntl(g_sockfd, F_SETFL, flags | O_NONBLOCK);

    memset(&g_server_addr, 0, sizeof(g_server_addr));
    g_server_addr.sin_family = AF_INET;
    g_server_addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &g_server_addr.sin_addr) <= 0) {
        PJ_LOG(1, (THIS_FILE, "[VASYNC] invalid host: %s", host));
        close(g_sockfd);
        g_sockfd = -1;
        return -1;
    }

    g_epollfd = epoll_create1(0);
    if (g_epollfd < 0) {
        PJ_LOG(1, (THIS_FILE, "[VASYNC] epoll_create1 failed: %s", strerror(errno)));
        close(g_sockfd);
        g_sockfd = -1;
        return -1;
    }

    memset(&ev, 0, sizeof(ev));
    ev.events  = EPOLLIN;
    ev.data.fd = g_sockfd;
    if (epoll_ctl(g_epollfd, EPOLL_CTL_ADD, g_sockfd, &ev) < 0) {
        PJ_LOG(1, (THIS_FILE, "[VASYNC] epoll_ctl failed: %s", strerror(errno)));
        close(g_epollfd); close(g_sockfd);
        g_epollfd = g_sockfd = -1;
        return -1;
    }

    g_running = 1;
    if (pthread_create(&g_thread, NULL, dispatcher_thread, NULL) != 0) {
        PJ_LOG(1, (THIS_FILE, "[VASYNC] pthread_create failed"));
        g_running = 0;
        close(g_epollfd); close(g_sockfd);
        g_epollfd = g_sockfd = -1;
        return -1;
    }

    PJ_LOG(3, (THIS_FILE, "[VASYNC] init ok host=%s port=%d max_pending=%d",
               host, port, MAX_PENDING));
    return 0;
}

void cc_vasync_destroy(void)
{
    g_running = 0;
    if (g_epollfd >= 0) { close(g_epollfd); g_epollfd = -1; }
    if (g_sockfd  >= 0) { close(g_sockfd);  g_sockfd  = -1; }
    pthread_join(g_thread, NULL);
}

int cc_udp_validate_async(const char *caller_msisdn,
                           const char *sponsor_msisdn,
                           const char *call_id,
                           const char *source,
                           const char *timestamp,
                           cc_vasync_callback_t cb,
                           void *cb_arg)
{
    char caller_j[256], sponsor_j[256], callid_j[512];
    char source_j[128], ts_j[128];
    char request[REQUEST_BUF];
    int  req_len, slot;
    ssize_t sent;

    if (g_sockfd < 0) {
        PJ_LOG(1, (THIS_FILE, "[VASYNC] not initialised"));
        return -1;
    }

    if (!caller_msisdn)  caller_msisdn  = "";
    if (!sponsor_msisdn) sponsor_msisdn = "";
    if (!call_id)        call_id        = "";
    if (!source)         source         = "";
    if (!timestamp)      timestamp      = "";

    if (json_escape(caller_msisdn,  caller_j,  sizeof(caller_j))  != 0 ||
        json_escape(sponsor_msisdn, sponsor_j, sizeof(sponsor_j)) != 0 ||
        json_escape(call_id,        callid_j,  sizeof(callid_j))  != 0 ||
        json_escape(source,         source_j,  sizeof(source_j))  != 0 ||
        json_escape(timestamp,      ts_j,      sizeof(ts_j))      != 0)
    {
        PJ_LOG(1, (THIS_FILE, "[VASYNC] field too long to JSON-encode"));
        return -1;
    }

    req_len = snprintf(request, sizeof(request),
        "{\"callerMsisdn\":\"%s\",\"sponsorMsisdn\":\"%s\","
        "\"callId\":\"%s\",\"source\":\"%s\",\"timestamp\":\"%s\"}",
        caller_j, sponsor_j, callid_j, source_j, ts_j);
    if (req_len < 0 || (size_t)req_len >= sizeof(request)) {
        PJ_LOG(1, (THIS_FILE, "[VASYNC] request buffer overflow"));
        return -1;
    }

    slot = slot_alloc(call_id, cb, cb_arg, cc_cfg_validation_timeout_ms());
    if (slot < 0) {
        PJ_LOG(1, (THIS_FILE, "[VASYNC] no free slots (max=%d)", MAX_PENDING));
        return -1;
    }

    sent = sendto(g_sockfd, request, (size_t)req_len, 0,
                  (struct sockaddr *)&g_server_addr, sizeof(g_server_addr));
    if (sent < 0) {
        pthread_mutex_lock(&g_lock);
        slot_free(slot);
        pthread_mutex_unlock(&g_lock);
        PJ_LOG(1, (THIS_FILE, "[VASYNC] sendto() failed: %s", strerror(errno)));
        return -1;
    }

    PJ_LOG(3, (THIS_FILE, "[VASYNC] sent slot=%d callId=%s", slot, call_id));
    return 0;
}
