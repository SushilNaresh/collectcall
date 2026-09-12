/*
 * rtpengine.c — UDP ng/bencode client for RTPengine
 *
 * Wire format: "<cookie> <bencode-dict>"
 * Cookie is unique per request so replies can be matched on a shared socket.
 */
#include "rtpengine.h"
#include "runtime_config.h"
#include "utils.h"
#include "worker.h"
#include "handlers.h"

#include <pjsip-ua/sip_inv.h>
#include <pjmedia/sdp.h>

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define THIS_FILE "rtpengine.c"
#define CC_NG_MAX_PKT     65507
#define CC_NG_SDP_MAX     CC_RTPENGINE_SDP_MAX
#define CC_NG_COOKIE_MAX  64

/*
 * Per-thread ng UDP sockets: concurrent offer/answer/delete no longer serialize
 * on one g_sock_lock (that capped A-answer CPS under load). Each worker/SIP
 * thread gets its own fd; cookies still demux replies on that socket.
 */
static pthread_key_t    g_sock_key;
static pthread_once_t   g_sock_key_once = PTHREAD_ONCE_INIT;
static atomic_int       g_ng_ready = 0;
static struct sockaddr_in g_dst;
static atomic_uint      g_cookie_seq = 1;

static void ng_sock_dtor(void *p)
{
    int *fd = (int *)p;
    if (!fd)
        return;
    if (*fd >= 0)
        close(*fd);
    free(fd);
}

static void ng_sock_key_init(void)
{
    (void)pthread_key_create(&g_sock_key, ng_sock_dtor);
}

static int ng_sock_get(void)
{
    int *slot;
    int fd;
    int timeout_ms;
    struct timeval tv;

    if (!atomic_load_explicit(&g_ng_ready, memory_order_acquire))
        return -1;

    pthread_once(&g_sock_key_once, ng_sock_key_init);
    slot = (int *)pthread_getspecific(g_sock_key);
    if (slot && *slot >= 0)
        return *slot;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        PJ_LOG(1, (THIS_FILE, "[RTPENGINE] per-thread socket() failed: %s",
                   strerror(errno)));
        return -1;
    }
    timeout_ms = cc_cfg_rtpengine_timeout_ms();
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (!slot) {
        slot = (int *)malloc(sizeof(*slot));
        if (!slot) {
            close(fd);
            return -1;
        }
        *slot = -1;
        if (pthread_setspecific(g_sock_key, slot) != 0) {
            close(fd);
            free(slot);
            return -1;
        }
    }
    *slot = fd;
    return fd;
}

static int              g_dtmf_sock = -1;
static pthread_t        g_dtmf_thread;
static int              g_dtmf_thread_started = 0;

#define CC_RE_BUCKETS 4096
typedef struct cc_re_map {
    unsigned            serial;
    cc_session_t       *s;
    struct cc_re_map   *next;
} cc_re_map_t;
static cc_re_map_t     *g_re_map[CC_RE_BUCKETS];
static pthread_mutex_t  g_re_map_lock = PTHREAD_MUTEX_INITIALIZER;

/* telephone-event PT 120: MicroSIP/many softphones answer with 120.
 * Offering only 101 made RE log "stray answer codec ...(120)" and set
 * DTMF output -1, so DTMF-log-dest never fired. Keep 101 as well for
 * peers that negotiate 101. */
static const char CC_DUMMY_ANSWER_SDP[] =
    "v=0\r\n"
    "o=- 0 0 IN IP4 127.0.0.1\r\n"
    "s=ccmedia\r\n"
    "c=IN IP4 127.0.0.1\r\n"
    "t=0 0\r\n"
    "m=audio 9 RTP/AVP 8 120 101\r\n"
    "a=rtpmap:8 PCMA/8000\r\n"
    "a=rtpmap:120 telephone-event/8000\r\n"
    "a=fmtp:120 0-15\r\n"
    "a=rtpmap:101 telephone-event/8000\r\n"
    "a=fmtp:101 0-15\r\n"
    "a=ptime:20\r\n"
    "a=sendrecv\r\n";

static int  dtmf_listen_start(void);
static void dtmf_listen_stop(void);
static int  restrict_sdp_text_pcma_te(cc_session_t *s, char *buf, size_t cap,
                                      const char *tag);

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    int    overflow;
} be_buf_t;

static void be_append(be_buf_t *b, const void *p, size_t n)
{
    if (!b || b->overflow)
        return;
    if (b->len + n >= b->cap) {
        b->overflow = 1;
        return;
    }
    memcpy(b->buf + b->len, p, n);
    b->len += n;
}

static void be_char(be_buf_t *b, char c)
{
    be_append(b, &c, 1);
}

static void be_str(be_buf_t *b, const char *s)
{
    char hdr[32];
    size_t n;
    int hlen;

    if (!s)
        s = "";
    n = strlen(s);
    hlen = snprintf(hdr, sizeof(hdr), "%zu:", n);
    if (hlen < 0)
        return;
    be_append(b, hdr, (size_t)hlen);
    be_append(b, s, n);
}

static void be_int(be_buf_t *b, long v)
{
    char tmp[40];
    int n = snprintf(tmp, sizeof(tmp), "i%lde", v);
    if (n > 0)
        be_append(b, tmp, (size_t)n);
}

static int be_parse_str(const char **pp, const char *end, const char **out, size_t *out_len)
{
    const char *p = *pp;
    char *colon;
    long n;

    if (p >= end)
        return -1;
    n = strtol(p, &colon, 10);
    if (colon == p || *colon != ':' || n < 0)
        return -1;
    p = colon + 1;
    if (p + n > end)
        return -1;
    *out = p;
    *out_len = (size_t)n;
    *pp = p + n;
    return 0;
}

static int be_skip_value(const char **pp, const char *end);

static int be_skip_dict_or_list(const char **pp, const char *end, int is_dict)
{
    const char *p = *pp;

    if (p >= end || (*p != (is_dict ? 'd' : 'l')))
        return -1;
    p++;
    while (p < end && *p != 'e') {
        if (is_dict) {
            const char *k;
            size_t klen;
            if (be_parse_str(&p, end, &k, &klen) != 0)
                return -1;
        }
        if (be_skip_value(&p, end) != 0)
            return -1;
    }
    if (p >= end || *p != 'e')
        return -1;
    *pp = p + 1;
    return 0;
}

static int be_skip_value(const char **pp, const char *end)
{
    const char *p = *pp;

    if (p >= end)
        return -1;
    if (*p == 'd')
        return be_skip_dict_or_list(pp, end, 1);
    if (*p == 'l')
        return be_skip_dict_or_list(pp, end, 0);
    if (*p == 'i') {
        p++;
        while (p < end && *p != 'e')
            p++;
        if (p >= end)
            return -1;
        *pp = p + 1;
        return 0;
    }
    {
        const char *s;
        size_t n;
        if (be_parse_str(&p, end, &s, &n) != 0)
            return -1;
        *pp = p;
        return 0;
    }
}

/* Find a top-level string value in a bencode dict. */
static int be_dict_get_str(const char *dict, size_t dict_len,
                           const char *key, const char **val, size_t *val_len)
{
    const char *p = dict;
    const char *end = dict + dict_len;
    size_t key_len = strlen(key);

    if (p >= end || *p != 'd')
        return -1;
    p++;
    while (p < end && *p != 'e') {
        const char *k;
        size_t klen;

        if (be_parse_str(&p, end, &k, &klen) != 0)
            return -1;
        if (klen == key_len && memcmp(k, key, key_len) == 0) {
            if (p < end && *p >= '0' && *p <= '9')
                return be_parse_str(&p, end, val, val_len);
            return -1;
        }
        if (be_skip_value(&p, end) != 0)
            return -1;
    }
    return -1;
}

static int be_dict_get_int(const char *dict, size_t dict_len,
                           const char *key, long *out)
{
    const char *p = dict;
    const char *end = dict + dict_len;
    size_t key_len = strlen(key);

    if (!out || p >= end || *p != 'd')
        return -1;
    p++;
    while (p < end && *p != 'e') {
        const char *k;
        size_t klen;

        if (be_parse_str(&p, end, &k, &klen) != 0)
            return -1;
        if (klen == key_len && memcmp(k, key, key_len) == 0) {
            char *endp;
            if (p >= end || *p != 'i')
                return -1;
            p++;
            *out = strtol(p, &endp, 10);
            if (endp == p || endp >= end || *endp != 'e')
                return -1;
            return 0;
        }
        if (be_skip_value(&p, end) != 0)
            return -1;
    }
    return -1;
}

static unsigned re_bucket(unsigned serial)
{
    return serial % CC_RE_BUCKETS;
}

static void re_map_add(cc_session_t *s)
{
    cc_re_map_t *n;
    unsigned b;

    if (!s)
        return;
    n = (cc_re_map_t *)calloc(1, sizeof(*n));
    if (!n)
        return;
    n->serial = s->session_serial;
    n->s = s;
    b = re_bucket(n->serial);
    pthread_mutex_lock(&g_re_map_lock);
    n->next = g_re_map[b];
    g_re_map[b] = n;
    pthread_mutex_unlock(&g_re_map_lock);
}

static void re_map_del(cc_session_t *s)
{
    unsigned b;
    cc_re_map_t **pp;

    if (!s)
        return;
    b = re_bucket(s->session_serial);
    pthread_mutex_lock(&g_re_map_lock);
    for (pp = &g_re_map[b]; *pp; pp = &(*pp)->next) {
        if ((*pp)->s == s || (*pp)->serial == s->session_serial) {
            cc_re_map_t *gone = *pp;
            *pp = gone->next;
            free(gone);
            break;
        }
    }
    pthread_mutex_unlock(&g_re_map_lock);
}

static cc_session_t *re_map_find(unsigned serial)
{
    cc_re_map_t *n;
    cc_session_t *s = NULL;
    unsigned b = re_bucket(serial);

    pthread_mutex_lock(&g_re_map_lock);
    for (n = g_re_map[b]; n; n = n->next) {
        if (n->serial == serial) {
            s = n->s;
            break;
        }
    }
    pthread_mutex_unlock(&g_re_map_lock);
    return s;
}

int cc_rtpengine_enabled(void)
{
    return cc_cfg_media_mode() == CC_MEDIA_MODE_RTPENGINE;
}

int cc_rtpengine_ready(const cc_session_t *s)
{
    if (!s || !cc_rtpengine_enabled())
        return 0;
    return s->rtpengine_offered && s->rtpengine_answered &&
           s->rtpengine_ep_a.valid && s->rtpengine_ep_b.valid &&
           !s->rtpengine_deleted;
}

int cc_rtpengine_sdp_target(const cc_session_t *s, int for_a_leg, cc_rtp_ep_t *out)
{
    if (!s || !out || !cc_rtpengine_enabled() || s->rtpengine_deleted)
        return 0;
    *out = for_a_leg ? s->rtpengine_ep_a : s->rtpengine_ep_b;
    return out->valid && out->port != 0;
}

void cc_rtpengine_prepare_session(cc_session_t *s)
{
    if (!s)
        return;
    if (s->rtpengine_call_id[0] != '\0')
        return;
    snprintf(s->rtpengine_call_id, sizeof(s->rtpengine_call_id),
             "cc-%u", s->session_serial);
    snprintf(s->rtpengine_from_tag, sizeof(s->rtpengine_from_tag),
             "a-%u", s->session_serial);
    snprintf(s->rtpengine_to_tag, sizeof(s->rtpengine_to_tag),
             "b-%u", s->session_serial);
    re_map_add(s);
}

pj_status_t cc_rtpengine_init(void)
{
    const char *host;

    if (atomic_load_explicit(&g_ng_ready, memory_order_acquire))
        return PJ_SUCCESS;

    host = cc_cfg_rtpengine_host();
    memset(&g_dst, 0, sizeof(g_dst));
    g_dst.sin_family = AF_INET;
    g_dst.sin_port = htons((uint16_t)cc_cfg_rtpengine_port());
    if (inet_pton(AF_INET, host, &g_dst.sin_addr) != 1) {
        PJ_LOG(1, (THIS_FILE,
                   "[RTPENGINE] invalid host '%s' — ng client disabled", host));
        return PJ_EINVAL;
    }

    pthread_once(&g_sock_key_once, ng_sock_key_init);
    atomic_store_explicit(&g_ng_ready, 1, memory_order_release);

    PJ_LOG(3, (THIS_FILE,
               "[RTPENGINE] ng client %s:%d timeout_ms=%d dtmf_dest=%s "
               "(per-thread UDP sockets)",
               host, cc_cfg_rtpengine_port(),
               cc_cfg_rtpengine_timeout_ms(),
               cc_cfg_rtpengine_dtmf_dest()));
    if (dtmf_listen_start() != 0) {
        PJ_LOG(1, (THIS_FILE,
                   "[RTPENGINE] DTMF listener failed on %s — "
                   "B digit 1/2 will be ignored (accept/reject broken)",
                   cc_cfg_rtpengine_dtmf_dest()));
        atomic_store_explicit(&g_ng_ready, 0, memory_order_release);
        return PJ_EUNKNOWN;
    }
    return PJ_SUCCESS;
}

void cc_rtpengine_shutdown(void)
{
    int *slot;

    dtmf_listen_stop();
    atomic_store_explicit(&g_ng_ready, 0, memory_order_release);
    /* Close this thread's socket; other threads close via pthread_key dtor. */
    pthread_once(&g_sock_key_once, ng_sock_key_init);
    slot = (int *)pthread_getspecific(g_sock_key);
    if (slot && *slot >= 0) {
        close(*slot);
        *slot = -1;
    }
}

static void be_flags(be_buf_t *b)
{
    const char *flags = cc_cfg_rtpengine_flags();
    char tmp[512];
    char *save = NULL;
    char *tok;
    int have_detect_dtmf = 0;
    int have_always_transcode = 0;
    int have_force_transcoding = 0;
    int flag_count = 0;

    be_str(b, "flags");
    be_char(b, 'l');
    if (flags && strlen(flags) >= sizeof(tmp)) {
        PJ_LOG(1, (THIS_FILE,
                   "[RTPENGINE] CC_RTPENGINE_FLAGS truncated (len=%zu max=%zu)",
                   strlen(flags), sizeof(tmp) - 1));
    }
    snprintf(tmp, sizeof(tmp), "%s", flags ? flags : "");
    /* Split on commas only so hyphenated multi-word flags (e.g.
     * port-latching / always-transcode) stay intact. Trim spaces. */
    tok = strtok_r(tmp, ",", &save);
    while (tok) {
        char *end;

        while (*tok == ' ' || *tok == '\t')
            tok++;
        end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        if (*tok) {
            be_str(b, tok);
            flag_count++;
            if (strcasecmp(tok, "detect-DTMF") == 0 ||
                strcasecmp(tok, "detect-dtmf") == 0 ||
                strcasecmp(tok, "detect DTMF") == 0 ||
                strcasecmp(tok, "detect dtmf") == 0)
                have_detect_dtmf = 1;
            if (strcasecmp(tok, "always-transcode") == 0 ||
                strcasecmp(tok, "always transcode") == 0)
                have_always_transcode = 1;
            if (strcasecmp(tok, "force-transcoding") == 0 ||
                strcasecmp(tok, "force transcoding") == 0)
                have_force_transcoding = 1;
        }
        tok = strtok_r(NULL, ",", &save);
    }
    /*
     * Digits on the wire + detect-DTMF alone still yielded zero DTMF-log-dest
     * notifies while streams were Kernelizing. always-transcode is not enough
     * when both sides agree on codecs — inject force-transcoding so media
     * stays in userspace and RFC2833 can be logged. detect-DTMF covers in-band.
     */
    if (!have_detect_dtmf) {
        be_str(b, "detect-DTMF");
        flag_count++;
    }
    if (!have_force_transcoding) {
        be_str(b, "force-transcoding");
        flag_count++;
    }
    if (!have_always_transcode) {
        be_str(b, "always-transcode");
        flag_count++;
    }
    be_char(b, 'e');

    PJ_LOG(4, (THIS_FILE, "[RTPENGINE] ng flags count=%d (%s)",
               flag_count, flags ? flags : ""));
}

static int ng_transact(const char *req, size_t req_len,
                       const char *cookie, size_t cookie_len,
                       char *resp, size_t resp_cap, size_t *resp_len)
{
    char pkt[CC_NG_MAX_PKT];
    ssize_t nsend, nrecv;
    int attempts;
    int sock;
    struct sockaddr_in from;
    socklen_t fromlen;

    sock = ng_sock_get();
    if (sock < 0)
        return -1;
    if (cookie_len + 1 + req_len > sizeof(pkt))
        return -1;

    memcpy(pkt, cookie, cookie_len);
    pkt[cookie_len] = ' ';
    memcpy(pkt + cookie_len + 1, req, req_len);

    /* No global lock — each thread uses its own socket. */
    nsend = sendto(sock, pkt, cookie_len + 1 + req_len, 0,
                   (struct sockaddr *)&g_dst, sizeof(g_dst));
    if (nsend < 0) {
        PJ_LOG(1, (THIS_FILE, "[RTPENGINE] sendto failed: %s", strerror(errno)));
        return -1;
    }

    /* Drain stale replies until our cookie matches. */
    for (attempts = 0; attempts < 8; attempts++) {
        fromlen = sizeof(from);
        nrecv = recvfrom(sock, resp, resp_cap - 1, 0,
                         (struct sockaddr *)&from, &fromlen);
        if (nrecv < 0) {
            PJ_LOG(1, (THIS_FILE, "[RTPENGINE] recv timeout/error: %s",
                       strerror(errno)));
            return -1;
        }
        if ((size_t)nrecv > cookie_len &&
            memcmp(resp, cookie, cookie_len) == 0 &&
            resp[cookie_len] == ' ')
        {
            resp[nrecv] = '\0';
            *resp_len = (size_t)nrecv;
            return 0;
        }
    }
    PJ_LOG(1, (THIS_FILE, "[RTPENGINE] no matching ng reply for cookie"));
    return -1;
}

static int ng_command_flag(const char *command,
                           const char *call_id,
                           const char *from_tag,
                           const char *to_tag,
                           const char *sdp,
                           int with_flags,
                           char *sdp_out, size_t sdp_out_cap,
                           char *raw_out, size_t raw_out_cap,
                           const char *extra_flag)
{
    char req[CC_NG_MAX_PKT];
    char resp[CC_NG_MAX_PKT];
    char cookie[CC_NG_COOKIE_MAX];
    be_buf_t b;
    size_t resp_len = 0;
    unsigned seq;
    const char *body;
    size_t body_len;
    const char *result;
    size_t result_len;
    const char *val;
    size_t val_len;

    if (cc_rtpengine_init() != PJ_SUCCESS)
        return -1;

    seq = atomic_fetch_add_explicit(&g_cookie_seq, 1u, memory_order_relaxed);
    snprintf(cookie, sizeof(cookie), "cc%u-%u", (unsigned)getpid(), seq);

    b.buf = req;
    b.len = 0;
    b.cap = sizeof(req);
    b.overflow = 0;

    /* Bencode keys in lexical order. */
    be_char(&b, 'd');
    be_str(&b, "call-id");
    be_str(&b, call_id);
    be_str(&b, "command");
    be_str(&b, command);
    if (with_flags) {
        const char *dtmf_dest = cc_cfg_rtpengine_dtmf_dest();
        if (dtmf_dest && dtmf_dest[0] != '\0') {
            be_str(&b, "DTMF-log-dest");
            be_str(&b, dtmf_dest);
        }
        be_flags(&b);
    } else if (extra_flag && extra_flag[0]) {
        be_str(&b, "flags");
        be_char(&b, 'l');
        be_str(&b, extra_flag);
        be_char(&b, 'e');
    }
    if (from_tag && from_tag[0]) {
        be_str(&b, "from-tag");
        be_str(&b, from_tag);
    }
    if (sdp) {
        be_str(&b, "sdp");
        be_str(&b, sdp);
    }
    if (to_tag && to_tag[0]) {
        be_str(&b, "to-tag");
        be_str(&b, to_tag);
    }
    be_char(&b, 'e');

    if (b.overflow) {
        PJ_LOG(1, (THIS_FILE, "[RTPENGINE] ng request overflow command=%s", command));
        return -1;
    }

    if (ng_transact(req, b.len, cookie, strlen(cookie), resp, sizeof(resp), &resp_len) != 0)
        return -1;

    body = memchr(resp, ' ', resp_len);
    if (!body)
        return -1;
    body++;
    body_len = (size_t)(resp + resp_len - body);

    if (raw_out && raw_out_cap) {
        size_t copy = body_len < raw_out_cap - 1 ? body_len : raw_out_cap - 1;
        memcpy(raw_out, body, copy);
        raw_out[copy] = '\0';
    }

    if (be_dict_get_str(body, body_len, "result", &result, &result_len) != 0) {
        PJ_LOG(1, (THIS_FILE, "[RTPENGINE] %s: missing result", command));
        return -1;
    }
    if (!(result_len == 2 && memcmp(result, "ok", 2) == 0)) {
        const char *reason = NULL;
        size_t rlen = 0;
        if (be_dict_get_str(body, body_len, "reason", &reason, &rlen) != 0 &&
            be_dict_get_str(body, body_len, "error-reason", &reason, &rlen) != 0)
        {
            reason = result;
            rlen = result_len;
        }
        PJ_LOG(1, (THIS_FILE, "[RTPENGINE] %s failed: %.*s",
                   command, (int)rlen, reason));
        return -1;
    }

    if (sdp_out && sdp_out_cap) {
        if (be_dict_get_str(body, body_len, "sdp", &val, &val_len) != 0) {
            PJ_LOG(1, (THIS_FILE, "[RTPENGINE] %s: ok but no sdp", command));
            return -1;
        }
        if (val_len >= sdp_out_cap) {
            PJ_LOG(1, (THIS_FILE, "[RTPENGINE] %s: sdp too large (%zu)",
                       command, val_len));
            return -1;
        }
        memcpy(sdp_out, val, val_len);
        sdp_out[val_len] = '\0';
    }

    return 0;
}

static int ng_command(const char *command,
                      const char *call_id,
                      const char *from_tag,
                      const char *to_tag,
                      const char *sdp,
                      int with_flags,
                      char *sdp_out, size_t sdp_out_cap,
                      char *raw_out, size_t raw_out_cap)
{
    return ng_command_flag(command, call_id, from_tag, to_tag, sdp, with_flags,
                           sdp_out, sdp_out_cap, raw_out, raw_out_cap, NULL);
}

static int parse_sdp_endpoint(cc_session_t *s, const char *sdp_text, cc_rtp_ep_t *ep)
{
    pjmedia_sdp_session *parsed = NULL;
    pj_pool_t *tmp = NULL;
    char *copy;
    pj_status_t status;
    size_t len;

    /*
     * Never parse into s->pool from worker threads: pj_pool is not
     * thread-safe, and pjmedia_sdp_parse keeps pointers into the input
     * buffer. A concurrent SIP-thread pool use + free(sdp_out) caused
     * glibc "unsorted double linked list corrupted" / SIGABRT.
     */
    if (!s || !s->pool || !sdp_text || !ep)
        return -1;
    len = strlen(sdp_text);
    if (len == 0 || len >= CC_NG_SDP_MAX)
        return -1;

    tmp = pj_pool_create(s->pool->factory, "re-sdp-tmp",
                         (pj_size_t)(len + 512), 512, NULL);
    if (!tmp)
        return -1;

    copy = (char *)pj_pool_alloc(tmp, (pj_size_t)len + 1);
    if (!copy) {
        pj_pool_release(tmp);
        return -1;
    }
    memcpy(copy, sdp_text, len + 1);

    status = pjmedia_sdp_parse(tmp, copy, len, &parsed);
    if (status != PJ_SUCCESS || !parsed) {
        pj_pool_release(tmp);
        return -1;
    }
    memset(ep, 0, sizeof(*ep));
    if (cc_sdp_extract_rtp(parsed, ep) != PJ_SUCCESS) {
        pj_pool_release(tmp);
        return -1;
    }
    pj_pool_release(tmp);
    return 0;
}

static int print_sdp(const pjmedia_sdp_session *sdp, char *buf, size_t buflen)
{
    int n;

    if (!sdp || !buf || buflen < 2)
        return -1;
    n = pjmedia_sdp_print(sdp, buf, (int)buflen - 1);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return n;
}

static int copy_rdata_sdp(pjsip_rx_data *rdata, char *buf, size_t buflen)
{
    pjsip_rdata_sdp_info *si;
    pjsip_msg_body *body;

    if (!rdata || !rdata->msg_info.msg || !buf)
        return -1;

    si = pjsip_rdata_get_sdp_info(rdata);
    if (si && si->sdp)
        return print_sdp(si->sdp, buf, buflen);

    body = rdata->msg_info.msg->body;
    if (!body || !body->data || body->len <= 0)
        return -1;
    if (pj_stricmp2(&body->content_type.type, "application") != 0 ||
        pj_stricmp2(&body->content_type.subtype, "sdp") != 0)
        return -1;
    if ((size_t)body->len >= buflen)
        return -1;
    memcpy(buf, body->data, (size_t)body->len);
    buf[body->len] = '\0';
    return (int)body->len;
}

int cc_rtpengine_copy_rdata_sdp(pjsip_rx_data *rdata, char *buf, size_t buflen)
{
    return copy_rdata_sdp(rdata, buf, buflen);
}

/*
 * Softphones (MicroSIP) answer telephone-event as PT 120. If the offer into
 * RTPengine only listed 101/98, RE rejects 120 as "stray" and DTMF logging
 * dies (DTMF output -1). Inject PT 120 into the audio m-line when missing.
 */
static int sdp_ensure_telephone_event_120(const char *in, char *out, size_t out_cap)
{
    const char *m;
    const char *m_end;
    const char *line;
    size_t m_len;
    size_t prefix_len;
    size_t suffix_len;
    size_t need;
    char mline[512];
    int has_120_in_m = 0;
    const char *p;

    if (!in || !out || out_cap < 64)
        return -1;

    if (strstr(in, "rtpmap:120 telephone-event") != NULL) {
        if (strlen(in) >= out_cap)
            return -1;
        memcpy(out, in, strlen(in) + 1);
        return 0;
    }

    m = strstr(in, "m=audio ");
    if (!m) {
        if (strlen(in) >= out_cap)
            return -1;
        memcpy(out, in, strlen(in) + 1);
        return 0;
    }

    m_end = strstr(m, "\r\n");
    if (!m_end)
        m_end = strchr(m, '\n');
    if (!m_end)
        return -1;
    m_len = (size_t)(m_end - m);
    if (m_len >= sizeof(mline))
        return -1;
    memcpy(mline, m, m_len);
    mline[m_len] = '\0';

    for (p = mline; *p; p++) {
        if (p[0] == '1' && p[1] == '2' && p[2] == '0' &&
            (p == mline || p[-1] == ' ') &&
            (p[3] == ' ' || p[3] == '\0' || p[3] == '\r')) {
            has_120_in_m = 1;
            break;
        }
    }
    if (!has_120_in_m) {
        if (m_len + 4 >= sizeof(mline))
            return -1;
        memcpy(mline + m_len, " 120", 4);
        m_len += 4;
        mline[m_len] = '\0';
    }

    /* Insert rtpmap/fmtp after the m-line (and any existing attrs stay). */
    prefix_len = (size_t)(m - in);
    line = m_end;
    if (line[0] == '\r' && line[1] == '\n')
        line += 2;
    else if (line[0] == '\n')
        line += 1;
    suffix_len = strlen(line);

    need = prefix_len + m_len + 2 /* CRLF */ +
           strlen("a=rtpmap:120 telephone-event/8000\r\n") +
           strlen("a=fmtp:120 0-15\r\n") +
           suffix_len + 1;
    if (need > out_cap)
        return -1;

    memcpy(out, in, prefix_len);
    memcpy(out + prefix_len, mline, m_len);
    memcpy(out + prefix_len + m_len, "\r\n", 2);
    {
        size_t o = prefix_len + m_len + 2;
        const char *ins1 = "a=rtpmap:120 telephone-event/8000\r\n";
        const char *ins2 = "a=fmtp:120 0-15\r\n";
        size_t n1 = strlen(ins1);
        size_t n2 = strlen(ins2);
        memcpy(out + o, ins1, n1);
        o += n1;
        memcpy(out + o, ins2, n2);
        o += n2;
        memcpy(out + o, line, suffix_len + 1);
    }
    return 0;
}

pj_status_t cc_rtpengine_offer(cc_session_t *s, const char *sdp)
{
    char call_id[128], from_tag[64], to_tag[64];
    char *sdp_out;
    char *sdp_in = NULL;
    cc_rtp_ep_t ep;

    if (!cc_rtpengine_enabled() || !s || !sdp || !sdp[0])
        return PJ_ENOTSUP;

    /*
     * Heap SDP buffer: worker threads use a 128KB stack. Nesting
     * offer → dummy answer with two 16KB stack arrays plus a large
     * pjsua_call_info in the caller overflows and corrupts frames
     * (SIGSEGV pthread_mutex_lock(mutex=0x4) in the field).
     */
    sdp_out = (char *)malloc(CC_NG_SDP_MAX);
    sdp_in = (char *)malloc(CC_NG_SDP_MAX);
    if (!sdp_out || !sdp_in) {
        free(sdp_out);
        free(sdp_in);
        return PJ_ENOMEM;
    }
    if (sdp_ensure_telephone_event_120(sdp, sdp_in, CC_NG_SDP_MAX) != 0) {
        free(sdp_out);
        free(sdp_in);
        return PJ_EINVAL;
    }

    /*
     * Restrict the offer RE sees to PCMA + telephone-event.
     *
     * RE picks what it sends *to A* from the codec list in this offer, not
     * from our 200 OK. A (Linphone) offers "9 101 0 8 3", so an unrestricted
     * offer makes RE send G.722 to A even though our answer advertised PCMA
     * only — prompts then arrive transcoded. sdp_out is still unused here, so
     * borrow it to hold the original in case the rewrite fails.
     */
    memcpy(sdp_out, sdp_in, strlen(sdp_in) + 1);
    if (restrict_sdp_text_pcma_te(s, sdp_in, CC_NG_SDP_MAX, "RE-OFFER") != 0) {
        memcpy(sdp_in, sdp_out, strlen(sdp_out) + 1);
        PJ_LOG(2, (THIS_FILE,
                   "[RTPENGINE] offer: PCMA restrict failed — sending A's "
                   "original codec list (RE may transcode prompts)"));
    }

    CC_SESSION_LOCK(s);
    if (s->rtpengine_deleted || s->torn_down) {
        CC_SESSION_UNLOCK(s);
        free(sdp_out);
        free(sdp_in);
        return PJ_ECANCELLED;
    }
    cc_rtpengine_prepare_session(s);
    snprintf(call_id, sizeof(call_id), "%s", s->rtpengine_call_id);
    snprintf(from_tag, sizeof(from_tag), "%s", s->rtpengine_from_tag);
    snprintf(to_tag, sizeof(to_tag), "%s", s->rtpengine_to_tag);
    CC_SESSION_UNLOCK(s);

    if (ng_command("offer", call_id, from_tag, NULL, sdp_in, 1,
                   sdp_out, CC_NG_SDP_MAX, NULL, 0) != 0) {
        free(sdp_out);
        free(sdp_in);
        return PJ_EUNKNOWN;
    }
    free(sdp_in);
    sdp_in = NULL;

    if (parse_sdp_endpoint(s, sdp_out, &ep) != 0) {
        PJ_LOG(1, (THIS_FILE, "[RTPENGINE] offer: could not parse returned SDP"));
        free(sdp_out);
        return PJ_EINVAL;
    }
    free(sdp_out);
    sdp_out = NULL;

    if (strcmp(ep.ip, "127.0.0.1") == 0 || strcmp(ep.ip, "::1") == 0 ||
        strcmp(ep.ip, "0.0.0.0") == 0)
    {
        const char *lip = cc_cfg_local_host();
        PJ_LOG(2, (THIS_FILE,
                   "[RTPENGINE] offer B-facing %s:%d → using CC_LOCAL_HOST %s",
                   ep.ip, ep.port, lip));
        snprintf(ep.ip, sizeof(ep.ip), "%s", lip);
    }

    CC_SESSION_LOCK(s);
    if (!s->rtpengine_deleted) {
        s->rtpengine_ep_b = ep;
        s->rtpengine_offered = 1;
    }
    CC_SESSION_UNLOCK(s);

    PJ_LOG(3, (THIS_FILE,
               "[RTPENGINE] offer ok call-id=%s B-facing %s:%d dtmf_dest=%s",
               call_id, ep.ip, ep.port, cc_cfg_rtpengine_dtmf_dest()));

    /*
     * Seed A-facing ports immediately so the A-leg 200 OK can advertise
     * RTPengine (prompts/DTMF never touch PJSUA). Real B SDP replaces this
     * and often reallocates A-facing ports — that triggers one A re-INVITE
     * (see leg_a_on_rtpengine_a_ep_changed). Do NOT also send post-accept
     * UPDATEs in rtpengine mode or dialogs collide under load.
     */
    {
        int need_dummy = 0;
        CC_SESSION_LOCK(s);
        need_dummy = !s->rtpengine_answered && !s->rtpengine_deleted &&
                     !s->torn_down;
        CC_SESSION_UNLOCK(s);
        if (need_dummy)
            (void)cc_rtpengine_answer(s, CC_DUMMY_ANSWER_SDP);
    }
    return PJ_SUCCESS;
}

pj_status_t cc_rtpengine_answer(cc_session_t *s, const char *sdp)
{
    char call_id[128], from_tag[64], to_tag[64];
    char *sdp_out;
    cc_rtp_ep_t ep;
    int offered;

    if (!cc_rtpengine_enabled() || !s || !sdp || !sdp[0])
        return PJ_ENOTSUP;

    sdp_out = (char *)malloc(CC_NG_SDP_MAX);
    if (!sdp_out)
        return PJ_ENOMEM;

    CC_SESSION_LOCK(s);
    if (s->rtpengine_deleted || s->torn_down) {
        CC_SESSION_UNLOCK(s);
        free(sdp_out);
        return PJ_ECANCELLED;
    }
    offered = s->rtpengine_offered;
    snprintf(call_id, sizeof(call_id), "%s", s->rtpengine_call_id);
    snprintf(from_tag, sizeof(from_tag), "%s", s->rtpengine_from_tag);
    snprintf(to_tag, sizeof(to_tag), "%s", s->rtpengine_to_tag);
    CC_SESSION_UNLOCK(s);

    if (!offered) {
        PJ_LOG(2, (THIS_FILE, "[RTPENGINE] answer skipped — no offer yet"));
        free(sdp_out);
        return PJ_EINVALIDOP;
    }

    if (ng_command("answer", call_id, from_tag, to_tag, sdp, 1,
                   sdp_out, CC_NG_SDP_MAX, NULL, 0) != 0) {
        free(sdp_out);
        return PJ_EUNKNOWN;
    }

    if (parse_sdp_endpoint(s, sdp_out, &ep) != 0) {
        PJ_LOG(1, (THIS_FILE, "[RTPENGINE] answer: could not parse returned SDP"));
        free(sdp_out);
        return PJ_EINVAL;
    }
    free(sdp_out);
    sdp_out = NULL;

    CC_SESSION_LOCK(s);
    if (!s->rtpengine_deleted) {
        int old_port = s->rtpengine_ep_a.valid ? s->rtpengine_ep_a.port : 0;
        s->rtpengine_ep_a = ep;
        s->rtpengine_answered = 1;
        if (old_port != 0 && old_port != ep.port) {
            s->rtpengine_a_ep_changed = 1;
            PJ_LOG(2, (THIS_FILE,
                       "[RTPENGINE] A-facing port changed %d → %d "
                       "(re-advertise + restart play required)",
                       old_port, ep.port));
        }
    }
    CC_SESSION_UNLOCK(s);

    PJ_LOG(3, (THIS_FILE,
               "[RTPENGINE] answer ok call-id=%s A-facing %s:%d",
               call_id, ep.ip, ep.port));
    return PJ_SUCCESS;
}

int cc_rtpengine_consume_a_ep_changed(cc_session_t *s)
{
    int changed = 0;

    if (!s)
        return 0;
    CC_SESSION_LOCK(s);
    changed = s->rtpengine_a_ep_changed;
    s->rtpengine_a_ep_changed = 0;
    CC_SESSION_UNLOCK(s);
    return changed;
}

pj_status_t cc_rtpengine_offer_from_sdp(cc_session_t *s, const pjmedia_sdp_session *sdp)
{
    char *buf;
    pj_status_t status;

    buf = (char *)malloc(CC_NG_SDP_MAX);
    if (!buf)
        return PJ_ENOMEM;
    if (print_sdp(sdp, buf, CC_NG_SDP_MAX) < 0) {
        free(buf);
        return PJ_EINVAL;
    }
    status = cc_rtpengine_offer(s, buf);
    free(buf);
    return status;
}

pj_status_t cc_rtpengine_answer_from_sdp(cc_session_t *s, const pjmedia_sdp_session *sdp)
{
    char *buf;
    pj_status_t status;

    buf = (char *)malloc(CC_NG_SDP_MAX);
    if (!buf)
        return PJ_ENOMEM;
    if (print_sdp(sdp, buf, CC_NG_SDP_MAX) < 0) {
        free(buf);
        return PJ_EINVAL;
    }
    status = cc_rtpengine_answer(s, buf);
    free(buf);
    return status;
}

/*
 * Apply the PCMA+telephone-event codec policy to an SDP text buffer in place.
 *
 * The 200 OK we send A is restricted after RTPengine has already answered, so
 * without this RTPengine keeps A's original list (e.g. "9 101 0 8 3"), picks
 * A's first codec for the A direction and transcodes every prompt into it —
 * observed as G.722 (PT 9) arriving at a phone that was offered PCMA only.
 * Returns 0 if buf was rewritten, -1 if it was left untouched.
 */
static int restrict_sdp_text_pcma_te(cc_session_t *s, char *buf, size_t cap,
                                     const char *tag)
{
    pjmedia_sdp_session *parsed = NULL;
    pj_pool_t *tmp;
    char *copy;
    size_t len;
    int n;

    if (!s || !s->pool || !buf)
        return -1;
    len = strlen(buf);
    if (len == 0 || len >= cap)
        return -1;

    /* Own pool: pjmedia_sdp_parse keeps pointers into the input buffer and
     * s->pool is not safe to use from worker threads. */
    tmp = pj_pool_create(s->pool->factory, "re-sdp-fmt",
                         (pj_size_t)(len + 1024), 512, NULL);
    if (!tmp)
        return -1;

    copy = (char *)pj_pool_alloc(tmp, (pj_size_t)len + 1);
    if (!copy) {
        pj_pool_release(tmp);
        return -1;
    }
    memcpy(copy, buf, len + 1);

    if (pjmedia_sdp_parse(tmp, copy, len, &parsed) != PJ_SUCCESS || !parsed) {
        pj_pool_release(tmp);
        return -1;
    }

    cc_sdp_restrict_audio_pcma_te(tmp, parsed, tag);

    n = print_sdp(parsed, buf, cap);
    pj_pool_release(tmp);
    if (n < 0) {
        /* buf may be partially written — caller must not use it. */
        return -1;
    }
    return 0;
}

pj_status_t cc_rtpengine_offer_from_rdata(cc_session_t *s, pjsip_rx_data *rdata)
{
    char *buf;
    pj_status_t status;

    buf = (char *)malloc(CC_NG_SDP_MAX);
    if (!buf)
        return PJ_ENOMEM;
    if (copy_rdata_sdp(rdata, buf, CC_NG_SDP_MAX) < 0) {
        free(buf);
        return PJ_ENOTFOUND;
    }

    /* cc_rtpengine_offer() applies the PCMA+telephone-event restriction. */
    status = cc_rtpengine_offer(s, buf);
    free(buf);
    return status;
}

/*
 * Softphones often put loopback or RFC1918 in c=/o=. RE would briefly send
 * RTP there (destination thrash) before latching. Prefer the SIP packet
 * source (NAT path Kamailio saw), even when that source is also private.
 */
static int ipv4_parse_octets(const char *s, unsigned *a, unsigned *b,
                             unsigned *c, unsigned *d, size_t *len_out)
{
    char *end;
    unsigned long v[4];
    int i;
    const char *p = s;

    for (i = 0; i < 4; i++) {
        if (i > 0) {
            if (*p != '.')
                return -1;
            p++;
        }
        if (*p < '0' || *p > '9')
            return -1;
        v[i] = strtoul(p, &end, 10);
        if (end == p || v[i] > 255)
            return -1;
        p = end;
    }
    *a = (unsigned)v[0];
    *b = (unsigned)v[1];
    *c = (unsigned)v[2];
    *d = (unsigned)v[3];
    if (len_out)
        *len_out = (size_t)(p - s);
    return 0;
}

static int ipv4_is_loopback_or_unspec(unsigned a, unsigned b, unsigned c,
                                      unsigned d)
{
    return a == 127 || (a == 0 && b == 0 && c == 0 && d == 0);
}

/*
 * Only rewrite loopback/0.0.0.0 in B answer SDP.
 * Do NOT rewrite RFC1918: B's 200 arrives via Kamailio, so pkt_info.src is
 * often CC_LOCAL_HOST / SBC (e.g. 10.20.10.120) — replacing private c= with
 * that sends RE media at the SIP proxy (pcap: RE→10.20.10.120:50514) and can
 * disturb the session. Port-latching learns the real phone RTP (10.20.10.130).
 */
static int ipv4_is_replaceable_sdp_addr(const char *ip, size_t len)
{
    unsigned a, b, c, d;
    size_t parsed;
    char tmp[16];

    if (len == 0 || len >= sizeof(tmp))
        return 0;
    memcpy(tmp, ip, len);
    tmp[len] = '\0';
    if (ipv4_parse_octets(tmp, &a, &b, &c, &d, &parsed) != 0 || parsed != len)
        return 0;
    return ipv4_is_loopback_or_unspec(a, b, c, d);
}

static int ipv4_src_usable_for_sdp_replace(const char *src_ip)
{
    unsigned a, b, c, d;
    size_t parsed;
    const char *local_host;
    const char *sbc_host;

    if (!src_ip || !src_ip[0])
        return 0;
    if (ipv4_parse_octets(src_ip, &a, &b, &c, &d, &parsed) != 0 ||
        src_ip[parsed] != '\0')
        return 0;
    if (ipv4_is_loopback_or_unspec(a, b, c, d))
        return 0;

    /* SIP from Kamailio / self is not the phone's media address. */
    local_host = cc_cfg_local_host();
    sbc_host = cc_cfg_sbc_host();
    if (local_host && local_host[0] && strcmp(src_ip, local_host) == 0)
        return 0;
    if (sbc_host && sbc_host[0] && strcmp(src_ip, sbc_host) == 0)
        return 0;
    return 1;
}

/** Replace loopback/unspec c=/o= IPv4 with src_ip. Returns replacement count. */
static int sdp_replace_bad_connection_ips(char *sdp, size_t cap,
                                          const char *src_ip)
{
    char *line = sdp;
    int nrep = 0;
    size_t src_len;

    if (!sdp || !src_ip || !ipv4_src_usable_for_sdp_replace(src_ip))
        return 0;
    src_len = strlen(src_ip);

    while (line && *line) {
        char *nl = strchr(line, '\n');
        char *eol = nl ? nl : line + strlen(line);
        char *ip4;
        char *ip_start;
        size_t old_len;
        unsigned a, b, c, d;
        size_t parsed;
        size_t line_off, ip_off, tail_len, need;

        if (strncmp(line, "c=IN IP4 ", 9) == 0) {
            ip4 = line + 9;
        } else if (line[0] == 'o' && line[1] == '=') {
            char saved = *eol;
            *eol = '\0';
            ip4 = strstr(line, "IN IP4 ");
            *eol = saved;
            if (!ip4)
                ip4 = NULL;
            else
                ip4 += 7;
        } else {
            ip4 = NULL;
        }

        if (ip4 && ip4 < eol) {
            ip_start = ip4;
            while (ip4 < eol && *ip4 != ' ' && *ip4 != '\r' && *ip4 != '\t')
                ip4++;
            old_len = (size_t)(ip4 - ip_start);
            if (old_len > 0 &&
                ipv4_parse_octets(ip_start, &a, &b, &c, &d, &parsed) == 0 &&
                parsed == old_len &&
                ipv4_is_replaceable_sdp_addr(ip_start, old_len) &&
                !(old_len == src_len && memcmp(ip_start, src_ip, src_len) == 0))
            {
                line_off = (size_t)(line - sdp);
                ip_off = (size_t)(ip_start - sdp);
                tail_len = strlen(ip_start + old_len);
                need = ip_off + src_len + tail_len + 1;
                if (need < cap) {
                    memmove(sdp + ip_off + src_len,
                            sdp + ip_off + old_len,
                            tail_len + 1);
                    memcpy(sdp + ip_off, src_ip, src_len);
                    nrep++;
                    line = sdp + line_off;
                    nl = strchr(line, '\n');
                    eol = nl ? nl : line + strlen(line);
                }
            }
        }

        if (!nl)
            break;
        line = nl + 1;
    }
    return nrep;
}

pj_status_t cc_rtpengine_answer_from_rdata(cc_session_t *s, pjsip_rx_data *rdata)
{
    char *buf;
    pj_status_t status;
    const char *src_ip;
    int nrep;

    buf = (char *)malloc(CC_NG_SDP_MAX);
    if (!buf)
        return PJ_ENOMEM;
    if (copy_rdata_sdp(rdata, buf, CC_NG_SDP_MAX) < 0) {
        free(buf);
        return PJ_ENOTFOUND;
    }

    src_ip = NULL;
    if (rdata && rdata->pkt_info.src_name[0] != '\0' &&
        ipv4_src_usable_for_sdp_replace(rdata->pkt_info.src_name))
        src_ip = rdata->pkt_info.src_name;

    nrep = src_ip ? sdp_replace_bad_connection_ips(buf, CC_NG_SDP_MAX, src_ip)
                  : 0;
    if (nrep > 0) {
        PJ_LOG(2, (THIS_FILE,
                   "[RTPENGINE] answer SDP: replaced loopback c=/o= "
                   "with SIP src %s (%d place(s))",
                   src_ip, nrep));
    } else if (rdata && rdata->pkt_info.src_name[0] != '\0' &&
               strstr(buf, "127.0.0.1") != NULL &&
               !ipv4_src_usable_for_sdp_replace(rdata->pkt_info.src_name))
    {
        PJ_LOG(2, (THIS_FILE,
                   "[RTPENGINE] answer SDP: left loopback/private c= — SIP src "
                   "%s is local/SBC (rely on port-latching)",
                   rdata->pkt_info.src_name));
    }

    status = cc_rtpengine_answer(s, buf);
    free(buf);
    return status;
}

void cc_rtpengine_query(cc_session_t *s)
{
    char call_id[128], from_tag[64], to_tag[64];
    char raw[4096];
    int offered;

    if (!cc_rtpengine_enabled() || !s)
        return;

    CC_SESSION_LOCK(s);
    offered = s->rtpengine_offered && !s->rtpengine_deleted;
    snprintf(call_id, sizeof(call_id), "%s", s->rtpengine_call_id);
    snprintf(from_tag, sizeof(from_tag), "%s", s->rtpengine_from_tag);
    snprintf(to_tag, sizeof(to_tag), "%s", s->rtpengine_to_tag);
    CC_SESSION_UNLOCK(s);

    if (!offered || call_id[0] == '\0')
        return;

    if (ng_command("query", call_id, from_tag, to_tag, NULL, 0,
                   NULL, 0, raw, sizeof(raw)) != 0)
        return;

    PJ_LOG(4, (THIS_FILE, "[RTPENGINE] query call-id=%s %s", call_id, raw));
}

void cc_rtpengine_delete(cc_session_t *s)
{
    char call_id[128], from_tag[64], to_tag[64];
    int do_it = 0;

    if (!s)
        return;

    CC_SESSION_LOCK(s);
    if (s->rtpengine_offered && !s->rtpengine_deleted &&
        s->rtpengine_call_id[0] != '\0')
    {
        snprintf(call_id, sizeof(call_id), "%s", s->rtpengine_call_id);
        snprintf(from_tag, sizeof(from_tag), "%s", s->rtpengine_from_tag);
        snprintf(to_tag, sizeof(to_tag), "%s", s->rtpengine_to_tag);
        s->rtpengine_deleted = 1;
        s->rtpengine_media_blocked = 0;
        do_it = 1;
    }
    CC_SESSION_UNLOCK(s);

    if (!do_it)
        return;

    re_map_del(s);

    if (!cc_rtpengine_enabled())
        return;

    (void)ng_command("delete", call_id, from_tag, to_tag, NULL, 0,
                     NULL, 0, NULL, 0);
    PJ_LOG(3, (THIS_FILE, "[RTPENGINE] delete call-id=%s", call_id));
}

static void resolve_media_file(const char *wav_path, char *out, size_t outlen)
{
    const char *dir = cc_cfg_rtpengine_media_dir();
    char resolved[PATH_MAX];

    if (!wav_path || !out || outlen == 0)
        return;
    if (dir && dir[0] != '\0') {
        const char *base = strrchr(wav_path, '/');
        base = base ? base + 1 : wav_path;
        snprintf(out, outlen, "%s/%s", dir, base);
        return;
    }
    if (realpath(wav_path, resolved) != NULL)
        snprintf(out, outlen, "%s", resolved);
    else
        snprintf(out, outlen, "%s", wav_path);
}

pj_status_t cc_rtpengine_play(cc_session_t *s, int for_a_leg,
                              const char *wav_path, int loop, int *duration_ms)
{
    char call_id[128], tag[64], file[PATH_MAX];
    char req[CC_NG_MAX_PKT];
    char resp[CC_NG_MAX_PKT];
    char cookie[CC_NG_COOKIE_MAX];
    be_buf_t b;
    size_t resp_len = 0;
    unsigned seq;
    const char *body;
    size_t body_len;
    const char *result;
    size_t result_len;
    long dur = 0;

    if (!cc_rtpengine_enabled() || !s || !wav_path || !wav_path[0])
        return PJ_ENOTSUP;

    CC_SESSION_LOCK(s);
    /* Allow play while torn_down: validation-reject sets torn_down before the
     * treatment announcement so B is not originated, but RTPengine must stay
     * up until mark_end. Only refuse if the ng call was already deleted. */
    if (!s->rtpengine_offered || s->rtpengine_deleted) {
        int offered = s->rtpengine_offered;
        int deleted = s->rtpengine_deleted;
        int torn = s->torn_down;
        CC_SESSION_UNLOCK(s);
        PJ_LOG(2, (THIS_FILE,
                   "[RTPENGINE] play skipped — offered=%d deleted=%d torn=%d file=%s",
                   offered, deleted, torn, wav_path));
        return PJ_EINVALIDOP;
    }
    snprintf(call_id, sizeof(call_id), "%s", s->rtpengine_call_id);
    snprintf(tag, sizeof(tag), "%s",
             for_a_leg ? s->rtpengine_from_tag : s->rtpengine_to_tag);
    CC_SESSION_UNLOCK(s);

    resolve_media_file(wav_path, file, sizeof(file));

    if (access(file, R_OK) != 0) {
        PJ_LOG(1, (THIS_FILE,
                   "[RTPENGINE] play media file missing/unreadable path=%s "
                   "(mapped from %s): %s",
                   file, wav_path, strerror(errno)));
        return PJ_ENOTFOUND;
    }

    if (cc_rtpengine_init() != PJ_SUCCESS)
        return PJ_EUNKNOWN;

    seq = atomic_fetch_add_explicit(&g_cookie_seq, 1u, memory_order_relaxed);
    snprintf(cookie, sizeof(cookie), "cc%u-%u", (unsigned)getpid(), seq);

    b.buf = req;
    b.len = 0;
    b.cap = sizeof(req);
    b.overflow = 0;

    be_char(&b, 'd');
    be_str(&b, "call-id");
    be_str(&b, call_id);
    be_str(&b, "command");
    be_str(&b, "play media");
    be_str(&b, "file");
    be_str(&b, file);
    /* Do not use block-egress: it can suppress announcement packets toward
     * the subscriber on some RTPengine builds while still returning ok. */
    be_str(&b, "from-tag");
    be_str(&b, tag);
    be_str(&b, "repeat-times");
    be_int(&b, loop ? 9999L : 1L);
    be_char(&b, 'e');

    if (b.overflow ||
        ng_transact(req, b.len, cookie, strlen(cookie), resp, sizeof(resp), &resp_len) != 0)
    {
        PJ_LOG(1, (THIS_FILE, "[RTPENGINE] play media failed file=%s", file));
        return PJ_EUNKNOWN;
    }

    body = memchr(resp, ' ', resp_len);
    if (!body)
        return PJ_EUNKNOWN;
    body++;
    body_len = (size_t)(resp + resp_len - body);

    if (be_dict_get_str(body, body_len, "result", &result, &result_len) != 0 ||
        !(result_len == 2 && memcmp(result, "ok", 2) == 0))
    {
        const char *reason = NULL;
        size_t rlen = 0;
        if (be_dict_get_str(body, body_len, "reason", &reason, &rlen) != 0 &&
            be_dict_get_str(body, body_len, "error-reason", &reason, &rlen) != 0)
        {
            reason = result;
            rlen = result_len;
        }
        PJ_LOG(1, (THIS_FILE,
                   "[RTPENGINE] play media not ok file=%s reason=%.*s",
                   file, (int)rlen, reason ? reason : ""));
        return PJ_EUNKNOWN;
    }

    if (be_dict_get_int(body, body_len, "duration", &dur) == 0 && dur > 0 &&
        duration_ms)
        *duration_ms = (int)dur;
    else if (duration_ms && *duration_ms <= 0)
        *duration_ms = 4000;

    PJ_LOG(3, (THIS_FILE,
               "[RTPENGINE] play %s-leg file=%s duration_ms=%d loop=%d",
               for_a_leg ? "A" : "B", file,
               duration_ms ? *duration_ms : 0, loop));
    return PJ_SUCCESS;
}

void cc_rtpengine_stop_play(cc_session_t *s, int for_a_leg)
{
    char call_id[128], tag[64];

    if (!cc_rtpengine_enabled() || !s)
        return;

    CC_SESSION_LOCK(s);
    if (!s->rtpengine_offered || s->rtpengine_deleted) {
        CC_SESSION_UNLOCK(s);
        return;
    }
    snprintf(call_id, sizeof(call_id), "%s", s->rtpengine_call_id);
    snprintf(tag, sizeof(tag), "%s",
             for_a_leg ? s->rtpengine_from_tag : s->rtpengine_to_tag);
    CC_SESSION_UNLOCK(s);

    (void)ng_command("stop media", call_id, tag, NULL, NULL, 0,
                     NULL, 0, NULL, 0);
}

/*
 * Collect-phase media isolation.
 *
 * During the B collect announcement RTPengine otherwise relays A's audio to B
 * at the same time as `play media`, so B receives two concurrent SSRCs.
 * Endpoints that lock onto the first SSRC (Linphone) never render the prompt;
 * endpoints that mix (Zoiper) render it choppy.
 *
 * Scope matters: per the ng protocol, directional methods IGNORE a supplied
 * from-tag unless the `directional` flag is present, and supplying a to-tag as
 * well selects a single media flow. Sending both tags without `directional`
 * blocked the whole call, which also stopped B's RFC2833 from reaching
 * DTMF-log-dest. Block only the A party, so B -> RE is never touched.
 */
static void cc_rtpengine_set_a_media_block(cc_session_t *s, int block)
{
    char call_id[128], from_tag[64];
    int do_it = 0;

    if (!cc_rtpengine_enabled() || !s)
        return;

    CC_SESSION_LOCK(s);
    if (s->rtpengine_offered && !s->rtpengine_deleted &&
        s->rtpengine_call_id[0] != '\0' &&
        s->rtpengine_from_tag[0] != '\0' &&
        s->rtpengine_media_blocked != block)
    {
        snprintf(call_id, sizeof(call_id), "%s", s->rtpengine_call_id);
        snprintf(from_tag, sizeof(from_tag), "%s", s->rtpengine_from_tag);
        s->rtpengine_media_blocked = block;
        do_it = 1;
    }
    CC_SESSION_UNLOCK(s);

    if (!do_it)
        return;

    if (ng_command_flag(block ? "block media" : "unblock media",
                        call_id, from_tag, NULL, NULL, 0,
                        NULL, 0, NULL, 0, "directional") != 0)
    {
        CC_SESSION_LOCK(s);
        s->rtpengine_media_blocked = block ? 0 : 1;
        CC_SESSION_UNLOCK(s);
        PJ_LOG(2, (THIS_FILE, "[RTPENGINE] %s media failed call-id=%s",
                   block ? "block" : "unblock", call_id));
        return;
    }

    PJ_LOG(3, (THIS_FILE,
               "[RTPENGINE] %s media call-id=%s from-tag=%s directional "
               "(A-leg only)",
               block ? "block" : "unblock", call_id, from_tag));
}

void cc_rtpengine_block_media(cc_session_t *s)
{
    cc_rtpengine_set_a_media_block(s, 1);
}

void cc_rtpengine_unblock_media(cc_session_t *s)
{
    cc_rtpengine_set_a_media_block(s, 0);
}

static int json_get_str(const char *json, const char *key, char *out, size_t outlen)
{
    char pat[80];
    const char *p, *q;

    if (!json || !key || !out || outlen == 0)
        return -1;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p)
        return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p)
        return -1;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '"')
        return -1;
    p++;
    q = strchr(p, '"');
    if (!q)
        return -1;
    if ((size_t)(q - p) >= outlen)
        return -1;
    memcpy(out, p, (size_t)(q - p));
    out[q - p] = '\0';
    return 0;
}

static int json_get_int(const char *json, const char *key, long *out)
{
    char pat[80];
    const char *p;
    char *end;

    if (!json || !key || !out)
        return -1;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p)
        return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p)
        return -1;
    p++;
    *out = strtol(p, &end, 10);
    if (end == p)
        return -1;
    return 0;
}

static char dtmf_event_to_digit(long event)
{
    if (event >= 0 && event <= 9)
        return (char)('0' + event);
    if (event == 10)
        return '*';
    if (event == 11)
        return '#';
    if (event >= 12 && event <= 15)
        return (char)('A' + (event - 12));
    return '\0';
}

static void *dtmf_thread_main(void *arg)
{
    char buf[2048];
    pj_thread_desc desc;
    pj_thread_t *pj_thr = NULL;
    (void)arg;

    /*
     * Raw pthread: must register before any PJ_LOG / pjlib use or pjlib
     * asserts (SIGABRT) — seen when the first DTMF-log-dest JSON arrived.
     */
    pj_bzero(desc, sizeof(desc));
    if (pj_thread_register("cc_re_dtmf", desc, &pj_thr) != PJ_SUCCESS) {
        /* Cannot PJ_LOG safely here; abort receiving rather than crash later. */
        return NULL;
    }

    while (g_dtmf_sock >= 0) {
        ssize_t n = recv(g_dtmf_sock, buf, sizeof(buf) - 1, 0);
        char callid[128];
        char source_tag[64];
        char from_tag[64];
        char to_tag[64];
        long event = -1;
        long duration = -1;
        unsigned serial = 0;
        cc_session_t *s;
        char digit;
        cc_event_t ev;
        int for_a;
        pjsua_call_id call_a;
        pjsua_call_id call_b;
        int mca_waiting;
        int accepted;
        int posted;

        if (n <= 0) {
            if (g_dtmf_sock < 0)
                break;
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;
        }
        buf[n] = '\0';

        callid[0] = '\0';
        source_tag[0] = '\0';
        if (json_get_str(buf, "callid", callid, sizeof(callid)) != 0) {
            PJ_LOG(2, (THIS_FILE,
                       "[RTPENGINE] DTMF notify ignored — no callid (%zd bytes)",
                       n));
            continue;
        }
        if (json_get_int(buf, "event", &event) != 0) {
            PJ_LOG(2, (THIS_FILE,
                       "[RTPENGINE] DTMF notify ignored — no event callid=%s",
                       callid));
            continue;
        }
        (void)json_get_str(buf, "source_tag", source_tag, sizeof(source_tag));
        (void)json_get_int(buf, "duration", &duration);

        if (strncmp(callid, "cc-", 3) != 0) {
            PJ_LOG(2, (THIS_FILE,
                       "[RTPENGINE] DTMF notify ignored — foreign callid=%s",
                       callid));
            continue;
        }
        serial = (unsigned)strtoul(callid + 3, NULL, 10);
        if (serial == 0)
            continue;

        digit = dtmf_event_to_digit(event);
        if (!digit) {
            PJ_LOG(3, (THIS_FILE,
                       "[RTPENGINE] DTMF notify ignored — event=%ld callid=%s",
                       event, callid));
            continue;
        }

        s = re_map_find(serial);
        if (!s) {
            PJ_LOG(2, (THIS_FILE,
                       "[RTPENGINE] DTMF notify ignored — no session "
                       "callid=%s digit=%c tag=%s",
                       callid, digit, source_tag));
            continue;
        }

        /*
         * Prefer exact tag match against the session's RE tags (a-N / b-N).
         * Fall back: leading 'a' → A (MCA); otherwise B (collect). If the
         * tag is missing, route by call phase (MCA wait → A, else B).
         */
        CC_SESSION_LOCK(s);
        snprintf(from_tag, sizeof(from_tag), "%s", s->rtpengine_from_tag);
        snprintf(to_tag, sizeof(to_tag), "%s", s->rtpengine_to_tag);
        call_a = s->call_a;
        call_b = s->call_b;
        mca_waiting = s->mca_waiting;
        accepted = s->accepted;
        CC_SESSION_UNLOCK(s);

        if (source_tag[0] != '\0' && from_tag[0] != '\0' &&
            strcmp(source_tag, from_tag) == 0)
            for_a = 1;
        else if (source_tag[0] != '\0' && to_tag[0] != '\0' &&
                 strcmp(source_tag, to_tag) == 0)
            for_a = 0;
        else if (source_tag[0] == 'a' || source_tag[0] == 'A')
            for_a = 1;
        else if (source_tag[0] == 'b' || source_tag[0] == 'B')
            for_a = 0;
        else if (mca_waiting && !accepted)
            for_a = 1;
        else
            for_a = 0;

        memset(&ev, 0, sizeof(ev));
        ev.type = CC_EV_RTPENGINE_DTMF;
        ev.session = s;
        ev.session_serial = s->session_serial;
        ev.sip_code = (int)(unsigned char)digit;
        ev.call_a = PJSUA_INVALID_ID;
        ev.call_b = PJSUA_INVALID_ID;
        if (for_a)
            ev.call_a = call_a;
        else
            ev.call_b = call_b;
        snprintf(ev.reason, sizeof(ev.reason), "rtpengine-dtmf");

        PJ_LOG(3, (THIS_FILE,
                   "[RTPENGINE] DTMF notify callid=%s digit=%c tag=%s "
                   "leg=%s duration=%ldms → post",
                   callid, digit, source_tag[0] ? source_tag : "(none)",
                   for_a ? "A" : "B", duration));

        posted = (cc_worker_post(&ev) == 0);
        if (!posted)
            PJ_LOG(1, (THIS_FILE,
                       "[RTPENGINE] DTMF notify post failed callid=%s digit=%c",
                       callid, digit));
    }
    return NULL;
}

static int dtmf_listen_start(void)
{
    struct sockaddr_in addr;
    int port = cc_cfg_rtpengine_dtmf_port();

    if (g_dtmf_sock >= 0)
        return 0;

    g_dtmf_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_dtmf_sock < 0) {
        PJ_LOG(1, (THIS_FILE, "[RTPENGINE] DTMF socket failed: %s",
                   strerror(errno)));
        return -1;
    }
    /*
     * Do NOT SO_REUSEADDR: multiple B2BUA instances sharing the same DTMF
     * port silently steal each other's notifies (digit on wire, no accept).
     * Bind failure here is intentional — give each instance a unique
     * CC_RTPENGINE_DTMF_PORT.
     */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(g_dtmf_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        PJ_LOG(1, (THIS_FILE,
                   "[RTPENGINE] DTMF bind :%d failed: %s "
                   "(set a unique CC_RTPENGINE_DTMF_PORT per instance)",
                   port, strerror(errno)));
        close(g_dtmf_sock);
        g_dtmf_sock = -1;
        return -1;
    }
    if (pthread_create(&g_dtmf_thread, NULL, dtmf_thread_main, NULL) != 0) {
        close(g_dtmf_sock);
        g_dtmf_sock = -1;
        return -1;
    }
    g_dtmf_thread_started = 1;
    PJ_LOG(3, (THIS_FILE,
               "[RTPENGINE] DTMF listener UDP :%d dest=%s",
               port, cc_cfg_rtpengine_dtmf_dest()));
    return 0;
}

static void dtmf_listen_stop(void)
{
    int sock = g_dtmf_sock;
    g_dtmf_sock = -1;
    if (sock >= 0)
        close(sock);
    if (g_dtmf_thread_started) {
        pthread_join(g_dtmf_thread, NULL);
        g_dtmf_thread_started = 0;
    }
}

