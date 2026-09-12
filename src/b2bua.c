/*
 * b2bua.c — Global PJSUA callbacks + B-leg origination
 *
 * PJSUA uses a single global pjsua_callback struct. Each callback
 * resolves which session the call belongs to via call user_data, then
 * dispatches to the appropriate leg handler.
 * Async work (UPDATE retries, ack watchdog) posted to worker pool.
 *
 * Call user_data layout:
 *   Leg-A:  pointer to cc_session_t, with session->call_a == this call_id
 *   Leg-B:  pointer to cc_session_t, with session->call_b == this call_id
 */
#include "b2bua.h"
#include "handlers.h"
#include "utils.h"
#include "config.h"
#include "validation.h"
#include "validation_async.h"
#include "api_mapping.h"
#include "runtime_config.h"
#include "rtpengine.h"
#include "worker.h"

#include <pjsua-lib/pjsua.h>
#include <pjsua-lib/pjsua_internal.h>
#include <pjsip/sip_msg.h>
#include <pjsip/sip_event.h>
#include <pjsip/sip_endpoint.h>
#include <pjsip-ua/sip_inv.h>
#include <pjmedia/sdp_neg.h>
#include <pjmedia/sdp.h>
#include <pj/timer.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>

#define THIS_FILE "b2bua.c"
#define CC_SDP_SESSION_NAME "ccmedia"

/*
 * Kamailio issues public GRUUs shaped like
 *   sip:<user>@[10.20.10.120:5061];gr=urn:uuid:...
 * with host:port wrapped in square brackets, which RFC 3261 reserves for IPv6
 * literals. PJSIP parses that as host "10.20.10.120:5061" with no port and
 * copies it into the dialog's remote target, so our ACK carries a request-URI
 * Kamailio will not route. B then retransmits its 200 OK until it gives up and
 * sends BYE — an ~8 s call that never completes. Unwrap the brackets before
 * the dialog layer reads the response; the ;gr= parameter is left intact so
 * Kamailio can still resolve the contact it issued.
 */
static pj_bool_t cc_unwrap_bracketed_ipv4_uri(pj_pool_t *pool,
                                              pjsip_sip_uri *uri)
{
    const char *host;
    pj_ssize_t host_len = -1;
    pj_ssize_t i;
    int dots = 0;
    int port = 0;
    char *fixed;

    if (!pool || !uri || uri->port != 0 || uri->host.slen < 9)
        return PJ_FALSE;

    host = uri->host.ptr;

    for (i = 0; i < uri->host.slen; i++) {
        if (host[i] == ':') {
            if (host_len >= 0)
                return PJ_FALSE;    /* more than one colon: real IPv6 */
            host_len = i;
        } else if (host[i] == '.') {
            if (host_len < 0)
                dots++;
        }
    }
    if (host_len <= 0 || dots != 3 || host_len >= 64)
        return PJ_FALSE;

    for (i = host_len + 1; i < uri->host.slen; i++) {
        if (host[i] < '0' || host[i] > '9')
            return PJ_FALSE;
        port = port * 10 + (host[i] - '0');
        if (port > 65535)
            return PJ_FALSE;
    }
    if (port <= 0)
        return PJ_FALSE;

    fixed = (char *)pj_pool_alloc(pool, (pj_size_t)host_len + 1);
    if (!fixed)
        return PJ_FALSE;
    pj_memcpy(fixed, host, (pj_size_t)host_len);
    fixed[host_len] = '\0';

    uri->host.ptr = fixed;
    uri->host.slen = host_len;
    uri->port = port;
    return PJ_TRUE;
}

static pj_bool_t cc_on_rx_response_fix_contact(pjsip_rx_data *rdata)
{
    pjsip_msg *msg = rdata->msg_info.msg;
    pjsip_contact_hdr *h;

    if (!msg || msg->type != PJSIP_RESPONSE_MSG)
        return PJ_FALSE;

    h = (pjsip_contact_hdr *)pjsip_msg_find_hdr(msg, PJSIP_H_CONTACT, NULL);
    while (h) {
        if (h->uri &&
            (PJSIP_URI_SCHEME_IS_SIP(h->uri) || PJSIP_URI_SCHEME_IS_SIPS(h->uri)))
        {
            pjsip_sip_uri *u = (pjsip_sip_uri *)pjsip_uri_get_uri(h->uri);
            if (cc_unwrap_bracketed_ipv4_uri(rdata->tp_info.pool, u)) {
                PJ_LOG(3, (THIS_FILE,
                           "[CONTACT-FIX] %d Contact had bracketed IPv4 — "
                           "target now %.*s:%d",
                           msg->line.status.code,
                           (int)u->host.slen, u->host.ptr, u->port));
            }
        }
        h = (pjsip_contact_hdr *)pjsip_msg_find_hdr(msg, PJSIP_H_CONTACT,
                                                    h->next);
    }
    return PJ_FALSE;    /* never consume the message */
}

static pjsip_module cc_contact_fix_mod = {
    NULL, NULL,                             /* prev, next          */
    { "mod-cc-contact-fix", 18 },           /* name                */
    -1,                                     /* id                  */
    PJSIP_MOD_PRIORITY_TRANSPORT_LAYER + 1, /* before dialog layer */
    NULL,                                   /* load                */
    NULL,                                   /* start               */
    NULL,                                   /* stop                */
    NULL,                                   /* unload              */
    NULL,                                   /* on_rx_request       */
    &cc_on_rx_response_fix_contact,         /* on_rx_response      */
    NULL,                                   /* on_tx_request       */
    NULL,                                   /* on_tx_response      */
    NULL                                    /* on_tsx_state        */
};

pj_status_t cc_sip_contact_fix_install(void)
{
    return pjsip_endpt_register_module(pjsua_get_pjsip_endpt(),
                                       &cc_contact_fix_mod);
}

static void cc_rewrite_sdp_audio_endpoint(pj_pool_t *pool,
                                          pjmedia_sdp_session *sdp,
                                          const cc_rtp_ep_t *ep,
                                          const char *tag);

/* Patch negotiator local SDP to A-facing RTPengine ports before 200 OK. */
static pj_status_t cc_patch_a_answer_sdp_for_rtpengine(pjsua_call_id call_id,
                                                       cc_session_t *session)
{
    cc_rtp_ep_t tgt;
    pj_status_t status = PJ_ENOTFOUND;
    const pjmedia_sdp_session *local = NULL;
    pjmedia_sdp_session *cloned = NULL;
    pj_pool_t *pool = NULL;
    pjsip_inv_session *inv = NULL;
    pjmedia_sdp_neg *neg = NULL;

    if (!cc_rtpengine_enabled() || !session)
        return PJ_ENOTSUP;

    if (!cc_rtpengine_sdp_target(session, 1, &tgt) || !tgt.valid) {
        PJ_LOG(1, (THIS_FILE,
                   "[RTPENGINE] A-answer SDP patch skipped — no A-facing endpoint"));
        return PJ_ENOTFOUND;
    }

    if (call_id < 0 || !pjsua_var.mutex || !pjsua_var.calls)
        return PJ_EINVALIDOP;

    pj_mutex_lock(pjsua_var.mutex);
    {
        struct pjsua_call *call = &pjsua_var.calls[call_id];
        inv = call->inv;
        if (inv && inv->neg) {
            neg = inv->neg;
            status = pjmedia_sdp_neg_get_neg_local(neg, &local);
            if (status != PJ_SUCCESS || !local)
                status = pjmedia_sdp_neg_get_active_local(neg, &local);
            pool = inv->pool_prov ? inv->pool_prov : inv->pool;
        }
    }
    pj_mutex_unlock(pjsua_var.mutex);

    if (status != PJ_SUCCESS || !local || !pool || !neg) {
        PJ_LOG(1, (THIS_FILE,
                   "[RTPENGINE] A-answer SDP patch failed — no local SDP "
                   "(status=%d)", status));
        return status != PJ_SUCCESS ? status : PJ_ENOTFOUND;
    }

    cloned = pjmedia_sdp_session_clone(pool, local);
    if (!cloned) {
        PJ_LOG(1, (THIS_FILE, "[RTPENGINE] A-answer SDP clone failed"));
        return PJ_ENOMEM;
    }

    cc_rewrite_sdp_audio_endpoint(pool, cloned, &tgt, "A-ANSWER");

    /* Prefer replacing the negotiator answer so answer2 serializes this SDP. */
    status = pjmedia_sdp_neg_set_local_answer(pool, neg, cloned);
    if (status != PJ_SUCCESS) {
        /* Fallback: mutate the object already held by the negotiator. */
        cc_rewrite_sdp_audio_endpoint(pool, (pjmedia_sdp_session *)local,
                                      &tgt, "A-ANSWER-MUTATE");
        PJ_LOG(2, (THIS_FILE,
                   "[RTPENGINE] set_local_answer status=%d — mutated in place to %s:%d",
                   status, tgt.ip, tgt.port));
        CC_SESSION_LOCK(session);
        session->rtpengine_a_advertised = 1;
        CC_SESSION_UNLOCK(session);
        return PJ_SUCCESS;
    }

    PJ_LOG(3, (THIS_FILE,
               "[RTPENGINE] A-answer SDP patched to %s:%d before 200 OK",
               tgt.ip, tgt.port));
    CC_SESSION_LOCK(session);
    session->rtpengine_a_advertised = 1;
    CC_SESSION_UNLOCK(session);
    return PJ_SUCCESS;
}

/* Queue A 200 on the answer pool (after offer + optional SDP patch). */
static int cc_queue_a_answer(cc_session_t *session, pjsua_call_id call_a)
{
    cc_event_t ev_ans;
    long long t_queued;

    if (!session || call_a == PJSUA_INVALID_ID)
        return -1;

    memset(&ev_ans, 0, sizeof(ev_ans));
    ev_ans.type = CC_EV_RTPENGINE_A_ANSWER;
    ev_ans.session = session;
    ev_ans.session_serial = session->session_serial;
    ev_ans.call_a = call_a;
    snprintf(ev_ans.reason, sizeof(ev_ans.reason), "rtpengine-a-answer");

    CC_SESSION_LOCK(session);
    if (session->torn_down || session->call_a != call_a) {
        CC_SESSION_UNLOCK(session);
        return -1;
    }
    session->rtpengine_a_answer_pending = 1;
    CC_SESSION_UNLOCK(session);

    if (cc_worker_post(&ev_ans) != 0) {
        CC_SESSION_LOCK(session);
        session->rtpengine_a_answer_pending = 0;
        CC_SESSION_UNLOCK(session);
        return -1;
    }

    t_queued = cc_monotonic_ms();
    CC_SESSION_LOCK(session);
    session->a_answer_queued_ms = t_queued;
    CC_SESSION_UNLOCK(session);

    PJ_LOG(3, (THIS_FILE,
               "[A-TIMING] call_a=%d callId=%s phase=ANSWER_QUEUED "
               "since_cb_ms=%lld mode=rtpengine-offer-then-answer",
               call_a, session->call_id,
               session->a_invite_cb_ms > 0 ?
                   t_queued - session->a_invite_cb_ms : -1));
    return 0;
}

static int cc_admission_should_reject(unsigned *active_out,
                                      unsigned *timer_out,
                                      const char **why_out)
{
    unsigned active;
    unsigned timers = 0;
    int max_calls = cc_cfg_admission_max_calls();
    int max_timers = cc_cfg_admission_timer_heap_max();
    pjsip_endpoint *endpt;
    pj_timer_heap_t *heap;

    active = pjsua_call_get_count();
    if (active_out)
        *active_out = active;
    if (timer_out)
        *timer_out = 0;
    if (why_out)
        *why_out = NULL;

    endpt = pjsua_get_pjsip_endpt();
    if (endpt) {
        heap = pjsip_endpt_get_timer_heap(endpt);
        if (heap) {
            timers = (unsigned)pj_timer_heap_count(heap);
            if (timer_out)
                *timer_out = timers;
        }
    }

    if (max_calls > 0 && (int)active >= max_calls) {
        if (why_out)
            *why_out = "active_calls";
        return 1;
    }
    if (max_timers > 0 && (int)timers >= max_timers) {
        if (why_out)
            *why_out = "timer_heap";
        return 1;
    }
    return 0;
}

/*
 * Worker path (legacy): offer then queue 200 (RE ports in initial answer).
 */
void cc_complete_a_rtpengine_answer(cc_session_t *session,
                                    pjsua_call_id call_a,
                                    const char *sdp)
{
    if (!sdp || !sdp[0] || !session)
        return;
    CC_SESSION_LOCK(session);
    if (!session->torn_down && session->call_a == call_a &&
        !session->rtpengine_deleted)
        session->rtpengine_a_offer_pending = 1;
    else {
        CC_SESSION_UNLOCK(session);
        return;
    }
    CC_SESSION_UNLOCK(session);
    cc_complete_a_rtpengine_offer(session, call_a, sdp);
}

/*
 * Async ng offer, patch A SDP to RTPengine, then queue 200 on answer pool.
 * Media no longer waits on CONFIRMED → re-INVITE under SIP starvation.
 */
void cc_complete_a_rtpengine_offer(cc_session_t *session,
                                   pjsua_call_id call_a,
                                   const char *sdp)
{
    pj_status_t status;
    int pending = 0;
    long long t0;
    long long t1;
    long long since_cb = -1;
    long long since_200 = -1;
    pj_status_t patch_st;

    if (!session || call_a == PJSUA_INVALID_ID || !sdp || !sdp[0])
        return;

    t0 = cc_monotonic_ms();
    CC_SESSION_LOCK(session);
    pending = session->rtpengine_a_offer_pending;
    session->a_offer_start_ms = t0;
    if (session->a_invite_cb_ms > 0)
        since_cb = t0 - session->a_invite_cb_ms;
    if (session->a_200_sent_ms > 0)
        since_200 = t0 - session->a_200_sent_ms;
    if (!pending || session->torn_down || session->rtpengine_deleted ||
        session->call_a != call_a)
    {
        session->rtpengine_a_offer_pending = 0;
        CC_SESSION_UNLOCK(session);
        return;
    }
    CC_SESSION_UNLOCK(session);

    PJ_LOG(3, (THIS_FILE,
               "[A-TIMING] call_a=%d callId=%s phase=OFFER_START "
               "since_cb_ms=%lld since_200_ms=%lld",
               call_a, session->call_id, since_cb, since_200));

    if (!cc_session_call_is_current(session, call_a, 1)) {
        CC_SESSION_LOCK(session);
        session->rtpengine_a_offer_pending = 0;
        CC_SESSION_UNLOCK(session);
        return;
    }

    /* Do not keep pjsua_call_info on this stack frame during offer/answer
     * (large struct + heap SDP nesting previously blew the 128KB worker stack). */
    status = cc_rtpengine_offer(session, sdp);
    t1 = cc_monotonic_ms();
    PJ_LOG(3, (THIS_FILE,
               "[A-TIMING] call_a=%d callId=%s phase=OFFER_DONE "
               "offer_dur_ms=%lld status=%d",
               call_a, session->call_id, t1 - t0, status));

    CC_SESSION_LOCK(session);
    session->rtpengine_a_offer_pending = 0;
    session->a_offer_done_ms = t1;
    if (status != PJ_SUCCESS) {
        session->torn_down = 1;
        CC_SESSION_UNLOCK(session);
        PJ_LOG(1, (THIS_FILE,
                   "[RTPENGINE] A-leg async offer failed status=%d — drop A",
                   status));
        cc_session_mark_end(session, "FAILED", "SYSTEM_ERROR");
        {
            pjsua_call_info ci;
            if (pjsua_call_get_info(call_a, &ci) == PJ_SUCCESS &&
                ci.state != PJSIP_INV_STATE_DISCONNECTED &&
                ci.state != PJSIP_INV_STATE_NULL)
            {
                if (ci.state < PJSIP_INV_STATE_CONNECTING)
                    pjsua_call_answer(call_a, PJSIP_SC_SERVICE_UNAVAILABLE,
                                      NULL, NULL);
                else
                    cc_safe_hangup(call_a, PJSIP_SC_SERVICE_UNAVAILABLE);
            }
        }
        cc_session_invalidate_a(session, call_a);
        return;
    }
    CC_SESSION_UNLOCK(session);

    patch_st = cc_patch_a_answer_sdp_for_rtpengine(call_a, session);
    if (patch_st != PJ_SUCCESS) {
        PJ_LOG(2, (THIS_FILE,
                   "[RTPENGINE] pre-200 SDP patch status=%d — 200 may need "
                   "re-INVITE advertise", patch_st));
    }

    if (cc_queue_a_answer(session, call_a) != 0) {
        PJ_LOG(1, (THIS_FILE,
                   "[RTPENGINE] answer queue full after offer — drop A call %d",
                   call_a));
        CC_SESSION_LOCK(session);
        session->torn_down = 1;
        CC_SESSION_UNLOCK(session);
        cc_session_mark_end(session, "FAILED", "SYSTEM_ERROR");
        pjsua_call_answer(call_a, PJSIP_SC_SERVICE_UNAVAILABLE, NULL, NULL);
        cc_session_invalidate_a(session, call_a);
        return;
    }

    {
        int advertised = 0;
        CC_SESSION_LOCK(session);
        advertised = session->rtpengine_a_advertised;
        CC_SESSION_UNLOCK(session);
        PJ_LOG(3, (THIS_FILE,
                   "[A-TIMING] call_a=%d callId=%s phase=OFFER_QUEUED_ANSWER "
                   "offer_dur_ms=%lld patch_status=%d advertised=%d",
                   call_a, session->call_id, t1 - t0, patch_st, advertised));
    }
}

/*
 * Worker path (update / reinvite / local_bridge / rtpengine answer-first):
 * pjsua_call_answer2 only. Same answer-pool lane for all modes.
 * Reuses rtpengine_a_answer_pending as the deferred-answer guard.
 */
void cc_complete_a_local_answer(cc_session_t *session, pjsua_call_id call_a)
{
    pjsua_call_setting cs;
    pjsua_call_info ci;
    pj_status_t status;
    int pending = 0;
    long long t_worker;
    long long t_done;
    long long queue_wait_ms = -1;
    long long since_cb = -1;
    long long since_100 = -1;

    if (!session || call_a == PJSUA_INVALID_ID)
        return;

    t_worker = cc_monotonic_ms();

    CC_SESSION_LOCK(session);
    pending = session->rtpengine_a_answer_pending;
    session->a_200_worker_ms = t_worker;
    if (session->a_answer_queued_ms > 0)
        queue_wait_ms = t_worker - session->a_answer_queued_ms;
    if (session->a_invite_cb_ms > 0)
        since_cb = t_worker - session->a_invite_cb_ms;
    if (session->a_100_sent_ms > 0)
        since_100 = t_worker - session->a_100_sent_ms;
    if (!pending || session->torn_down || session->call_a != call_a) {
        session->rtpengine_a_answer_pending = 0;
        CC_SESSION_UNLOCK(session);
        return;
    }
    CC_SESSION_UNLOCK(session);

    PJ_LOG(3, (THIS_FILE,
               "[A-TIMING] call_a=%d callId=%s phase=200_WORKER "
               "queue_wait_ms=%lld since_cb_ms=%lld since_100_ms=%lld",
               call_a, session->call_id, queue_wait_ms, since_cb, since_100));

    if (!cc_session_call_is_current(session, call_a, 1)) {
        CC_SESSION_LOCK(session);
        session->rtpengine_a_answer_pending = 0;
        CC_SESSION_UNLOCK(session);
        return;
    }

    if (pjsua_call_get_info(call_a, &ci) != PJ_SUCCESS) {
        CC_SESSION_LOCK(session);
        session->rtpengine_a_answer_pending = 0;
        CC_SESSION_UNLOCK(session);
        return;
    }
    if (ci.state == PJSIP_INV_STATE_NULL ||
        ci.state == PJSIP_INV_STATE_DISCONNECTED ||
        ci.state == PJSIP_INV_STATE_CONFIRMED)
    {
        CC_SESSION_LOCK(session);
        session->rtpengine_a_answer_pending = 0;
        CC_SESSION_UNLOCK(session);
        PJ_LOG(3, (THIS_FILE,
                   "[A] local answer skipped — call %d state=%d",
                   call_a, (int)ci.state));
        return;
    }

    pjsua_call_setting_default(&cs);
    cs.aud_cnt = 1;
    cs.vid_cnt = 0;
    cs.txt_cnt = 0;

    /* Rtpengine: ensure RE ports are in negotiator SDP before answer2. */
    if (cc_rtpengine_enabled()) {
        pj_status_t pst = cc_patch_a_answer_sdp_for_rtpengine(call_a, session);
        if (pst != PJ_SUCCESS) {
            PJ_LOG(2, (THIS_FILE,
                       "[RTPENGINE] answer-time SDP patch status=%d call=%d",
                       pst, call_a));
        }
    }

    {
        long long lock_wait_ms = 0;
        long long t_ans = cc_monotonic_ms();
        int advertised = 0;
        status = cc_call_answer2_serialized(call_a, &cs, PJSIP_SC_OK,
                                            NULL, NULL, &lock_wait_ms);
        t_done = cc_monotonic_ms();

        CC_SESSION_LOCK(session);
        session->rtpengine_a_answer_pending = 0;
        session->a_200_sent_ms = t_done;
        advertised = session->rtpengine_a_advertised;
        since_cb = session->a_invite_cb_ms > 0 ? t_done - session->a_invite_cb_ms : -1;
        since_100 = session->a_100_sent_ms > 0 ? t_done - session->a_100_sent_ms : -1;
        CC_SESSION_UNLOCK(session);

        if (status != PJ_SUCCESS) {
            PJ_LOG(1, (THIS_FILE, "[ERROR] A-leg async 200 OK failed: %d", status));
            cc_session_mark_end(session, "FAILED", "SYSTEM_ERROR");
            CC_SESSION_LOCK(session);
            session->torn_down = 1;
            CC_SESSION_UNLOCK(session);
            cc_session_invalidate_a(session, call_a);
            return;
        }

        PJ_LOG(3, (THIS_FILE,
                   "[A-TIMING] call_a=%d callId=%s phase=200_OK "
                   "answer2_dur_ms=%lld answer2_lock_wait_ms=%lld "
                   "since_cb_ms=%lld since_100_ms=%lld "
                   "queue_wait_ms=%lld mode=%s rtpengine_sdp=%d",
                   call_a, session->call_id,
                   (t_done - t_ans) - lock_wait_ms, lock_wait_ms,
                   since_cb, since_100, queue_wait_ms,
                   cc_rtpengine_enabled() ? "rtpengine-offer-then-answer" : "local",
                   advertised));
        (void)t_worker;
    }
}

static int cc_header_name_is(const char *actual, const char *expected)
{
    pj_str_t actual_name;

    if (!actual || !expected)
        return 0;

    actual_name = pj_str((char *)actual);
    return pj_stricmp2(&actual_name, expected) == 0;
}

static int cc_add_msg_header(pj_pool_t *pool,
                             pjsua_msg_data *msg_data,
                             const char *name,
                             const char *value)
{
    pjsip_generic_string_hdr *header;
    pj_str_t header_name;
    pj_str_t header_value;

    if (!pool || !msg_data || !name || !value)
        return 0;

    header_name = pj_str((char *)name);
    header_value = pj_str((char *)value);
    header = pjsip_generic_string_hdr_create(pool,
                                              &header_name,
                                              &header_value);
    if (!header)
        return 0;

    pj_list_push_back(&msg_data->hdr_list, header);
    return 1;
}

static void cc_sdp_set_session_name(pjmedia_sdp_session *sdp,
                                    pj_pool_t *pool,
                                    const char *name)
{
    if (!sdp || !pool || !name)
        return;

    if (pj_strcmp2(&sdp->name, name) == 0)
        return;

    sdp->name = pj_strdup3(pool, name);
    PJ_LOG(3, (THIS_FILE, "[SDP] Session name set to %s", name));
}

static const char *cc_captured_header_value(const cc_session_t *session,
                                            const char *name)
{
    int i;

    if (!session || !name)
        return NULL;

    for (i = 0; i < session->fwd_hdr_count; i++) {
        if (cc_header_name_is(session->fwd_hdrs[i].name, name))
            return session->fwd_hdrs[i].value;
    }

    return NULL;
}

/* ── Incoming call ───────────────────────────────────────────────────────── */

void cc_on_incoming_call(pjsua_acc_id acc_id,
                          pjsua_call_id call_id,
                          pjsip_rx_data *rdata)
{
    pjsua_call_info  ci;
    char             raw_ruri[512] = {0};
    char             ruri_user[128] = {0};
    char             to_user[128] = {0};
    char             local_user[128] = {0};
    char             dialed_raw[128] = {0};
    char             dialed_source[16] = "none";
    char             sponsor_normalized[64] = {0};
    cc_collect_number_t collect_number;
    cc_session_t    *session;
    pjsua_call_setting cs;
    pj_status_t      status;
    long long        t_cb = cc_monotonic_ms();
    long long        t_100 = 0;
    long long        t_queued = 0;

    /* Soft admission: shed new INVITEs. PJSUA already sent auto-100 before
     * this callback (see below); a 503 after that is still fine. */
    {
        unsigned active = 0;
        unsigned timers = 0;
        const char *why = NULL;
        if (cc_admission_should_reject(&active, &timers, &why)) {
            PJ_LOG(2, (THIS_FILE,
                       "[ADMISSION] reject call_a=%d reason=%s active_calls=%u "
                       "timer_heap=%u max_calls=%d max_timers=%d",
                       call_id, why ? why : "unknown", active, timers,
                       cc_cfg_admission_max_calls(),
                       cc_cfg_admission_timer_heap_max()));
            pjsua_call_answer(call_id, PJSIP_SC_SERVICE_UNAVAILABLE, NULL, NULL);
            return;
        }
    }

    /*
     * Do NOT pjsua_call_answer(100) here.
     *
     * With default PJSUA_DISABLE_AUTO_SEND_100=0, pjsua_call.c already sent
     * 100 Trying before invoking on_incoming_call. A second answer(100)
     * produces two identical Trying responses on the wire (seen 2026-09-11
     * in sip_traffic.pcap). Kamailio's t_fr is already armed by that first
     * auto-100, which is earlier than anything we can send from this
     * callback.
     *
     * Only restore an explicit answer(100) if pjproject is rebuilt with
     * -DPJSUA_DISABLE_AUTO_SEND_100=1.
     */
    {
        unsigned active = pjsua_call_get_count();
        t_100 = cc_monotonic_ms();
        PJ_LOG(3, (THIS_FILE,
                   "[A-TIMING] call_a=%d phase=100_TRYING "
                   "cb_entry_ms=%lld since_cb_ms=%lld active_calls=%u "
                   "(PJSUA auto-100; no app answer)",
                   call_id, t_cb, t_100 - t_cb, active));
    }

    status = pjsua_call_get_info(call_id, &ci);
    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] incoming call info failed call=%d status=%d",
                   call_id, status));
        pjsua_call_answer(call_id, PJSIP_SC_SERVICE_UNAVAILABLE, NULL, NULL);
        return;
    }

    /* Guard against PJSUA slot reuse: if this slot still has a live session
     * from a previous call (e.g. CC_EV_HANGUP_A_ONLY delayed in timer heap
     * while PJSUA already recycled the slot), reject the new INVITE.
     * user_data is set at cc_on_incoming_call and cleared only in
     * cc_session_invalidate_a (DISCONNECTED callback). PJSUA can assign
     * the slot to a new INVITE during the window between pjsua_call_hangup()
     * and the DISCONNECTED callback — this guard catches that window.
     * The SBC will retry on a different slot immediately. */
    {
        cc_session_t *stale = (cc_session_t *)pjsua_call_get_user_data(call_id);
        if (stale) {
            PJ_LOG(2, (THIS_FILE,
                       "[CALL-SLOTS] slot %d still has live session=%p — "
                       "rejecting new INVITE to avoid slot reuse",
                       call_id, stale));
            pjsua_call_answer(call_id, PJSIP_SC_SERVICE_UNAVAILABLE, NULL, NULL);
            return;
        }
    }

    /* Log active call count at debug — hot path under SIPp. */
    {
        unsigned active = pjsua_call_get_count();
        PJ_LOG(4, (THIS_FILE,
                   "[CALL-SLOTS] incoming call_id=%d active_calls=%u "
                   "pjsua_max_calls=%d",
                   call_id, active, PJSUA_MAX_CALLS));
    }

    PJ_LOG(4, (THIS_FILE, "Incoming call: from=%.*s to=%.*s",
               (int)ci.remote_info.slen, ci.remote_info.ptr,
               (int)ci.local_info.slen,  ci.local_info.ptr));

    /* Extract B's number (strips collect prefix) */
    status = cc_extract_request_uri_user(rdata,
                                         raw_ruri,
                                         sizeof(raw_ruri),
                                         ruri_user,
                                         sizeof(ruri_user));
    PJ_LOG(4, (THIS_FILE,
               "[INCOMING] raw_ruri=%s",
               raw_ruri[0] ? raw_ruri : "<unavailable>"));
    if (status == PJ_SUCCESS) {
        snprintf(dialed_raw, sizeof(dialed_raw), "%s", ruri_user);
        snprintf(dialed_source, sizeof(dialed_source), "%s", "Request-URI");
        PJ_LOG(4, (THIS_FILE,
                   "[INCOMING] ruri_user=%s",
                   ruri_user));
    } else {
        PJ_LOG(4, (THIS_FILE,
                   "[INCOMING] Request-URI user unavailable; trying To header"));
    }

    if (dialed_raw[0] == '\0' &&
        cc_extract_to_header_user(rdata, to_user, sizeof(to_user)) == PJ_SUCCESS)
    {
        snprintf(dialed_raw, sizeof(dialed_raw), "%s", to_user);
        snprintf(dialed_source, sizeof(dialed_source), "%s", "To");
        PJ_LOG(3, (THIS_FILE,
                   "[INCOMING] fallback=To user=%s",
                   to_user));
    }

    if (dialed_raw[0] == '\0' &&
        cc_extract_pj_uri_user(&ci.local_info,
                               local_user,
                               sizeof(local_user)) == PJ_SUCCESS)
    {
        snprintf(dialed_raw, sizeof(dialed_raw), "%s", local_user);
        snprintf(dialed_source, sizeof(dialed_source), "%s", "local_info");
        PJ_LOG(3, (THIS_FILE,
                   "[INCOMING] fallback=local_info user=%s",
                   local_user));
    }

    if (dialed_raw[0] == '\0') {
        PJ_LOG(2, (THIS_FILE,
                   "[DIALED] no dialed number found in Request-URI, To, or local_info; rejecting call %d",
                   call_id));
        pjsua_call_answer(call_id, PJSIP_SC_NOT_FOUND, NULL, NULL);
        return;
    }

    PJ_LOG(4, (THIS_FILE,
               "[DIALED] raw=%s source=%s",
               dialed_raw,
               dialed_source));

    /* Validate full dialed number against CC_B_NUMBER_PREFIXES.
     * Only applies when a prefix matches: strip it, remaining must be 10 digits.
     * If no prefix matches, fall through — cc_split_collect_number handles it. */
    {
        const char *pfxs = cc_cfg_b_number_prefixes();
        if (pfxs && pfxs[0] != '\0') {
            size_t dlen = strlen(dialed_raw);
            size_t best = 0;
            int    matched = 0;
            const char *p = pfxs;
            while (p && *p) {
                const char *comma = strchr(p, ',');
                size_t slen = comma ? (size_t)(comma - p) : strlen(p);
                if (slen > 0 && slen <= dlen &&
                    strncmp(dialed_raw, p, slen) == 0 && slen > best)
                {
                    best = slen;
                    matched = 1;
                }
                p += slen;
                if (*p == ',') p++;
            }
            if (matched && dlen - best != 10) {
                PJ_LOG(2, (THIS_FILE,
                           "[DIALED] prefix matched but remaining digits=%zu (need 10); "
                           "playing incomplete-number prompt to call %d",
                           dlen - best, call_id));
                session = cc_session_create();
                if (!session) {
                    pjsua_call_answer(call_id, PJSIP_SC_NOT_FOUND, NULL, NULL);
                    return;
                }
                session->call_a = call_id;
                session->acc_id = acc_id;
                session->call_start_ts = time(NULL);
                if (ci.call_id.slen > 0) {
                    pj_size_t clen = (pj_size_t)ci.call_id.slen;
                    if (clen >= sizeof(session->call_id))
                        clen = sizeof(session->call_id) - 1;
                    memcpy(session->call_id, ci.call_id.ptr, clen);
                    session->call_id[clen] = '\0';
                }
                pjsua_call_set_user_data(call_id, session);
                CC_SESSION_LOCK(session);
                session->torn_down = 1;
                CC_SESSION_UNLOCK(session);
                pjsua_call_setting_default(&cs);
                cs.aud_cnt = 1; cs.vid_cnt = 0; cs.txt_cnt = 0;
                if (cc_call_answer2_serialized(call_id, &cs, PJSIP_SC_OK,
                                               NULL, NULL, NULL)
                        != PJ_SUCCESS) {
                    pjsua_call_answer(call_id, PJSIP_SC_NOT_FOUND, NULL, NULL);
                    cc_session_invalidate_a(session, call_id);
                    cc_session_maybe_finalize(session);
                    return;
                }
                leg_a_play_prompt_then_hangup(session,
                                              CC_PROMPT_INCOMPLETE_NUMBER,
                                              PJSIP_SC_NOT_FOUND);
                cc_session_maybe_finalize(session);
                return;
            }
        }
    }

    status = cc_split_collect_number(dialed_raw, &collect_number);
    if (status != PJ_SUCCESS) {
        /*
         * Dialed number may be the bare short code (e.g. "612") from a
         * call-forward scenario. Check Diversion/History-Info for the
         * original called party to use as sponsor MSISDN.
         */
        char diversion_user[128] = {0};

        if (rdata && rdata->msg_info.msg &&
            cc_extract_diversion_user(rdata->msg_info.msg,
                                      diversion_user,
                                      sizeof(diversion_user)) == PJ_SUCCESS &&
            diversion_user[0] != '\0')
        {
            PJ_LOG(3, (THIS_FILE,
                       "[DIVERSION] bare prefix=%s; using Diversion user=%s as sponsor",
                       dialed_raw, diversion_user));
            memset(&collect_number, 0, sizeof(collect_number));
            snprintf(collect_number.sponsor_raw,
                     sizeof(collect_number.sponsor_raw),
                     "%s", diversion_user);
            snprintf(collect_number.dialed_digits,
                     sizeof(collect_number.dialed_digits),
                     "%s", dialed_raw);
            snprintf(collect_number.matched_prefix,
                     sizeof(collect_number.matched_prefix),
                     "%s", dialed_raw);
            collect_number.already_stripped = 1;
            status = PJ_SUCCESS;
        } else {
            PJ_LOG(2, (THIS_FILE,
                       "[PREFIX] mode=%s prefixes=%s matched=<none>; rejecting call %d",
                       cc_cfg_prefix_mode_name(),
                       cc_cfg_collect_prefixes(),
                       call_id));
            pjsua_call_answer(call_id, PJSIP_SC_NOT_FOUND, NULL, NULL);
            return;
        }
    }

    PJ_LOG(4, (THIS_FILE,
               "[PREFIX] mode=%s prefixes=%s matched=%s already_stripped=%s",
               cc_cfg_prefix_mode_name(),
               cc_cfg_collect_prefixes(),
               collect_number.prefix_matched ?
                   collect_number.matched_prefix : "<none>",
               collect_number.already_stripped ? "yes" : "no"));

    status = cc_normalize_msisdn(collect_number.sponsor_raw,
                                 sponsor_normalized,
                                 sizeof(sponsor_normalized));
    if (status != PJ_SUCCESS) {
        PJ_LOG(2, (THIS_FILE,
                   "[B-PARTY] raw=%s normalization failed; rejecting call %d",
                   collect_number.sponsor_raw,
                   call_id));
        pjsua_call_answer(call_id, PJSIP_SC_NOT_FOUND, NULL, NULL);
        return;
    }

    PJ_LOG(4, (THIS_FILE,
               "[B-PARTY] raw=%s normalized=%s",
               collect_number.sponsor_raw,
               sponsor_normalized));

    /* Validate B-number length and prefix */
    if (cc_validate_b_number(sponsor_normalized) != PJ_SUCCESS) {
        PJ_LOG(2, (THIS_FILE,
                   "[B-PARTY] invalid B number raw=%s normalized=%s; "
                   "playing incomplete-number prompt to A",
                   collect_number.sponsor_raw, sponsor_normalized));

        /* Must create a minimal session so leg_a_play_prompt_then_hangup
         * can play the WAV and hang up A cleanly. */
        session = cc_session_create();
        if (!session) {
            pjsua_call_answer(call_id, PJSIP_SC_NOT_FOUND, NULL, NULL);
            return;
        }
        session->call_a = call_id;
        session->acc_id = acc_id;
        session->call_start_ts = time(NULL);
        if (ci.call_id.slen > 0) {
            pj_size_t clen = (pj_size_t)ci.call_id.slen;
            if (clen >= sizeof(session->call_id))
                clen = sizeof(session->call_id) - 1;
            memcpy(session->call_id, ci.call_id.ptr, clen);
            session->call_id[clen] = '\0';
        }
        pjsua_call_set_user_data(call_id, session);

        pjsua_call_setting_default(&cs);
        cs.aud_cnt = 1;
        cs.vid_cnt = 0;
        cs.txt_cnt = 0;
        if (cc_call_answer2_serialized(call_id, &cs, PJSIP_SC_OK,
                                       NULL, NULL, NULL)
                != PJ_SUCCESS) {
            pjsua_call_answer(call_id, PJSIP_SC_NOT_FOUND, NULL, NULL);
            cc_session_mark_end(session, "FAILED", "INVALID_B_NUMBER");
            CC_SESSION_LOCK(session);
            session->torn_down = 1;
            CC_SESSION_UNLOCK(session);
            cc_session_invalidate_a(session, call_id);
            cc_session_maybe_finalize(session);
            return;
        }

        CC_SESSION_LOCK(session);
        session->torn_down = 1;
        snprintf(session->final_status, sizeof(session->final_status),
                 "FAILED");
        snprintf(session->final_reason, sizeof(session->final_reason),
                 "INVALID_B_NUMBER");
        CC_SESSION_UNLOCK(session);
        leg_a_play_prompt_then_hangup(session,
                                      CC_PROMPT_INCOMPLETE_NUMBER,
                                      PJSIP_SC_NOT_FOUND);
        {
            int treatment_armed = 0;
            CC_SESSION_LOCK(session);
            treatment_armed = session->a_treatment_running;
            CC_SESSION_UNLOCK(session);
            if (!treatment_armed)
                cc_session_mark_end(session, "FAILED", "INVALID_B_NUMBER");
        }
        cc_session_maybe_finalize(session);
        return;
    }
    session = cc_session_create();
    if (!session) {
        PJ_LOG(1, (THIS_FILE, "session_create failed"));
        pjsua_call_answer(call_id, PJSIP_SC_SERVICE_UNAVAILABLE, NULL, NULL);
        return;
    }

    session->call_a = call_id;
    session->acc_id = acc_id;
    strncpy(session->b_number,
            sponsor_normalized,
            sizeof(session->b_number) - 1);
    session->b_number[sizeof(session->b_number) - 1] = '\0';
    snprintf(session->dialed_number_raw,
             sizeof(session->dialed_number_raw),
             "%s",
             dialed_raw);
    snprintf(session->dialed_number_digits,
             sizeof(session->dialed_number_digits),
             "%s",
             collect_number.dialed_digits);
    snprintf(session->dialed_number_source,
             sizeof(session->dialed_number_source),
             "%s",
             dialed_source);
    snprintf(session->matched_prefix,
             sizeof(session->matched_prefix),
             "%s",
             collect_number.matched_prefix);
    session->fundless = cc_cfg_is_fundless_prefix(session->matched_prefix);
    snprintf(session->sponsor_msisdn_raw,
             sizeof(session->sponsor_msisdn_raw),
             "%s",
             collect_number.sponsor_raw);
    snprintf(session->sponsor_msisdn_normalized,
             sizeof(session->sponsor_msisdn_normalized),
             "%s",
             sponsor_normalized);
    session->call_start_ts = time(NULL);
    session->a_invite_cb_ms = t_cb;
    session->a_100_sent_ms = t_100;
    if (ci.call_id.slen > 0) {
        pj_size_t len = (pj_size_t)ci.call_id.slen;
        if (len >= sizeof(session->call_id))
            len = sizeof(session->call_id) - 1;
        memcpy(session->call_id, ci.call_id.ptr, len);
        session->call_id[len] = '\0';
    } else {
        snprintf(session->call_id, sizeof(session->call_id),
                 "CALL-%ld", (long)session->call_start_ts);
    }

    PJ_LOG(3, (THIS_FILE, "[CALL-START] callId=%s", session->call_id));

    /* Capture operator headers and API identity fields from A's INVITE. */
    if (rdata && rdata->msg_info.msg)
        cc_capture_fwd_headers(rdata->msg_info.msg, session);

    /* Extract SSP from the LAST Record-Route of A-leg INVITE.
     * Record-Route order as seen by B2BUA (outermost = closest to B2BUA):
     *   RR[0]: Kamailio  <sip:10.185.49.39;lr>          (topmost)
     *   RR[1]: MTN SBC   <sip:102.89.52.113:5060;lr>    (last = phone-side SBC)
     * The last RR is the SBC that anchored the call from the subscriber side.
     * Use typed PJSIP_H_RECORD_ROUTE search — more reliable than name string
     * walk which can miss headers depending on parse order. */
    if (rdata && rdata->msg_info.msg) {
        pjsip_route_hdr *rr = (pjsip_route_hdr *)
            pjsip_msg_find_hdr(rdata->msg_info.msg,
                               PJSIP_H_RECORD_ROUTE, NULL);
        pjsip_route_hdr *rr_last = NULL;
        int rr_count = 0;
        while (rr) {
            rr_last = rr;
            rr_count++;
            rr = (pjsip_route_hdr *)
                pjsip_msg_find_hdr(rdata->msg_info.msg,
                                   PJSIP_H_RECORD_ROUTE, rr->next);
        }
        PJ_LOG(4, (THIS_FILE, "[SSP] Record-Route count=%d", rr_count));
        if (rr_last) {
            pjsip_sip_uri *rr_uri =
                (pjsip_sip_uri *)pjsip_uri_get_uri(rr_last->name_addr.uri);
            if (rr_uri && rr_uri->host.slen > 0) {
                pj_size_t hlen = (pj_size_t)rr_uri->host.slen;
                if (hlen >= sizeof(session->a_rr_host))
                    hlen = sizeof(session->a_rr_host) - 1;
                memcpy(session->a_rr_host, rr_uri->host.ptr, hlen);
                session->a_rr_host[hlen] = '\0';
                session->a_rr_port = rr_uri->port > 0 ? rr_uri->port : 5060;
                PJ_LOG(4, (THIS_FILE,
                           "[SSP] extracted from Record-Route[%d/%d]: %s:%d",
                           rr_count, rr_count,
                           session->a_rr_host, session->a_rr_port));
            }
        } else {
            PJ_LOG(4, (THIS_FILE, "[SSP] no Record-Route in A-leg INVITE"));
        }
    }

    {
        const char *pai_value;
        const char *caller_source = "UNKNOWN";
        char caller_raw[128] = "";
        char caller_normalized[64] = "";
        char from_identity[512];

        pai_value = cc_captured_header_value(session, "P-Asserted-Identity");
        if (pai_value &&
            cc_extract_uri_user(pai_value,
                                caller_raw,
                                sizeof(caller_raw)) == PJ_SUCCESS)
        {
            caller_source = "PAI";
        }

        if (caller_raw[0] == '\0') {
            pj_size_t from_len = (pj_size_t)ci.remote_info.slen;

            if (from_len >= sizeof(from_identity))
                from_len = sizeof(from_identity) - 1;
            memcpy(from_identity, ci.remote_info.ptr, from_len);
            from_identity[from_len] = '\0';

            if (cc_extract_uri_user(from_identity,
                                    caller_raw,
                                    sizeof(caller_raw)) == PJ_SUCCESS)
            {
                caller_source = "From";
            }
        }

        if (caller_raw[0] != '\0' &&
            cc_normalize_msisdn(caller_raw,
                                caller_normalized,
                                sizeof(caller_normalized)) == PJ_SUCCESS)
        {
            snprintf(session->caller_msisdn_raw,
                     sizeof(session->caller_msisdn_raw),
                     "%s",
                     caller_raw);
            snprintf(session->caller_msisdn_normalized,
                     sizeof(session->caller_msisdn_normalized),
                     "%s",
                     caller_normalized);
            snprintf(session->caller_msisdn,
                     sizeof(session->caller_msisdn),
                     "%s",
                     caller_normalized);
            snprintf(session->caller_msisdn_source,
                     sizeof(session->caller_msisdn_source),
                     "%s",
                     caller_source);
        }
    }

    if (session->caller_msisdn[0] == '\0') {
        char from_identity[512];
        pj_size_t from_len = (pj_size_t)ci.remote_info.slen;

        if (from_len >= sizeof(from_identity))
            from_len = sizeof(from_identity) - 1;
        memcpy(from_identity, ci.remote_info.ptr, from_len);
        from_identity[from_len] = '\0';

        (void)cc_extract_identity_user(from_identity,
                                       session->caller_msisdn,
                                       sizeof(session->caller_msisdn));
    }

    if (session->caller_msisdn[0] == '\0') {
        PJ_LOG(1, (THIS_FILE,
                   "[INITIATE-API] callerMsisdn could not be extracted from P-Asserted-Identity or From"));
    } else {
        PJ_LOG(4, (THIS_FILE,
                   "[CALLER] raw=%s normalized=%s source=%s",
                   session->caller_msisdn_raw,
                   session->caller_msisdn,
                   session->caller_msisdn_source[0] ?
                       session->caller_msisdn_source : "UNKNOWN"));
        PJ_LOG(4, (THIS_FILE,
                   "[INITIATE-API] callerMsisdn=%s",
                   session->caller_msisdn));
    }

    PJ_LOG(4, (THIS_FILE,
               "[END-API] ICID=%s",
               session->icid));

    /* Attach session as call user_data so callbacks can find it */
    pjsua_call_set_user_data(call_id, session);

    /*
     * RTPengine offer-then-answer: ng offer on general pool, patch A SDP to
     * RTPengine ports, then 200 on answer pool. Phone gets correct media in
     * the initial 200 — does not wait for late CONFIRMED/re-INVITE.
     */
    if (cc_rtpengine_enabled()) {
        char *sdp = (char *)malloc(CC_RTPENGINE_SDP_MAX);
        cc_event_t ev_off;

        if (!sdp ||
            cc_rtpengine_copy_rdata_sdp(rdata, sdp, CC_RTPENGINE_SDP_MAX) < 0)
        {
            PJ_LOG(1, (THIS_FILE,
                       "[RTPENGINE] A-leg SDP copy failed — reject call %d",
                       call_id));
            free(sdp);
            cc_session_mark_end(session, "FAILED", "SYSTEM_ERROR");
            CC_SESSION_LOCK(session);
            session->torn_down = 1;
            CC_SESSION_UNLOCK(session);
            pjsua_call_answer(call_id, PJSIP_SC_SERVICE_UNAVAILABLE, NULL, NULL);
            cc_session_invalidate_a(session, call_id);
            cc_session_maybe_finalize(session);
            return;
        }

        PJ_LOG(4, (THIS_FILE,
                   "Collect call (offer-then-answer rtpengine): A=%.*s B=%s",
                   (int)ci.remote_info.slen, ci.remote_info.ptr,
                   sponsor_normalized));

        memset(&ev_off, 0, sizeof(ev_off));
        ev_off.type = CC_EV_RTPENGINE_A_OFFER;
        ev_off.session = session;
        ev_off.session_serial = session->session_serial;
        ev_off.call_a = call_id;
        ev_off.data = sdp;
        snprintf(ev_off.reason, sizeof(ev_off.reason), "rtpengine-a-offer");

        CC_SESSION_LOCK(session);
        session->rtpengine_a_answer_pending = 0;
        session->rtpengine_a_offer_pending = 1;
        session->rtpengine_a_advertised = 0;
        session->rtpengine_a_need_advertise = 0;
        CC_SESSION_UNLOCK(session);

        if (cc_worker_post(&ev_off) != 0) {
            PJ_LOG(1, (THIS_FILE,
                       "[RTPENGINE] offer queue full — reject A call %d",
                       call_id));
            free(sdp);
            CC_SESSION_LOCK(session);
            session->rtpengine_a_offer_pending = 0;
            session->torn_down = 1;
            CC_SESSION_UNLOCK(session);
            cc_session_mark_end(session, "FAILED", "SYSTEM_ERROR");
            pjsua_call_answer(call_id, PJSIP_SC_SERVICE_UNAVAILABLE, NULL, NULL);
            cc_session_invalidate_a(session, call_id);
            cc_session_maybe_finalize(session);
            return;
        }

        t_queued = cc_monotonic_ms();
        PJ_LOG(3, (THIS_FILE,
                   "[A-TIMING] call_a=%d callId=%s phase=OFFER_QUEUED "
                   "since_cb_ms=%lld since_100_ms=%lld setup_ms=%lld "
                   "mode=rtpengine-offer-then-answer",
                   call_id, session->call_id,
                   t_queued - t_cb, t_queued - t_100, t_queued - t_100));
        return;
    }

    /*
     * UPDATE / re-INVITE / local_bridge: defer pjsua_call_answer2 (UDP bind +
     * media create) to the answer worker pool — same CPS pattern as rtpengine.
     */
    {
        cc_event_t ev;

        PJ_LOG(3, (THIS_FILE, "Collect call (async local answer): A=%.*s B=%s",
                   (int)ci.remote_info.slen, ci.remote_info.ptr,
                   sponsor_normalized));

        memset(&ev, 0, sizeof(ev));
        ev.type = CC_EV_RTPENGINE_A_ANSWER; /* data=NULL → local answer path */
        ev.session = session;
        ev.session_serial = session->session_serial;
        ev.call_a = call_id;
        snprintf(ev.reason, sizeof(ev.reason), "a-answer-local");

        CC_SESSION_LOCK(session);
        session->rtpengine_a_answer_pending = 1;
        CC_SESSION_UNLOCK(session);

        if (cc_worker_post(&ev) != 0) {
            PJ_LOG(1, (THIS_FILE,
                       "[A] answer worker queue full — reject call %d", call_id));
            CC_SESSION_LOCK(session);
            session->rtpengine_a_answer_pending = 0;
            session->torn_down = 1;
            CC_SESSION_UNLOCK(session);
            cc_session_mark_end(session, "FAILED", "SYSTEM_ERROR");
            pjsua_call_answer(call_id, PJSIP_SC_SERVICE_UNAVAILABLE, NULL, NULL);
            cc_session_invalidate_a(session, call_id);
            cc_session_maybe_finalize(session);
            return;
        }

        t_queued = cc_monotonic_ms();
        CC_SESSION_LOCK(session);
        session->a_answer_queued_ms = t_queued;
        CC_SESSION_UNLOCK(session);
        PJ_LOG(3, (THIS_FILE,
                   "[A-TIMING] call_a=%d callId=%s phase=ANSWER_QUEUED "
                   "since_cb_ms=%lld since_100_ms=%lld mode=local",
                   call_id, session->call_id,
                   t_queued - t_cb, t_queued - t_100));
        return;
    }
}

/* ── Async validation callback context ──────────────────────────────────── */

typedef struct {
    cc_session_t       *session;
    cc_originate_arg_t *arg;
    char                original_b[64];
} vasync_ctx_t;

/* ── Post-validation logic (shared by sync fallback and async callback) ─── */

static void cc_on_validation_result(cc_session_t *session,
                                     cc_originate_arg_t *arg,
                                     const char *original_b,
                                     cc_validation_result_t *result)
{
    int vstatus = result->status;

    PJ_LOG(3, (THIS_FILE, "UDP validation result: status=%d reason=%s",
               vstatus, result->reason));

    if (vstatus != 0) {
        const char *end_status;
        const char *end_reason;

        PJ_LOG(2, (THIS_FILE, "UDP validation rejected call: status=%d reason=%s",
                   vstatus, result->reason));

        CC_SESSION_LOCK(session);
        if (session->torn_down || session->call_a == PJSUA_INVALID_ID) {
            CC_SESSION_UNLOCK(session);
            PJ_LOG(3, (THIS_FILE,
                       "[VALIDATION] call already torn down after validation, skip B-leg"));
            free(arg);
            cc_session_acquire_reason(session, "finalize-guard");
            cc_session_maybe_finalize(session);
            cc_session_release_reason(session, "b-origination-ineligible");
            cc_session_release_reason(session, "finalize-guard");
            return;
        }
        session->torn_down = 1;
        CC_SESSION_UNLOCK(session);

        end_status = (vstatus < 0 || vstatus == CC_VALIDATION_API_FAILURE)
                     ? "FAILED" : "CANCELLED";
        end_reason = result->reason[0] ? result->reason
                                       : "ELIGIBILITY_TIMEOUT";
        /*
         * Do NOT mark_end / rtpengine delete yet — that would tear down media
         * before the rejection prompt can play to A. Store CDR fields, play
         * treatment, then mark_end from hangup-after-WAV (torn_down already
         * set so DISCONNECTED will not overwrite with USER_ABANDONED).
         */
        CC_SESSION_LOCK(session);
        snprintf(session->final_status, sizeof(session->final_status),
                 "%s", end_status);
        snprintf(session->final_reason, sizeof(session->final_reason),
                 "%s", end_reason);
        CC_SESSION_UNLOCK(session);
        free(arg);

        {
            cc_prompt_tag_t prompt_tag;
            pjsip_status_code sip_code;
            int treatment_armed = 0;

            switch (vstatus) {
            case CC_VALIDATION_CALLER_BLACKLISTED:
                prompt_tag = CC_PROMPT_NOT_AVAILABLE_TO_PAY;
                sip_code   = PJSIP_SC_FORBIDDEN;
                break;
            case CC_VALIDATION_SPONSOR_BALANCE_FAIL:
                prompt_tag = CC_PROMPT_LOW_BALANCE;
                sip_code   = PJSIP_SC_FORBIDDEN;
                break;
            case CC_VALIDATION_CALLER_ROAMING:
            case CC_VALIDATION_SPONSOR_DND_ACTIVE:
            case CC_VALIDATION_SPONSOR_ROAMING:
                prompt_tag = CC_PROMPT_NOT_AVAILABLE_TO_PAY;
                sip_code   = PJSIP_SC_FORBIDDEN;
                break;
            default:
                prompt_tag = CC_PROMPT_NOT_AVAILABLE_TO_PAY;
                sip_code   = PJSIP_SC_SERVICE_UNAVAILABLE;
                break;
            }
            leg_a_play_prompt_then_hangup(session, prompt_tag, sip_code);

            CC_SESSION_LOCK(session);
            treatment_armed = session->a_treatment_running;
            CC_SESSION_UNLOCK(session);
            if (!treatment_armed) {
                /* Prompt/worker post failed — emit CDR and tear down media now. */
                cc_session_mark_end(session, end_status, end_reason);
            }
        }

        cc_session_acquire_reason(session, "finalize-guard");
        cc_session_maybe_finalize(session);
        cc_session_release_reason(session, "b-origination-ineligible");
        cc_session_release_reason(session, "finalize-guard");
        return;
    }

    /* Whitelisted check */
    PJ_LOG(3, (THIS_FILE, "[WHITELIST-CHECK] details='%s' len=%d",
               result->details, (int)strlen(result->details)));
    if (result->details[0] != '\0' &&
        strcasestr(result->details, "IS_WHITELISTED") != NULL)
    {
        CC_SESSION_LOCK(session);
        session->whitelisted = 1;
        CC_SESSION_UNLOCK(session);
        PJ_LOG(3, (THIS_FILE, "[WHITELIST] caller whitelisted; B-leg will bridge directly"));
    }

    {
        cc_service_key_mode_t sk_mode      = cc_cfg_service_key_mode();
        const char           *sk_mode_name = cc_cfg_service_key_mode_name();
        const char           *api_sk       = result->service_key;
        const char           *placeholder  = cc_cfg_service_key_placeholder();
        char  service_key[64];
        int   have_sk = api_sk[0] != '\0';
        char  keyed_user[128];
        int   keyed_len;

        if (have_sk) {
            snprintf(service_key, sizeof(service_key), "%s", api_sk);
            PJ_LOG(3, (THIS_FILE, "[SERVICEKEY] source=api service_key=%s", service_key));
        } else {
            snprintf(service_key, sizeof(service_key), "%s",
                     placeholder ? placeholder : "");
            have_sk = service_key[0] != '\0';
            if (have_sk)
                PJ_LOG(3, (THIS_FILE,
                           "[SERVICEKEY] source=placeholder service_key=%s "
                           "reason=missing_in_eligible_response", service_key));
        }

        snprintf(arg->service_key,      sizeof(arg->service_key),      "%s", service_key);
        snprintf(arg->service_key_mode, sizeof(arg->service_key_mode), "%s", sk_mode_name);
        snprintf(arg->b_dial_number,    sizeof(arg->b_dial_number),    "%s", original_b);
        snprintf(arg->b_from_user,      sizeof(arg->b_from_user),      "%s",
                 session->caller_msisdn);

        if (sk_mode != CC_SERVICE_KEY_MODE_DISABLED && have_sk) {
            keyed_len = snprintf(keyed_user, sizeof(keyed_user), "%s%s",
                                 service_key, original_b);
            if (keyed_len < 0 || (size_t)keyed_len >= sizeof(keyed_user)) {
                keyed_user[0] = '\0';
                PJ_LOG(1, (THIS_FILE,
                           "[ERROR] serviceKey+B number too long; using sponsorMsisdn"));
            }
            if (keyed_user[0] != '\0') {
                if (sk_mode == CC_SERVICE_KEY_MODE_FROM_ONLY ||
                    sk_mode == CC_SERVICE_KEY_MODE_REQUEST_URI_AND_FROM)
                    snprintf(arg->b_from_user, sizeof(arg->b_from_user),
                             "%s", keyed_user);
                if (sk_mode == CC_SERVICE_KEY_MODE_REQUEST_URI ||
                    sk_mode == CC_SERVICE_KEY_MODE_REQUEST_URI_AND_FROM)
                    snprintf(arg->b_dial_number, sizeof(arg->b_dial_number),
                             "%s", keyed_user);
            }
        } else if (sk_mode != CC_SERVICE_KEY_MODE_DISABLED && !have_sk) {
            PJ_LOG(2, (THIS_FILE,
                       "[B-LEG] service_key_mode=%s requested but service_key empty; "
                       "using sponsorMsisdn", sk_mode_name));
        }

        CC_SESSION_LOCK(session);
        snprintf(session->service_key,    sizeof(session->service_key),
                 "%s", service_key);
        snprintf(session->b_dial_number,  sizeof(session->b_dial_number),
                 "%s", arg->b_dial_number);
        CC_SESSION_UNLOCK(session);

        PJ_LOG(3, (THIS_FILE,
                   "[B-LEG] service_key_mode=%s service_key=%s "
                   "request_user=%s from_user=%s",
                   sk_mode_name,
                   have_sk ? service_key : "<none>",
                   arg->b_dial_number, arg->b_from_user));
        PJ_LOG(3, (THIS_FILE,
                   "[B-LEG] original_b_user=%s service_key=%s final_request_user=%s",
                   original_b, have_sk ? service_key : "<none>",
                   arg->b_dial_number));
    }

    PJ_LOG(3, (THIS_FILE, "A-leg confirmed; starting B-leg to %s", arg->b_dial_number));

    CC_SESSION_LOCK(session);
    if (session->torn_down ||
        session->call_a == PJSUA_INVALID_ID ||
        session->final_cleanup_started)
    {
        CC_SESSION_UNLOCK(session);
        PJ_LOG(3, (THIS_FILE,
                   "[VALIDATION] call already torn down before B worker, skip B-leg"));
        free(arg);
        cc_session_acquire_reason(session, "finalize-guard");
        cc_session_maybe_finalize(session);
        cc_session_release_reason(session, "b-origination-ineligible");
        cc_session_release_reason(session, "finalize-guard");
        return;
    }
    session->b_origination_pending = 1;
    session->b_leg_started = 1;
    CC_SESSION_UNLOCK(session);

    if (!cc_session_acquire_reason(session, "b-origination-worker")) {
        PJ_LOG(1, (THIS_FILE, "[ERROR] B-leg worker could not retain session"));
        CC_SESSION_LOCK(session);
        session->b_origination_pending = 0;
        CC_SESSION_UNLOCK(session);
        free(arg);
        cc_session_acquire_reason(session, "finalize-guard");
        cc_session_maybe_finalize(session);
        cc_session_release_reason(session, "b-origination-ineligible");
        cc_session_release_reason(session, "finalize-guard");
        return;
    }

    CC_SESSION_LOCK(session);
    session->originate_arg = arg;
    CC_SESSION_UNLOCK(session);

    {
        cc_event_t ev;
        int wait_ms = 0;
        memset(&ev, 0, sizeof(ev));
        ev.type    = CC_EV_ORIGINATE_B;
        ev.session = session;
        snprintf(ev.reason, sizeof(ev.reason), "b-origination-worker");
        /* Compute remaining prompt time so B-leg fires exactly when 1.1.wav ends,
         * regardless of how long validation took. */
        CC_SESSION_LOCK(session);
        if (session->a_prompt_duration_ms > 0 && session->a_confirmed_ms > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            long long now_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
            long long elapsed = now_ms - session->a_confirmed_ms;
            long long remaining = (long long)session->a_prompt_duration_ms - elapsed;
            if (remaining > 0)
                wait_ms = (int)remaining;
        }
        CC_SESSION_UNLOCK(session);
        cc_session_release_reason(session, "b-origination-worker");
        if (cc_worker_post_delayed(&ev, wait_ms) != 0) {
            PJ_LOG(1, (THIS_FILE, "[ERROR] B-leg worker post failed"));
            CC_SESSION_LOCK(session);
            session->b_origination_pending = 0;
            session->originate_arg = NULL;
            session->torn_down = 1;
            pjsua_call_id call_a = session->call_a;
            CC_SESSION_UNLOCK(session);
            free(arg);
            cc_session_mark_end(session, "FAILED", "SYSTEM_ERROR");
            if (call_a != PJSUA_INVALID_ID)
                cc_safe_hangup(call_a, PJSIP_SC_SERVICE_UNAVAILABLE);
            cc_session_acquire_reason(session, "finalize-guard");
            cc_session_maybe_finalize(session);
            cc_session_release_reason(session, "b-origination-ineligible");
            cc_session_release_reason(session, "finalize-guard");
        }
    }
}

static void vasync_validation_cb(void *cb_arg, cc_validation_result_t *result)
{
    vasync_ctx_t *ctx = (vasync_ctx_t *)cb_arg;
    cc_on_validation_result(ctx->session, ctx->arg, ctx->original_b, result);
    /* release the ref acquired before cc_udp_validate_async */
    cc_session_release_reason(ctx->session, "b-origination-validation");
    free(ctx);
}

/* ── Start B leg after A-leg ACK/CONFIRMED ───────────────────────────────── */

pjsua_call_id cc_start_b_leg_after_a_confirmed(cc_session_t *session)
{
    cc_originate_arg_t *arg;
    char original_b[64];

    if (!session)
        return PJSUA_INVALID_ID;

    CC_SESSION_LOCK(session);

    if (session->torn_down || session->b_validation_started) {
        CC_SESSION_UNLOCK(session);
        return PJSUA_INVALID_ID;
    }

    /* Mark validation flow started — do NOT set b_leg_started yet.
     * b_leg_started means B originate was armed after ELIGIBLE; setting it
     * here made ineligible take the "stop MOH" path and cut off 1.1.wav. */
    session->b_validation_started = 1;

    arg = malloc(sizeof(*arg));
    if (!arg) {
        session->torn_down = 1;
        pjsua_call_id call_a = session->call_a;
        CC_SESSION_UNLOCK(session);

        cc_session_mark_end(session, "FAILED", "SYSTEM_ERROR");

        if (call_a != PJSUA_INVALID_ID)
            cc_safe_hangup(call_a, PJSIP_SC_SERVICE_UNAVAILABLE);
        return PJSUA_INVALID_ID;
    }

    memset(arg, 0, sizeof(*arg));
    arg->session = session;
    arg->acc_id  = session->acc_id;
    snprintf(original_b, sizeof(original_b), "%s", session->b_number);
    snprintf(arg->b_dial_number, sizeof(arg->b_dial_number),
             "%s", session->b_number);
    if (session->caller_msisdn[0] != '\0') {
        snprintf(arg->b_from_user, sizeof(arg->b_from_user),
                 "%s", session->caller_msisdn);
    } else {
        snprintf(arg->b_from_user, sizeof(arg->b_from_user),
                 "%s", session->b_number);
        PJ_LOG(2, (THIS_FILE,
                   "[B-LEG] caller_msisdn empty; falling back to b_number for From"));
    }

    CC_SESSION_UNLOCK(session);

    /* Async validation — returns immediately; callback fires on worker thread */
    {
        char call_id[128];
        char caller_msisdn[64];
        char time_str[64];
        const char *initiate_source;
        vasync_ctx_t *ctx;
        time_t now = time(NULL);

        if (cc_format_nigeria_time(now, time_str, sizeof(time_str)) != PJ_SUCCESS)
            time_str[0] = '\0';

        CC_SESSION_LOCK(session);
        snprintf(call_id,       sizeof(call_id),       "%s", session->call_id);
        snprintf(caller_msisdn, sizeof(caller_msisdn), "%s", session->caller_msisdn);
        initiate_source = session->fundless ? CC_INITIATE_SOURCE_FUNDLESS
                                            : CC_INITIATE_SOURCE_NORMAL;
        CC_SESSION_UNLOCK(session);

        PJ_LOG(3, (THIS_FILE, "[API-TIME] timestamp=%s", time_str));
        PJ_LOG(3, (THIS_FILE,
                   "[INITIATE-API] callerMsisdn=%s sponsorMsisdn=%s callId=%s "
                   "source=%s timestamp=%s",
                   caller_msisdn, original_b, call_id, initiate_source, time_str));
        PJ_LOG(3, (THIS_FILE, "[API] initiate caller=%s sponsor=%s",
                   caller_msisdn, original_b));

        ctx = malloc(sizeof(*ctx));
        if (!ctx) {
            PJ_LOG(1, (THIS_FILE, "[ERROR] vasync_ctx malloc failed"));
            CC_SESSION_LOCK(session);
            session->torn_down = 1;
            pjsua_call_id call_a = session->call_a;
            CC_SESSION_UNLOCK(session);
            free(arg);
            cc_session_mark_end(session, "FAILED", "SYSTEM_ERROR");
            if (call_a != PJSUA_INVALID_ID)
                cc_safe_hangup(call_a, PJSIP_SC_SERVICE_UNAVAILABLE);
            cc_session_acquire_reason(session, "finalize-guard");
            cc_session_maybe_finalize(session);
            cc_session_release_reason(session, "b-origination-ineligible");
            cc_session_release_reason(session, "finalize-guard");
            return PJSUA_INVALID_ID;
        }
        ctx->session = session;
        ctx->arg     = arg;
        snprintf(ctx->original_b, sizeof(ctx->original_b), "%s", original_b);

        /* Hold a ref across the async gap; released in vasync_validation_cb */
        if (!cc_session_acquire_reason(session, "b-origination-validation")) {
            free(ctx);
            free(arg);
            cc_session_acquire_reason(session, "finalize-guard");
            cc_session_maybe_finalize(session);
            cc_session_release_reason(session, "b-origination-ineligible");
            cc_session_release_reason(session, "finalize-guard");
            return PJSUA_INVALID_ID;
        }

        if (cc_udp_validate_async(caller_msisdn, original_b, call_id,
                                   initiate_source, time_str,
                                   vasync_validation_cb, ctx) != 0)
        {
            PJ_LOG(1, (THIS_FILE, "[ERROR] cc_udp_validate_async failed"));
            cc_session_release_reason(session, "b-origination-validation");
            free(ctx);
            CC_SESSION_LOCK(session);
            session->torn_down = 1;
            pjsua_call_id call_a = session->call_a;
            CC_SESSION_UNLOCK(session);
            free(arg);
            cc_session_mark_end(session, "FAILED", "SYSTEM_ERROR");
            if (call_a != PJSUA_INVALID_ID)
                cc_safe_hangup(call_a, PJSIP_SC_SERVICE_UNAVAILABLE);
            cc_session_acquire_reason(session, "finalize-guard");
            cc_session_maybe_finalize(session);
            cc_session_release_reason(session, "b-origination-ineligible");
            cc_session_release_reason(session, "finalize-guard");
            return PJSUA_INVALID_ID;
        }
    }
    return PJSUA_INVALID_ID; /* result handled in vasync_validation_cb */
}

/* ── Originate B leg ─────────────────────────────────────────────────────────────── */

void *cc_originate_b_thread(void *arg_ptr)
{
    cc_originate_arg_t *arg = (cc_originate_arg_t *)arg_ptr;
    cc_session_t       *session = arg->session;
    pj_thread_desc desc;
    pj_thread_t *this_thread = NULL;
    pj_status_t thread_status;

    pj_bzero(desc, sizeof(desc));
    thread_status = pj_thread_register("cc_orig_b", desc, &this_thread);
    if (thread_status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] B-leg PJ thread registration failed: %d",
                   thread_status));
        CC_SESSION_LOCK(session);
        session->b_origination_pending = 0;
        CC_SESSION_UNLOCK(session);
        free(arg);
        return NULL;
    }

    pjsua_acc_id	acc_id = arg->acc_id;
    char                b_uri[256];
    char                b_from_uri[256];
    char                request_user[128];
    char                from_user[128];
    char                service_key[64];
    char                service_key_mode[32];
    pjsua_call_id       call_b = PJSUA_INVALID_ID;
    pjsua_call_setting  cs;
    pjsua_msg_data      msg_data;
    pj_str_t            target;
    pj_status_t         status;

    snprintf(request_user, sizeof(request_user), "%s", arg->b_dial_number);
    snprintf(from_user, sizeof(from_user), "%s", arg->b_from_user);
    snprintf(service_key, sizeof(service_key), "%s", arg->service_key);
    snprintf(service_key_mode,
             sizeof(service_key_mode),
             "%s",
             arg->service_key_mode);

    PJ_LOG(3, (THIS_FILE,
               "[B-LEG] using final dial number=%s",
               request_user));
    PJ_LOG(3, (THIS_FILE,
               "[B-LEG] next_hop=%s",
               cc_cfg_sbc_next_hop()));
    PJ_LOG(3, (THIS_FILE,
               "[B-LEG] service_key_mode=%s service_key=%s",
               service_key_mode[0] ? service_key_mode :
                   cc_cfg_service_key_mode_name(),
               service_key[0] ? service_key : "<none>"));

    status = cc_build_b_uri(request_user, b_uri, sizeof(b_uri));
    if (status == PJ_SUCCESS)
        status = cc_build_b_from_uri(from_user, b_from_uri, sizeof(b_from_uri));
    free(arg);

    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] B-leg URI exceeds configured buffer"));
        CC_SESSION_LOCK(session);
        session->torn_down = 1;
        session->b_origination_pending = 0;
        snprintf(session->final_status, sizeof(session->final_status),
                 "FAILED");
        snprintf(session->final_reason, sizeof(session->final_reason),
                 "SYSTEM_ERROR");
        CC_SESSION_UNLOCK(session);
        leg_a_play_unavailable_then_hangup(session);
        {
            int treatment_armed = 0;
            CC_SESSION_LOCK(session);
            treatment_armed = session->a_treatment_running;
            CC_SESSION_UNLOCK(session);
            if (!treatment_armed)
                cc_session_mark_end(session, "FAILED", "SYSTEM_ERROR");
        }
        return NULL;
    }

    CC_SESSION_LOCK(session);
    if (session->torn_down ||
        session->call_a == PJSUA_INVALID_ID ||
        session->final_cleanup_started)
    {
        session->b_origination_pending = 0;
        CC_SESSION_UNLOCK(session);
        PJ_LOG(3, (THIS_FILE,
                   "[TIMER] skipped stale action: B-leg origination"));
        return NULL;
    }
    CC_SESSION_UNLOCK(session);

    /* A-prompt wait is now handled by cc_worker_post_delayed at the call site.
     * Worker arrives here only after the prompt duration has elapsed. */

    /* Start MOH on A-leg while B is ringing — all callers.
     * Clear player_a first: 1.1.wav finished naturally (one-shot) so
     * session->player_a still holds its id but the player is done. */
    {
        pjsua_call_id call_a;
        pjsua_player_id old_player;
        CC_SESSION_LOCK(session);
        call_a = session->call_a;
        old_player = session->player_a;
        session->player_a = PJSUA_INVALID_ID;
        CC_SESSION_UNLOCK(session);

        if (old_player != PJSUA_INVALID_ID) {
            PJ_LOG(3, (THIS_FILE,
                       "[VOICE] Clearing finished 1.1.wav player=%d before MOH",
                       old_player));
            cc_stop_wav(old_player, PJSUA_INVALID_ID);
        }

        if (call_a != PJSUA_INVALID_ID) {
            const char *moh_path = cc_prompt_get_path(CC_PROMPT_MOH);
            pjsua_player_id moh_pid = cc_start_wav(call_a, moh_path, PJ_TRUE);
            if (moh_pid != PJSUA_INVALID_ID) {
                CC_SESSION_LOCK(session);
                if (session->call_a == call_a &&
                    !session->accepted &&
                    !session->torn_down &&
                    session->player_a == PJSUA_INVALID_ID)
                {
                    session->player_a = moh_pid;
                    session->a_prompt_duration_ms = 0; /* looping — no deferral needed */
                    PJ_LOG(3, (THIS_FILE,
                               "[VOICE] MOH started on A-leg player=%d path=%s",
                               moh_pid, moh_path));
                } else {
                    CC_SESSION_UNLOCK(session);
                    cc_stop_wav(moh_pid, PJSUA_INVALID_ID);
                    PJ_LOG(3, (THIS_FILE, "[VOICE] MOH discarded (stale)"));
                    goto skip_moh_store;
                }
                CC_SESSION_UNLOCK(session);
            }
        }
        skip_moh_store:;
    }

    PJ_LOG(3, (THIS_FILE, "Originating B leg to %s", b_uri));
    PJ_LOG(3, (THIS_FILE,
               "[CALL-SLOTS] before B-leg originate active_calls=%u pjsua_max_calls=%d",
               pjsua_call_get_count(), PJSUA_MAX_CALLS));

    pjsua_call_setting_default(&cs);

    /* B-leg is audio only; disable video/text m= sections. */
    cs.aud_cnt = 1;
    cs.vid_cnt = 0;
    cs.txt_cnt = 0;
    /* Disable session timer on B-leg: prevents reinv_timer_cb from firing
     * during EARLY state (while PRACK transaction holds the dialog lock),
     * which causes the "Timed-out trying to acquire dialog mutex" warning. */




    pjsua_msg_data_init(&msg_data);

    /*
     * local_uri is PJSUA's supported per-call From override for the initial
     * INVITE. Do not add From as a generic header, which would duplicate it.
     */
#if CC_BLEG_FROM_USE_FINAL_DIAL_NUMBER
    msg_data.local_uri = pj_str(b_from_uri);
#endif

    PJ_LOG(3, (THIS_FILE, "[B-LEG] request_uri=%s", b_uri));
    PJ_LOG(3, (THIS_FILE, "[B-LEG-HDR] request_uri=%s", b_uri));
#if CC_BLEG_FROM_USE_FINAL_DIAL_NUMBER
    PJ_LOG(3, (THIS_FILE, "[B-LEG-HDR] from_uri=%s", b_from_uri));
#else
    PJ_LOG(3, (THIS_FILE, "[B-LEG-HDR] from_uri=account-default"));
#endif

    /* Build forwarded operator headers for the initial outbound INVITE. */
    pj_pool_t *hdr_pool = pjsua_pool_create("fwd_hdrs", 2048, 1024);
    int pcv_copied = 0;
    int pani_copied = 0;
    int pani_static = 0;
    int pai_copied = 0;

    if (hdr_pool) {
        pj_list_init(&msg_data.hdr_list);
        int i;

        for (i = 0; i < session->fwd_hdr_count; i++) {
            const char *name = session->fwd_hdrs[i].name;
            const char *value = session->fwd_hdrs[i].value;
            int is_pani = cc_header_name_is(name,
                                             "P-Access-Network-Info");
            int is_pai = cc_header_name_is(name,
                                            "P-Asserted-Identity");

#if CC_BLEG_STATIC_PANI_ENABLE && CC_BLEG_REPLACE_COPIED_PANI
            if (is_pani)
                continue;
#endif

            /* PAI is rebuilt from caller MSISDN + local host below */
            if (is_pai)
                continue;

            if (!cc_add_msg_header(hdr_pool, &msg_data, name, value)) {
                PJ_LOG(1, (THIS_FILE,
                           "[ERROR] B-leg header add failed: %s",
                           name));
                continue;
            }

            if (cc_header_name_is(name, "P-Charging-Vector"))
                pcv_copied = 1;
            else if (is_pani)
                pani_copied = 1;
        }

#if CC_BLEG_STATIC_PANI_ENABLE
        {
            const char *pani_value = cc_cfg_pani_value();

            if (pani_value && pani_value[0] != '\0') {
                pani_static = cc_add_msg_header(hdr_pool,
                                                &msg_data,
                                                "P-Access-Network-Info",
                                                pani_value);
            }
        }
        PJ_LOG(3, (THIS_FILE,
                   "[B-LEG-HDR] static PANI added=%s",
                   pani_static ? "yes" : "no"));
        if (!pani_static) {
            PJ_LOG(1, (THIS_FILE,
                       "[ERROR] B-leg static PANI add failed"));
        }
#endif

        /* P-Early-Media: Supported — required by IMS/SBC for early media */
        cc_add_msg_header(hdr_pool, &msg_data, "P-Early-Media", "Supported");

        /* Build PAI from caller MSISDN + local host (no '+' prefix per network spec) */
        {
            char pai_buf[256];
            const char *caller = session->caller_msisdn;
            int pai_len;

            /* Strip leading '+' if present — PAI uses bare digits */
            if (caller[0] == '+')
                caller++;

            pai_len = snprintf(pai_buf, sizeof(pai_buf),
                               "<sip:%s@%s;user=phone>",
                               caller, cc_cfg_local_host());

            if (pai_len > 0 && (size_t)pai_len < sizeof(pai_buf)) {
                pai_copied = cc_add_msg_header(hdr_pool,
                                               &msg_data,
                                               "P-Asserted-Identity",
                                               pai_buf);
            }
            PJ_LOG(3, (THIS_FILE,
                       "[B-LEG-HDR] PAI built=%s added=%s",
                       pai_buf, pai_copied ? "yes" : "no"));
        }
    } else {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] B-leg header pool creation failed"));
    }

    PJ_LOG(3, (THIS_FILE,
               "[B-LEG-HDR] P-Charging-Vector copied=%s",
               pcv_copied ? "yes" : "no"));
    PJ_LOG(3, (THIS_FILE,
               "[B-LEG-HDR] P-Asserted-Identity copied=%s",
               pai_copied ? "yes" : "no"));
#if CC_BLEG_STATIC_PANI_ENABLE
#if CC_BLEG_REPLACE_COPIED_PANI
    PJ_LOG(3, (THIS_FILE,
               "[B-LEG-HDR] PANI mode=%s value=%s",
               pani_static ? "static" : "none",
               pani_static ? cc_cfg_pani_value() : ""));
#else
    PJ_LOG(3, (THIS_FILE,
               "[B-LEG-HDR] PANI mode=%s value=%s",
               (pani_static && pani_copied) ? "static+copy" :
               (pani_static ? "static" : (pani_copied ? "copy" : "none")),
               pani_static ? cc_cfg_pani_value() : ""));
#endif
#else
    PJ_LOG(3, (THIS_FILE,
               "[B-LEG-HDR] PANI mode=%s",
               pani_copied ? "copy" : "none"));
#endif
    PJ_LOG(3, (THIS_FILE,
               "[HEADERS] pcv copied=%s pai copied=%s pani mode=%s value=%s",
               pcv_copied ? "yes" : "no",
               pai_copied ? "yes" : "no",
               pani_static ? "static" : (pani_copied ? "copy" : "none"),
               pani_static ? cc_cfg_pani_value() : ""));

    /* Supported: 100rel — advertise support without requiring it */
    if (hdr_pool) {
        cc_add_msg_header(hdr_pool, &msg_data, "Supported", "100rel");
    }

    /* Route2: SSP from A-leg last Record-Route.
     * Route1 (Kamailio) is added automatically by PJSUA via acc_cfg.proxy_uri.
     * Only add Route2 when the MTN SBC is distinct from Kamailio. */
    {
        char ssp_host[128];
        int  ssp_port;
        const char *kam_host = cc_cfg_sbc_host();
        int         kam_port = cc_cfg_sbc_port();

        CC_SESSION_LOCK(session);
        snprintf(ssp_host, sizeof(ssp_host), "%s", session->a_rr_host);
        ssp_port = session->a_rr_port;
        CC_SESSION_UNLOCK(session);

        if (ssp_host[0] != '\0' && hdr_pool &&
            (strcmp(ssp_host, kam_host) != 0 || ssp_port != kam_port))
        {
            char route_buf[256];
            int rlen = snprintf(route_buf, sizeof(route_buf),
                                "<sip:%s:%d;transport=udp;lr>",
                                ssp_host, ssp_port);
            if (rlen > 0 && (size_t)rlen < sizeof(route_buf)) {
                cc_add_msg_header(hdr_pool, &msg_data, "Route", route_buf);
                PJ_LOG(3, (THIS_FILE, "[B-LEG-HDR] Route2(SSP)=%s", route_buf));
            }
        } else if (ssp_host[0] == '\0') {
            PJ_LOG(3, (THIS_FILE, "[B-LEG-HDR] Route2(SSP) omitted — no Record-Route in A-leg"));
        } else {
            PJ_LOG(3, (THIS_FILE, "[B-LEG-HDR] Route2(SSP) omitted — same as Kamailio (%s:%d)",
                       ssp_host, ssp_port));
        }
    }

    target = pj_str(b_uri);
    status = pjsua_call_make_call(acc_id,
                                   &target, &cs, session,
                                   &msg_data, &call_b);
    if (hdr_pool) pj_pool_release(hdr_pool);

    if (status != PJ_SUCCESS || call_b == PJSUA_INVALID_ID) {
        PJ_LOG(1, (THIS_FILE, "[ERROR] Originate to %s failed: %d", b_uri, status));
        CC_SESSION_LOCK(session);
        session->torn_down = 1;
        session->b_origination_pending = 0;
        snprintf(session->final_status, sizeof(session->final_status),
                 "FAILED");
        snprintf(session->final_reason, sizeof(session->final_reason),
                 "SYSTEM_ERROR");
        CC_SESSION_UNLOCK(session);
        leg_a_play_unavailable_then_hangup(session);
        {
            int treatment_armed = 0;
            CC_SESSION_LOCK(session);
            treatment_armed = session->a_treatment_running;
            CC_SESSION_UNLOCK(session);
            if (!treatment_armed)
                cc_session_mark_end(session, "FAILED", "SYSTEM_ERROR");
        }
        return NULL;
    }

    {
        int stale_b = 0;

        CC_SESSION_LOCK(session);
        if (session->torn_down ||
            session->call_a == PJSUA_INVALID_ID ||
            (session->call_b != PJSUA_INVALID_ID &&
             session->call_b != call_b))
        {
            stale_b = 1;
        } else {
            session->call_b = call_b;
        }
        session->b_origination_pending = 0;
        CC_SESSION_UNLOCK(session);

        if (stale_b) {
            PJ_LOG(3, (THIS_FILE,
                       "[TIMER] skipped stale action: B-leg completed after teardown call=%d",
                       call_b));
            if (pjsua_call_get_user_data(call_b) == session)
                pjsua_call_set_user_data(call_b, NULL);
            if (pjsua_call_is_active(call_b))
                cc_safe_hangup(call_b, PJSIP_SC_OK);
            return NULL;
        }
    }

    /* make_call() received session as user_data; reinforce the association. */
    pjsua_call_set_user_data(call_b, session);

    /* Start ring timeout watchdog */
    leg_b_start_ring_timer(session);

    PJ_LOG(3, (THIS_FILE, "B leg call_id=%d started", call_b));
    return NULL;
}

/* ── Global PJSUA callbacks ──────────────────────────────────────────────── */

static int cc_resolve_leg(cc_session_t *session, pjsua_call_id call_id)
{
    int leg = 0;

    CC_SESSION_LOCK(session);
    if (call_id == session->call_a) {
        leg = 1;
    } else if (call_id == session->call_b) {
        leg = 2;
    } else if (session->call_b == PJSUA_INVALID_ID &&
               session->b_leg_started &&
               !session->torn_down &&
               !session->final_cleanup_started &&
               call_id != session->call_a)
    {
        session->call_b = call_id;
        leg = 2;
        PJ_LOG(3, (THIS_FILE,
                   "[SESSION] early B-leg associated call=%d session=%p",
                   call_id, session));
    }
    CC_SESSION_UNLOCK(session);

    return leg;
}

static const char *cc_dtmf_method_name(pjsua_dtmf_method method)
{
    switch (method) {
    case PJSUA_DTMF_METHOD_RFC2833:
        return "RFC2833";
    case PJSUA_DTMF_METHOD_SIP_INFO:
        return "SIP_INFO";
    default:
        return "UNKNOWN";
    }
}

static void cc_dispatch_dtmf(pjsua_call_id call_id,
                             int digit,
                             pjsua_dtmf_method method,
                             unsigned duration,
                             cc_session_t *session)
{
    int leg = cc_resolve_leg(session, call_id);
    const char *leg_name = leg == 1 ? "A" : (leg == 2 ? "B" : "UNKNOWN");

    if (duration == (unsigned)-1) {
        PJ_LOG(3, (THIS_FILE,
                   "[DTMF] call_id=%d leg=%s digit=%c method=%s duration=unknown",
                   call_id,
                   leg_name,
                   (char)digit,
                   cc_dtmf_method_name(method)));
    } else {
        PJ_LOG(3, (THIS_FILE,
                   "[DTMF] call_id=%d leg=%s digit=%c method=%s duration=%u",
                   call_id,
                   leg_name,
                   (char)digit,
                   cc_dtmf_method_name(method),
                   duration));
    }

    if (leg == 2) {
        leg_b_on_dtmf(call_id, digit, session);
    } else if (leg == 1) {
        int mca_waiting = 0;
        CC_SESSION_LOCK(session);
        mca_waiting = session->mca_waiting;
        CC_SESSION_UNLOCK(session);

        if (mca_waiting) {
            leg_a_on_dtmf_mca(call_id, digit, session);
        } else {
            PJ_LOG(3, (THIS_FILE,
                       "[DTMF] A-leg digit=%c ignored; no MCA pending",
                       (char)digit));
        }
    } else {
        PJ_LOG(2, (THIS_FILE,
                   "[DTMF] digit ignored; call is not associated with A or B leg"));
    }
}

void cc_on_call_state(pjsua_call_id call_id, pjsip_event *e)
{
    cc_session_t *session = NULL;
    pjsua_call_id deferred_hangup = PJSUA_INVALID_ID;
    int leg;

    session = (cc_session_t *)pjsua_call_get_user_data(call_id);
    if (!session || !cc_session_acquire_reason(session, "callback-call-state"))
        return;

    leg = cc_resolve_leg(session, call_id);

    /* ng answer/re-offer on any SDP-bearing SIP message (200/183/UPDATE/re-INVITE). */
    if (cc_rtpengine_enabled() && e) {
        pjsip_rx_data *rdata = NULL;
        if (e->type == PJSIP_EVENT_TSX_STATE &&
            e->body.tsx_state.type == PJSIP_EVENT_RX_MSG)
            rdata = e->body.tsx_state.src.rdata;
        else if (e->type == PJSIP_EVENT_RX_MSG)
            rdata = e->body.rx_msg.rdata;
        if (rdata) {
            if (leg == 1)
                (void)cc_rtpengine_offer_from_rdata(session, rdata);
            else if (leg == 2) {
                (void)cc_rtpengine_answer_from_rdata(session, rdata);
                if (cc_rtpengine_consume_a_ep_changed(session))
                    leg_a_on_rtpengine_a_ep_changed(session);
            }
        }
    }
    if (leg == 1)
        deferred_hangup = leg_a_on_call_state(call_id, session);
    else if (leg == 2) {
        /* Track SBC-initiated re-INVITEs on B-leg (late-offer completion).
         * Set b_reinvite_active when a new INVITE transaction starts on the
         * B-leg dialog; clear it when the dialog returns to CONFIRMED.
         * This blocks UPDATE dispatch while the re-INVITE is in progress. */
        {
            pjsua_call_info ci_b;
            if (pjsua_call_get_info(call_id, &ci_b) == PJ_SUCCESS) {
                if (ci_b.state == PJSIP_INV_STATE_CONNECTING) {
                    CC_SESSION_LOCK(session);
                    session->b_reinvite_active = 1;
                    CC_SESSION_UNLOCK(session);
                    PJ_LOG(3, (THIS_FILE,
                               "[B] re-INVITE in progress (CONNECTING) — UPDATE blocked"));
                } else if (ci_b.state == PJSIP_INV_STATE_CONFIRMED) {
                    CC_SESSION_LOCK(session);
                    session->b_reinvite_active = 0;
                    CC_SESSION_UNLOCK(session);
                }
            }
        }
        leg_b_on_call_state(call_id, session);
    }

    /* Capture Allow: UPDATE from B-leg 200 OK (REQ-18: per-dialog capability check).
     * pjsua_call_get_info exposes last_status; the Allow header is in the
     * response tdata which is not directly accessible here.  We use a
     * conservative heuristic: if the B-leg reaches CONFIRMED it has
     * successfully processed at least one UPDATE (the bypass UPDATE sent
     * during setup), so UPDATE is confirmed supported.  Set the flag once. */
    if (leg == 2) {
        pjsua_call_info ci_b;
        if (pjsua_call_get_info(call_id, &ci_b) == PJ_SUCCESS &&
            ci_b.state == PJSIP_INV_STATE_CONFIRMED)
        {
            CC_SESSION_LOCK(session);
            session->b_update_allowed = 1;
            CC_SESSION_UNLOCK(session);
        }
    }

    /* Mirror of the above for A-leg (needed now that hold propagation can
     * also target A — see ev_hold_propagate_a/ev_resume_propagate_a). */
    if (leg == 1) {
        pjsua_call_info ci_a;
        if (pjsua_call_get_info(call_id, &ci_a) == PJ_SUCCESS &&
            ci_a.state == PJSIP_INV_STATE_CONFIRMED)
        {
            CC_SESSION_LOCK(session);
            session->a_update_allowed = 1;
            CC_SESSION_UNLOCK(session);
        }
    }

    cc_session_release_reason(session, "callback-call-state");

    if (deferred_hangup != PJSUA_INVALID_ID) {
        PJ_LOG(3, (THIS_FILE,
                   "[VALIDATION] callback reference released; hangup rejected A-leg call=%d",
                   deferred_hangup));
        cc_safe_hangup(deferred_hangup, PJSIP_SC_FORBIDDEN);
    }
}


static void spawn_update_b_retry(cc_session_t *session, pjsua_call_id call_b)
{
    cc_event_t ev;

    CC_SESSION_LOCK(session);
    if (session->update_b_retry_pending ||
        session->media_bypassed ||
        session->update_b_acked ||
        session->torn_down)
    {
        CC_SESSION_UNLOCK(session);
        return;
    }
    session->update_b_retry_pending = 1;
    CC_SESSION_UNLOCK(session);

    memset(&ev, 0, sizeof(ev));
    ev.type    = CC_EV_UPDATE_B_RETRY;
    ev.session = session;
    ev.call_b  = call_b;
    snprintf(ev.reason, sizeof(ev.reason), "update-b-retry-worker");

    if (cc_worker_post_delayed(&ev, 3000) != 0) {
        CC_SESSION_LOCK(session);
        session->update_b_retry_pending = 0;
        CC_SESSION_UNLOCK(session);
    }
}


static void spawn_update_a_retry(cc_session_t *session, pjsua_call_id call_a)
{
    cc_event_t ev;

    CC_SESSION_LOCK(session);
    if (session->update_a_retry_pending ||
        session->media_bypassed ||
        session->update_a_acked ||
        session->torn_down)
    {
        CC_SESSION_UNLOCK(session);
        return;
    }
    session->update_a_retry_pending = 1;
    CC_SESSION_UNLOCK(session);

    memset(&ev, 0, sizeof(ev));
    ev.type    = CC_EV_UPDATE_A_RETRY;
    ev.session = session;
    ev.call_a  = call_a;
    snprintf(ev.reason, sizeof(ev.reason), "update-a-retry-worker");

    if (cc_worker_post_delayed(&ev, 3000) != 0) {
        CC_SESSION_LOCK(session);
        session->update_a_retry_pending = 0;
        CC_SESSION_UNLOCK(session);
    }
}

/* ── UPDATE 200 OK timeout watchdog ─────────────────────────────────────── */
/*
 * If either UPDATE 200 OK does not arrive within CC_UPDATE_ACK_TIMEOUT_MS,
 * fall back to local bridge so the call is not left in a broken state.
 * This covers: SBC drops UPDATE silently, network loss, non-200 that PJSUA
 * does not surface as a media-state callback.
 */
#define CC_UPDATE_ACK_TIMEOUT_MS  8000
#define CC_UPDATE_ACK_POLL_MS       100


static void spawn_update_ack_watchdog(cc_session_t *session,
                                      pjsua_call_id call_a,
                                      pjsua_call_id call_b)
{
    cc_event_t ev;

    CC_SESSION_LOCK(session);
    if (session->update_ack_watchdog_started || session->torn_down) {
        CC_SESSION_UNLOCK(session);
        return;
    }
    session->update_ack_watchdog_started = 1;
    CC_SESSION_UNLOCK(session);

    memset(&ev, 0, sizeof(ev));
    ev.type    = CC_EV_UPDATE_ACK_WATCHDOG;
    ev.session = session;
    ev.call_a  = call_a;
    ev.call_b  = call_b;
    snprintf(ev.reason, sizeof(ev.reason), "update-ack-watchdog-worker");

    if (cc_worker_post_delayed(&ev, CC_UPDATE_ACK_TIMEOUT_MS) != 0) {
        CC_SESSION_LOCK(session);
        session->update_ack_watchdog_started = 0;
        CC_SESSION_UNLOCK(session);
        return;
    }
    PJ_LOG(3, (THIS_FILE,
               "[UPDATE-WD] watchdog posted — fallback to bridge if no 200 OK in %dms",
               CC_UPDATE_ACK_TIMEOUT_MS));
}

void cc_on_call_media_state(pjsua_call_id call_id)
{
    cc_session_t *session;
    int leg;

    session = (cc_session_t *)pjsua_call_get_user_data(call_id);
    if (!session || !cc_session_acquire_reason(session, "callback-media-state"))
        return;

    leg = cc_resolve_leg(session, call_id);
    if (leg == 1)
        leg_a_on_media_state(call_id, session);
    else if (leg == 2)
        leg_b_on_media_state(call_id, session);

    /* Prompts and conversation RTP live on RTPengine — mute local TX/RX so
     * PJSUA does not emit a second stream toward the MGW, and isolate from
     * the master mix so silence frames are not fed into the paused stream. */
    if (cc_rtpengine_enabled()) {
        cc_silence_call(call_id);
        cc_isolate_call_from_master(call_id);
    }

    /*
     * UPDATE bypass post-processing: once both UPDATE 200 OKs have been
     * processed by PJSUA (triggering on_call_media_state on each leg),
     * silence both conf slots so B2BUA stops transmitting RTP.
     * This is the industry-standard approach: the B2BUA exits the media
     * path by disconnecting its conf ports after the UPDATEs complete.
     */
    if (cc_cfg_media_uses_update()) {
        pjsua_call_id call_a, call_b;
        int do_silence = 0;
        int do_hold = 0;
        int do_resume = 0;
        pjsua_call_id resume_call_a = PJSUA_INVALID_ID;
        pjsua_call_id resume_call_b = PJSUA_INVALID_ID;

        CC_SESSION_LOCK(session);
        call_a = session->call_a;
        call_b = session->call_b;
        if (!session->media_bypassed &&
            !session->torn_down &&
            session->accepted)
        {
            if (call_id == call_a && session->update_a_sent)
                session->update_a_acked = 1;
            else if (call_id == call_b && session->update_b_sent)
                session->update_b_acked = 1;

            if (session->update_a_acked && session->update_b_acked) {
                session->media_bypassed = 1;
                do_silence = 1;
            } else if (session->update_a_acked &&
                       session->update_b_sent &&
                       !session->update_b_acked) {
                /* A acked but B hasn't — likely a 491 on B-leg.
                 * Spawn a retry after a short delay. */
                do_silence = 0;
            }
        } else if (session->media_bypassed &&
                   !session->torn_down &&
                   session->accepted)
        {
            /* Post-bypass: either leg media state changed — hold or resume.
             * Also detect B's 200 OK to a hold/resume UPDATE (REQ-14/REQ-19):
             * when hold_propagate_pending is set and B's media state fires,
             * that is B confirming the hold UPDATE — set hold_update_b_acked. */
            if (call_id == call_b && session->hold_propagate_pending &&
                !session->hold_update_b_acked)
            {
                session->hold_update_b_acked = 1;
                PJ_LOG(3, (THIS_FILE, "[HOLD/M2] B hold UPDATE 200 OK detected"));
            }
            /* Mirror: detect A's 200 OK to a hold/resume UPDATE propagated
             * because B went on hold — set hold_update_a_acked. */
            if (call_id == call_a && session->hold_propagate_a_pending &&
                !session->hold_update_a_acked)
            {
                session->hold_update_a_acked = 1;
                PJ_LOG(3, (THIS_FILE, "[HOLD/M2] A hold UPDATE 200 OK detected"));
            }
            if (call_id == call_b) {
                pjsua_call_info ci_b;
                if (pjsua_call_get_info(call_b, &ci_b) == PJ_SUCCESS) {
                    if (ci_b.media[0].status == PJSUA_CALL_MEDIA_LOCAL_HOLD ||
                        ci_b.media[0].status == PJSUA_CALL_MEDIA_REMOTE_HOLD)
                    {
                        if (!session->b_on_hold) {
                            session->b_on_hold = 1;
                            do_hold = 1;
                        }
                    } else if (ci_b.media[0].status == PJSUA_CALL_MEDIA_ACTIVE) {
                        if (session->b_on_hold) {
                            session->b_on_hold = 0;
                            do_resume = 1;
                            resume_call_a = call_a;
                            resume_call_b = call_b;
                            session->media_bypassed          = 0;
                            session->update_a_sent           = 0;
                            session->update_b_sent           = 0;
                            session->update_a_acked          = 0;
                            session->update_b_acked          = 0;
                            session->update_b_retry_pending  = 0;
                            session->update_a_retry_pending  = 0;
                            session->update_ack_watchdog_started = 0;
                            session->b_reinvite_active       = 0;
                        }
                    }
                }
            } else if (call_id == call_a) {
                pjsua_call_info ci_a;
                if (pjsua_call_get_info(call_a, &ci_a) == PJ_SUCCESS) {
                    if (ci_a.media[0].status == PJSUA_CALL_MEDIA_LOCAL_HOLD ||
                        ci_a.media[0].status == PJSUA_CALL_MEDIA_REMOTE_HOLD)
                    {
                        if (!session->a_on_hold) {
                            session->a_on_hold = 1;
                            do_hold = 2;
                        }
                    } else if (ci_a.media[0].status == PJSUA_CALL_MEDIA_ACTIVE) {
                        if (session->a_on_hold) {
                            session->a_on_hold = 0;
                            do_resume = 2;
                            resume_call_a = call_a;
                            resume_call_b = call_b;
                            session->media_bypassed          = 0;
                            session->update_a_sent           = 0;
                            session->update_b_sent           = 0;
                            session->update_a_acked          = 0;
                            session->update_b_acked          = 0;
                            session->update_b_retry_pending  = 0;
                            session->update_a_retry_pending  = 0;
                            session->update_ack_watchdog_started = 0;
                            session->b_reinvite_active       = 0;
                        }
                    }
                }
            }
        }
        CC_SESSION_UNLOCK(session);

        if (do_silence) {
            PJ_LOG(3, (THIS_FILE,
                       "[BYPASS] both UPDATE 200 OKs received — silencing B2BUA conf slots"));
            cc_silence_call(call_a);
            cc_silence_call(call_b);
            if (!cc_rtpengine_enabled())
                cc_spawn_bypass_rtp_watchdog(session, call_a, call_b);
        }

        /* A acked but B hasn't — 491 on B-leg; retry after delay */
        {
            int need_retry = 0;
            CC_SESSION_LOCK(session);
            if (!session->media_bypassed &&
                !session->torn_down &&
                session->accepted &&
                session->update_a_acked &&
                session->update_b_sent &&
                !session->update_b_acked &&
                !session->update_b_retry_pending)
            {
                need_retry = 1;
            }
            CC_SESSION_UNLOCK(session);
            if (need_retry) {
                PJ_LOG(3, (THIS_FILE,
                           "[BYPASS] A UPDATE acked but B UPDATE not acked — spawning 491 retry"));
                spawn_update_b_retry(session, call_b);
            }
        }

        /* B acked but A hasn't — 491 on A-leg; retry after delay */
        {
            int need_retry = 0;
            CC_SESSION_LOCK(session);
            if (!session->media_bypassed &&
                !session->torn_down &&
                session->accepted &&
                session->update_b_acked &&
                session->update_a_sent &&
                !session->update_a_acked &&
                !session->update_a_retry_pending)
            {
                need_retry = 1;
            }
            CC_SESSION_UNLOCK(session);
            if (need_retry) {
                PJ_LOG(3, (THIS_FILE,
                           "[BYPASS] B UPDATE acked but A UPDATE not acked — spawning 491 retry"));
                spawn_update_a_retry(session, call_a);
            }
        }

        /* Spawn UPDATE ack timeout watchdog once both UPDATEs are sent.
         *
         * NOTE: this is now a SECONDARY safety net only. The primary arm
         * point moved to worker.c's send path (cc_maybe_arm_update_ack_watchdog,
         * called from ev_update_a_bypass/ev_update_b_bypass/
         * ev_reinvite_a_bypass/ev_reinvite_b_bypass right after a successful
         * send) — because this callback only fires on a SUCCESSFUL media
         * renegotiation, so a rejected (e.g. 405) or unanswered UPDATE never
         * reaches this code at all, leaving the watchdog unarmed in exactly
         * the failure case it exists to catch (see rtp_test_1.pcap /
         * collect_call log analysis: 10s of silence, no watchdog, call
         * killed by a PJSIP-level transaction timeout instead of falling
         * back to local bridge). Left here so the watchdog also gets armed
         * in the reverse ordering (media_state fires before both sends are
         * observed by the worker for some reason) — update_ack_watchdog_started
         * makes this idempotent with the send-path arm. */
        {
            int need_watchdog = 0;
            CC_SESSION_LOCK(session);
            if (!session->media_bypassed &&
                !session->torn_down &&
                session->accepted &&
                session->update_a_sent &&
                session->update_b_sent &&
                !session->update_ack_watchdog_started)
            {
                need_watchdog = 1;
            }
            CC_SESSION_UNLOCK(session);
            if (need_watchdog)
                spawn_update_ack_watchdog(session, call_a, call_b);
        }
        if (do_hold == 1) {
            /* B put the call on hold.
             * Mirrors do_hold==2 exactly (A<->B swapped): propagate
             * sendonly to A via UPDATE. Post CC_EV_HOLD_PROPAGATE_A; worker
             * sends UPDATE sendonly to A, waits for 200 OK, then plays MOH
             * to B — matching the target convention do_hold==2 already
             * uses (MOH to the leg that ISN'T re-signaled). No local-only
             * shortcut anymore.
             *
             * NOTE ON STRAY RTP: ev_hold_propagate_a() below only resumes
             * TX + bridges call_b (to carry MOH), leaving call_a's B2BUA
             * side stream paused. Resuming call_a's TX here too would
             * revive B2BUA-origin RTP toward A's real address even though
             * A's SBC is expecting direct bypass RTP from B — that duplicate
             * stream is exactly the "stray RTP after UPDATE" bug from the
             * pcap. If your SBC-side testing shows the existing do_hold==2
             * path (which mirrors this one) needs both legs resumed, apply
             * the same change to both symmetrically — don't diverge the two
             * directions again. */
            int already;
            CC_SESSION_LOCK(session);
            already = session->hold_propagate_a_pending || session->torn_down;
            if (!already) session->hold_propagate_a_pending = 1;
            CC_SESSION_UNLOCK(session);
            if (!already) {
                cc_event_t hev;
                memset(&hev, 0, sizeof(hev));
                hev.type    = CC_EV_HOLD_PROPAGATE_A;
                hev.session = session;
                hev.call_a  = call_a;
                hev.call_b  = call_b;
                snprintf(hev.reason, sizeof(hev.reason), "hold-propagate-a");
                if (cc_worker_post(&hev) != 0) {
                    CC_SESSION_LOCK(session);
                    session->hold_propagate_a_pending = 0;
                    CC_SESSION_UNLOCK(session);
                    PJ_LOG(1, (THIS_FILE, "[HOLD/M2] hold propagate-A post failed"));
                } else {
                    PJ_LOG(3, (THIS_FILE, "[HOLD/M2] B on hold — posted hold propagation to A"));
                }
            }
        }

        if (do_hold == 2) {
            /* A put the call on hold.
             * Mode 2 (REQ-11..REQ-15): propagate sendonly to B via UPDATE.
             * Post CC_EV_HOLD_PROPAGATE_B; worker sends UPDATE sendonly to B,
             * waits for 200 OK, then plays MOH to A.
             * PJSUA has already answered A's re-INVITE locally (recvonly) —
             * REQ-14 cannot be satisfied synchronously in a PJSUA callback;
             * the local answer is immediate and the B propagation is async. */
            int already;
            CC_SESSION_LOCK(session);
            already = session->hold_propagate_pending || session->torn_down;
            if (!already) session->hold_propagate_pending = 1;
            CC_SESSION_UNLOCK(session);
            if (!already) {
                cc_event_t hev;
                memset(&hev, 0, sizeof(hev));
                hev.type    = CC_EV_HOLD_PROPAGATE_B;
                hev.session = session;
                hev.call_a  = call_a;
                hev.call_b  = call_b;
                snprintf(hev.reason, sizeof(hev.reason), "hold-propagate-b");
                if (cc_worker_post(&hev) != 0) {
                    CC_SESSION_LOCK(session);
                    session->hold_propagate_pending = 0;
                    CC_SESSION_UNLOCK(session);
                    PJ_LOG(1, (THIS_FILE, "[HOLD/M2] hold propagate post failed"));
                } else {
                    PJ_LOG(3, (THIS_FILE, "[HOLD/M2] A on hold — posted hold propagation to B"));
                }
            }
        }

        if (do_resume == 1) {
            /* B resumed — stop MOH on A, re-send bypass UPDATEs to restore direct RTP.
             * B's resume is local to B's dialog; no A-leg signaling needed (symmetric REQ-9). */
            pjsua_player_id moh_pid = PJSUA_INVALID_ID;
            PJ_LOG(3, (THIS_FILE, "[HOLD/M2] B resumed — stopping MOH, re-sending bypass UPDATEs"));
            CC_SESSION_LOCK(session);
            if (session->hold_player_a != PJSUA_INVALID_ID) {
                moh_pid = session->hold_player_a;
                session->hold_player_a = PJSUA_INVALID_ID;
            }
            CC_SESSION_UNLOCK(session);
            if (moh_pid != PJSUA_INVALID_ID)
                cc_stop_wav(moh_pid, PJSUA_INVALID_ID);
            leg_a_send_update_bypass(resume_call_a, session);
            leg_b_send_update_bypass(resume_call_b, session);
        }

        if (do_resume == 2) {
            /* A resumed.
             * Mode 2 (REQ-17..REQ-20): propagate sendrecv to B via UPDATE.
             * Post CC_EV_RESUME_PROPAGATE_B; worker sends UPDATE sendrecv to B,
             * waits for 200 OK, then re-runs bypass UPDATEs to restore direct RTP.
             * hold_update_b_acked check guards against duplicate resume events. */
            int already;
            CC_SESSION_LOCK(session);
            already = session->resume_propagate_pending || session->torn_down;
            if (!already) session->resume_propagate_pending = 1;
            CC_SESSION_UNLOCK(session);
            if (!already) {
                cc_event_t rev;
                memset(&rev, 0, sizeof(rev));
                rev.type    = CC_EV_RESUME_PROPAGATE_B;
                rev.session = session;
                rev.call_a  = resume_call_a;
                rev.call_b  = resume_call_b;
                snprintf(rev.reason, sizeof(rev.reason), "resume-propagate-b");
                if (cc_worker_post(&rev) != 0) {
                    CC_SESSION_LOCK(session);
                    session->resume_propagate_pending = 0;
                    CC_SESSION_UNLOCK(session);
                    PJ_LOG(1, (THIS_FILE, "[HOLD/M2] resume propagate post failed"));
                } else {
                    PJ_LOG(3, (THIS_FILE, "[HOLD/M2] A resumed — posted resume propagation to B"));
                }
            }
        }
    }

    if (cc_cfg_media_mode() == CC_MEDIA_MODE_LOCAL_BRIDGE) {
        pjsua_call_id call_a, call_b;
        int do_hold = 0;
        int do_resume = 0;

        CC_SESSION_LOCK(session);
        call_a = session->call_a;
        call_b = session->call_b;
        if (session->accepted && !session->torn_down) {
            if (call_id == call_b) {
                pjsua_call_info ci_b;
                if (pjsua_call_get_info(call_b, &ci_b) == PJ_SUCCESS) {
                    if (ci_b.media[0].status == PJSUA_CALL_MEDIA_LOCAL_HOLD ||
                        ci_b.media[0].status == PJSUA_CALL_MEDIA_REMOTE_HOLD)
                    {
                        if (!session->b_on_hold) {
                            session->b_on_hold = 1;
                            do_hold = 1;
                        }
                    } else if (ci_b.media[0].status == PJSUA_CALL_MEDIA_ACTIVE) {
                        if (session->b_on_hold) {
                            session->b_on_hold = 0;
                            do_resume = 1;
                        }
                    }
                }
            } else if (call_id == call_a) {
                pjsua_call_info ci_a;
                if (pjsua_call_get_info(call_a, &ci_a) == PJ_SUCCESS) {
                    if (ci_a.media[0].status == PJSUA_CALL_MEDIA_LOCAL_HOLD ||
                        ci_a.media[0].status == PJSUA_CALL_MEDIA_REMOTE_HOLD)
                    {
                        if (!session->a_on_hold) {
                            session->a_on_hold = 1;
                            do_hold = 2;
                        }
                    } else if (ci_a.media[0].status == PJSUA_CALL_MEDIA_ACTIVE) {
                        if (session->a_on_hold) {
                            session->a_on_hold = 0;
                            do_resume = 2;
                        }
                    }
                }
            }
        }
        CC_SESSION_UNLOCK(session);

        if (do_hold == 1) {
            pjsua_player_id moh_pid = PJSUA_INVALID_ID;
            PJ_LOG(3, (THIS_FILE, "[HOLD/LB] B on hold — unbridging, playing MOH to A"));
            cc_unbridge_calls(call_a, call_b);
            if (cc_session_call_is_current(session, call_a, 1))
                moh_pid = cc_start_wav(call_a, cc_prompt_get_path(CC_PROMPT_MOH), PJ_TRUE);
            CC_SESSION_LOCK(session);
            if (moh_pid != PJSUA_INVALID_ID &&
                session->hold_player_a == PJSUA_INVALID_ID &&
                !session->torn_down)
            {
                session->hold_player_a = moh_pid;
            } else if (moh_pid != PJSUA_INVALID_ID) {
                CC_SESSION_UNLOCK(session);
                cc_stop_wav(moh_pid, PJSUA_INVALID_ID);
                goto lb_hold_done;
            }
            CC_SESSION_UNLOCK(session);
            lb_hold_done:;
        }

        if (do_hold == 2) {
            pjsua_player_id moh_pid = PJSUA_INVALID_ID;
            PJ_LOG(3, (THIS_FILE, "[HOLD/LB] A on hold — unbridging, playing MOH to B"));
            cc_unbridge_calls(call_a, call_b);
            if (cc_session_call_is_current(session, call_b, 0))
                moh_pid = cc_start_wav(call_b, cc_prompt_get_path(CC_PROMPT_MOH), PJ_TRUE);
            CC_SESSION_LOCK(session);
            if (moh_pid != PJSUA_INVALID_ID &&
                session->hold_player_b == PJSUA_INVALID_ID &&
                !session->torn_down)
            {
                session->hold_player_b = moh_pid;
            } else if (moh_pid != PJSUA_INVALID_ID) {
                CC_SESSION_UNLOCK(session);
                cc_stop_wav(moh_pid, PJSUA_INVALID_ID);
                goto lb_hold_b_done;
            }
            CC_SESSION_UNLOCK(session);
            lb_hold_b_done:;
        }

        if (do_resume == 1) {
            pjsua_player_id moh_pid = PJSUA_INVALID_ID;
            PJ_LOG(3, (THIS_FILE, "[HOLD/LB] B resumed — stopping MOH, re-bridging"));
            CC_SESSION_LOCK(session);
            if (session->hold_player_a != PJSUA_INVALID_ID) {
                moh_pid = session->hold_player_a;
                session->hold_player_a = PJSUA_INVALID_ID;
            }
            CC_SESSION_UNLOCK(session);
            if (moh_pid != PJSUA_INVALID_ID)
                cc_stop_wav(moh_pid, PJSUA_INVALID_ID);
            cc_bridge_calls(call_a, call_b);
        }

        if (do_resume == 2) {
            pjsua_player_id moh_pid = PJSUA_INVALID_ID;
            PJ_LOG(3, (THIS_FILE, "[HOLD/LB] A resumed — stopping MOH, re-bridging"));
            CC_SESSION_LOCK(session);
            if (session->hold_player_b != PJSUA_INVALID_ID) {
                moh_pid = session->hold_player_b;
                session->hold_player_b = PJSUA_INVALID_ID;
            }
            CC_SESSION_UNLOCK(session);
            if (moh_pid != PJSUA_INVALID_ID)
                cc_stop_wav(moh_pid, PJSUA_INVALID_ID);
            cc_bridge_calls(call_a, call_b);
        }
    }

    cc_session_release_reason(session, "callback-media-state");
}

void cc_on_dtmf_digit(pjsua_call_id call_id, int digit)
{
    cc_session_t *session;

    session = (cc_session_t *)pjsua_call_get_user_data(call_id);
    if (!session || !cc_session_acquire_reason(session, "callback-dtmf"))
        return;

    /*
     * PJSIP uses this legacy callback for RFC2833 only when digit2 is not
     * implemented. Keep it wired for compatibility with older deployments.
     */
    cc_dispatch_dtmf(call_id,
                     digit,
                     PJSUA_DTMF_METHOD_RFC2833,
                     (unsigned)-1,
                     session);

    cc_session_release_reason(session, "callback-dtmf");
}

void cc_on_dtmf_digit2(pjsua_call_id call_id,
                       const pjsua_dtmf_info *info)
{
    cc_session_t *session;
    int digit;

    if (!info)
        return;

    session = (cc_session_t *)pjsua_call_get_user_data(call_id);
    if (!session || !cc_session_acquire_reason(session, "callback-dtmf2"))
        return;

    digit = info->digit;

    /*
     * This is the primary callback for both RFC2833 telephone-event and
     * SIP INFO DTMF. Both methods enter the same B-leg decision state machine.
     */
    cc_dispatch_dtmf(call_id,
                     digit,
                     info->method,
                     info->duration,
                     session);

    cc_session_release_reason(session, "callback-dtmf2");
}

/*
 * Align SIP SDP with RTPengine single-codec policy (PCMA + telephone-event).
 * Endpoint rewrite alone left PJSUA advertising PCMU+PCMA; RE then mixed
 * PT 0 and PT 8 toward Linphone. Strip non-PCMA audio PTs on every rewrite.
 */
static int cc_sdp_pt_is_kept(const pjmedia_sdp_media *m, const pj_str_t *fmt)
{
    unsigned j;
    char ptbuf[16];
    pj_str_t te = pj_str("telephone-event");

    if (!m || !fmt || !fmt->ptr || fmt->slen <= 0)
        return 0;

    /* Static PCMA */
    if (fmt->slen == 1 && fmt->ptr[0] == '8')
        return 1;

    if (fmt->slen >= (pj_ssize_t)sizeof(ptbuf))
        return 0;
    memcpy(ptbuf, fmt->ptr, (size_t)fmt->slen);
    ptbuf[fmt->slen] = '\0';

    for (j = 0; j < m->attr_count; j++) {
        const pjmedia_sdp_attr *a = m->attr[j];
        const char *v;
        const char *sp;
        size_t ptlen;

        if (!a || pj_strcmp2(&a->name, "rtpmap") != 0 || !a->value.ptr)
            continue;
        v = a->value.ptr;
        sp = memchr(v, ' ', (size_t)a->value.slen);
        if (!sp)
            continue;
        ptlen = (size_t)(sp - v);
        if (ptlen != (size_t)fmt->slen || memcmp(v, fmt->ptr, ptlen) != 0)
            continue;
        /* rtpmap:<pt> <encoding>/... */
        if (pj_stristr(&a->value, &te) != NULL)
            return 1;
        if (a->value.slen >= (pj_ssize_t)(ptlen + 1 + 4) &&
            strncasecmp(sp + 1, "PCMA", 4) == 0)
            return 1;
        return 0;
    }

    /* No rtpmap: keep only static 8 (already handled). Drop unknown PTs. */
    return 0;
}

void cc_sdp_restrict_audio_pcma_te(pj_pool_t *pool,
                                   pjmedia_sdp_session *sdp,
                                   const char *tag)
{
    pj_size_t mi;
    unsigned removed_total = 0;

    if (!pool || !sdp)
        return;

    for (mi = 0; mi < sdp->media_count; mi++) {
        pjmedia_sdp_media *m = sdp->media[mi];
        pj_str_t keep_fmt[PJMEDIA_MAX_SDP_FMT];
        unsigned keep_n = 0;
        unsigned fi;
        unsigned ai;
        unsigned new_attr_n = 0;
        unsigned removed;

        if (!m || pj_strcmp2(&m->desc.media, "audio") != 0)
            continue;

        for (fi = 0; fi < m->desc.fmt_count; fi++) {
            if (!cc_sdp_pt_is_kept(m, &m->desc.fmt[fi])) {
                removed_total++;
                continue;
            }
            if (keep_n < PJMEDIA_MAX_SDP_FMT)
                keep_fmt[keep_n++] = m->desc.fmt[fi];
        }

        /* Ensure PCMA (PT 8) is always first among kept formats. */
        {
            unsigned k;
            int have_8 = 0;
            for (k = 0; k < keep_n; k++) {
                if (keep_fmt[k].slen == 1 && keep_fmt[k].ptr &&
                    keep_fmt[k].ptr[0] == '8')
                {
                    have_8 = 1;
                    break;
                }
            }
            if (!have_8) {
                if (keep_n >= PJMEDIA_MAX_SDP_FMT)
                    keep_n = PJMEDIA_MAX_SDP_FMT - 1;
                for (k = keep_n; k > 0; k--)
                    keep_fmt[k] = keep_fmt[k - 1];
                keep_fmt[0] = pj_strdup3(pool, "8");
                keep_n++;
                PJ_LOG(2, (THIS_FILE,
                           "[%s] SDP codec restrict: inserting PCMA/8",
                           tag ? tag : "SDP"));
            }
        }

        /* Always advertise rtpmap for PCMA when PT 8 is kept. */
        {
            int have_pcma_map = 0;
            for (ai = 0; ai < m->attr_count; ai++) {
                pjmedia_sdp_attr *a = m->attr[ai];
                if (a && pj_strcmp2(&a->name, "rtpmap") == 0 &&
                    a->value.ptr && a->value.slen >= 6 &&
                    a->value.ptr[0] == '8' && a->value.ptr[1] == ' ' &&
                    strncasecmp(a->value.ptr + 2, "PCMA", 4) == 0)
                {
                    have_pcma_map = 1;
                    break;
                }
            }
            if (!have_pcma_map && m->attr_count < PJMEDIA_MAX_SDP_ATTR) {
                pjmedia_sdp_attr *a = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_attr);
                a->name = pj_str("rtpmap");
                a->value = pj_strdup3(pool, "8 PCMA/8000");
                m->attr[m->attr_count++] = a;
            }
        }

        removed = m->desc.fmt_count > keep_n ? m->desc.fmt_count - keep_n : 0;
        m->desc.fmt_count = keep_n;
        for (fi = 0; fi < keep_n; fi++)
            m->desc.fmt[fi] = keep_fmt[fi];

        /* Drop rtpmap/fmtp for removed PTs; keep other attributes. */
        for (ai = 0; ai < m->attr_count; ai++) {
            pjmedia_sdp_attr *a = m->attr[ai];
            const char *v;
            const char *sp;
            pj_str_t pt;
            unsigned k;
            int is_map = 0;

            if (!a)
                continue;
            if (pj_strcmp2(&a->name, "rtpmap") == 0 ||
                pj_strcmp2(&a->name, "fmtp") == 0)
                is_map = 1;
            if (!is_map) {
                m->attr[new_attr_n++] = a;
                continue;
            }
            if (!a->value.ptr || a->value.slen <= 0)
                continue;
            v = a->value.ptr;
            sp = memchr(v, ' ', (size_t)a->value.slen);
            pt.ptr = (char *)v;
            pt.slen = sp ? (pj_ssize_t)(sp - v) : a->value.slen;
            for (k = 0; k < keep_n; k++) {
                if (pt.slen == keep_fmt[k].slen &&
                    memcmp(pt.ptr, keep_fmt[k].ptr, (size_t)pt.slen) == 0)
                {
                    m->attr[new_attr_n++] = a;
                    break;
                }
            }
        }
        m->attr_count = new_attr_n;

        if (removed > 0) {
            PJ_LOG(3, (THIS_FILE,
                       "[%s] SDP codec restrict: kept PCMA+telephone-event "
                       "(%u fmt, removed %u)",
                       tag ? tag : "SDP", keep_n, removed));
        }
    }

    (void)removed_total;
}

static void cc_rewrite_sdp_audio_endpoint(pj_pool_t *pool,
                                          pjmedia_sdp_session *sdp,
                                          const cc_rtp_ep_t *ep,
                                          const char *tag)
{
    pj_size_t i;
    cc_rtp_ep_t use;
    const char *media_ip;

    if (!pool || !sdp || !ep || !ep->valid)
        return;

    use = *ep;
    media_ip = use.ip;
    /* Never advertise loopback toward phones — prompts would stay on server. */
    if (strcmp(use.ip, "127.0.0.1") == 0 || strcmp(use.ip, "::1") == 0 ||
        strcmp(use.ip, "0.0.0.0") == 0 || use.ip[0] == '\0')
    {
        media_ip = cc_cfg_local_host();
        PJ_LOG(2, (THIS_FILE,
                   "[%s] SDP media IP %s replaced with CC_LOCAL_HOST %s",
                   tag ? tag : "SDP",
                   use.ip[0] ? use.ip : "(empty)",
                   media_ip));
        snprintf(use.ip, sizeof(use.ip), "%s", media_ip);
    }

    /* Keep o= origin aligned with advertised media. Wireshark/SBCs that
     * display Owner Address otherwise keep showing the B2BUA bound IP. */
    sdp->origin.addr = pj_strdup3(pool, use.ip);
    sdp->origin.version++;

    /* Rewrite session-level c= line if present */
    if (sdp->conn) {
        sdp->conn->addr = pj_strdup3(pool, use.ip);
    }

    for (i = 0; i < sdp->media_count; i++) {
        pjmedia_sdp_media *m = sdp->media[i];
        pjmedia_sdp_conn *conn;
        unsigned j;
        int rtcp_updated = 0;
        char rtcp_value[128];

        if (!m)
            continue;

        /* Disable non-audio media like m=text in UPDATE SDP */
        if (pj_strcmp2(&m->desc.media, "audio") != 0) {
            m->desc.port = 0;
            /* Clear connection line on rejected section to avoid
             * confusing SBCs that inspect c= even when port=0 */
            if (m->conn) {
                m->conn->addr = pj_strdup3(pool, "0.0.0.0");
            }
            m->attr_count = 0;
            continue;
        }

        /* Rewrite audio m= port */
        m->desc.port = (pj_uint16_t)use.port;

        /* Rewrite media-level c= line */
        conn = m->conn ? m->conn : sdp->conn;
        if (conn) {
            conn->addr = pj_strdup3(pool, use.ip);
        } else {
            m->conn = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_conn);
            m->conn->net_type  = pj_str("IN");
            m->conn->addr_type = pj_str("IP4");
            m->conn->addr      = pj_strdup3(pool, use.ip);
        }

        /* Rewrite a=rtcp:<port+1> IN IP4 <ip> */
        snprintf(rtcp_value, sizeof(rtcp_value), "%d IN IP4 %s",
                 use.port + 1, use.ip);

        for (j = 0; j < m->attr_count; j++) {
            if (m->attr[j] &&
                pj_strcmp2(&m->attr[j]->name, "rtcp") == 0)
            {
                m->attr[j]->value = pj_strdup3(pool, rtcp_value);
                rtcp_updated = 1;
                break;
            }
        }

        if (!rtcp_updated && m->attr_count < PJMEDIA_MAX_SDP_ATTR) {
            pjmedia_sdp_attr *a;

            a = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_attr);
            a->name = pj_str("rtcp");
            a->value = pj_strdup3(pool, rtcp_value);

            m->attr[m->attr_count++] = a;
        }

        PJ_LOG(3, (THIS_FILE,
                   "[%s] SDP rewritten to RTP %s:%d",
                   tag ? tag : "UPDATE",
                   use.ip,
                   use.port));
    }

    /* After IP/port rewrite: force single audio codec on the wire. */
    cc_sdp_restrict_audio_pcma_te(pool, sdp, tag);
}

static int cc_sdp_has_rtpmap(const pjmedia_sdp_session *sdp,
                             const char *encoding)
{
    pj_size_t i;
    pj_str_t encoding_name;

    if (!sdp || !encoding)
        return 0;

    encoding_name = pj_str((char *)encoding);

    for (i = 0; i < sdp->media_count; i++) {
        const pjmedia_sdp_media *media = sdp->media[i];
        unsigned j;

        if (!media || pj_stricmp2(&media->desc.media, "audio") != 0)
            continue;

        for (j = 0; j < media->attr_count; j++) {
            const pjmedia_sdp_attr *attr = media->attr[j];

            if (attr &&
                pj_stricmp2(&attr->name, "rtpmap") == 0 &&
                pj_stristr(&attr->value, &encoding_name) != NULL)
            {
                return 1;
            }
        }
    }

    return 0;
}

void cc_on_call_sdp_created(pjsua_call_id call_id,
                            pjmedia_sdp_session *sdp,
                            pj_pool_t *pool,
                            const pjmedia_sdp_session *rem_sdp)
{
    cc_session_t *session;
    int do_a = 0;
    int do_b = 0;
    int was_update = 0;
    int was_reinvite = 0;
    int hold_direction = 0;  /* 1=sendonly 2=sendrecv for hold/resume SDP rewrite */
    int leg = 0;
    cc_rtp_ep_t target;
    const char *method = NULL;
    const char *tag = NULL;

    cc_sdp_set_session_name(sdp, pool, CC_SDP_SESSION_NAME);

    session = (cc_session_t *)pjsua_call_get_user_data(call_id);
    if (!session || !cc_session_acquire_reason(session, "callback-sdp"))
        return;

    /*
     * Associate early B INVITE before rewrite decisions. make_call() fires
     * on_call_sdp_created before it returns call_b; without this, B SDP kept
     * PJSUA 127.0.0.1:106xx and RE played prompts to loopback.
     */
    leg = cc_resolve_leg(session, call_id);

    if (cc_rtpengine_enabled() && rem_sdp) {
        if (leg == 1)
            (void)cc_rtpengine_offer_from_sdp(session, rem_sdp);
        else if (leg == 2) {
            (void)cc_rtpengine_answer_from_sdp(session, rem_sdp);
            if (cc_rtpengine_consume_a_ep_changed(session))
                leg_a_on_rtpengine_a_ep_changed(session);
        }
    }

    memset(&target, 0, sizeof(target));

    CC_SESSION_LOCK(session);

    {
        cc_rtp_ep_t a_tgt, b_tgt;
        int have_a_tgt = 0;
        int have_b_tgt = 0;

        if (cc_rtpengine_sdp_target(session, 1, &a_tgt))
            have_a_tgt = 1;
        else if (session->rtp_b.port != 0) {
            a_tgt = session->rtp_b;
            have_a_tgt = 1;
        }
        if (cc_rtpengine_sdp_target(session, 0, &b_tgt))
            have_b_tgt = 1;
        else if (session->rtp_a.port != 0) {
            b_tgt = session->rtp_a;
            have_b_tgt = 1;
        }

    if (call_id == session->call_a &&
        (session->update_a_pending || session->reinvite_a_pending) &&
        have_a_tgt)
    {
        do_a = 1;
        target = a_tgt;
        was_update = session->update_a_pending;
        was_reinvite = session->reinvite_a_pending;
        session->update_a_pending = 0;
        session->reinvite_a_pending = 0;
    }
    else if (call_id == session->call_a &&
             rem_sdp != NULL &&
             session->accepted &&
             !session->torn_down &&
             have_a_tgt &&
             (session->media_bypassed || session->a_on_hold))
    {
        /* Incoming A-leg re-INVITE (hold/resume from the network). */
        do_a = 1;
        target = a_tgt;
        was_reinvite = 1;
        PJ_LOG(3, (THIS_FILE,
                   "[SDP-REWRITE] A-leg incoming re-INVITE answer: keep RTP %s:%d",
                   a_tgt.ip,
                   a_tgt.port));
    }
    else if (call_id == session->call_b &&
             session->hold_sdp_b_pending &&
             have_b_tgt)
    {
        do_b = 1;
        target = b_tgt;
        was_update = 1;
        session->hold_sdp_b_pending = 0;
        session->hold_sdp_direction = 1;  /* sendonly */
        hold_direction = 1;
    }
    else if (call_id == session->call_b &&
             session->resume_sdp_b_pending &&
             have_b_tgt)
    {
        do_b = 1;
        target = b_tgt;
        was_update = 1;
        session->resume_sdp_b_pending = 0;
        session->hold_sdp_direction = 2;  /* sendrecv */
        hold_direction = 2;
    }
    else if (call_id == session->call_a &&
             session->hold_sdp_a_pending &&
             have_a_tgt)
    {
        do_a = 1;
        target = a_tgt;
        was_update = 1;
        session->hold_sdp_a_pending = 0;
        session->hold_sdp_direction = 1;  /* sendonly */
        hold_direction = 1;
    }
    else if (call_id == session->call_a &&
             session->resume_sdp_a_pending &&
             have_a_tgt)
    {
        do_a = 1;
        target = a_tgt;
        was_update = 1;
        session->resume_sdp_a_pending = 0;
        session->hold_sdp_direction = 2;  /* sendrecv */
        hold_direction = 2;
    }
    else if (call_id == session->call_b &&
             (session->update_b_pending || session->reinvite_b_pending) &&
             have_b_tgt)
    {
        do_b = 1;
        target = b_tgt;
        was_update = session->update_b_pending;
        was_reinvite = session->reinvite_b_pending;
        session->update_b_pending = 0;
        session->reinvite_b_pending = 0;
    }
    else if (cc_rtpengine_enabled() && have_a_tgt &&
             call_id == session->call_a)
    {
        /* Initial A 200 / any A SDP: advertise RTPengine, not local PJSUA RTP. */
        do_a = 1;
        target = a_tgt;
    }
    else if (cc_rtpengine_enabled() && have_b_tgt &&
             (call_id == session->call_b || leg == 2))
    {
        do_b = 1;
        target = b_tgt;
    }

    }

    CC_SESSION_UNLOCK(session);

    if (was_update && was_reinvite)
        method = "UPDATE+REINVITE";
    else if (was_reinvite)
        method = "REINVITE";
    else if (was_update)
        method = "UPDATE";
    else
        method = "INVITE";

    if (do_a || do_b) {
        const char *leg_tag = do_a ? "A" : "B";
        const char *peer_tag = do_a ? "B" : "A";
        int initial_rtpengine = cc_rtpengine_enabled() &&
                                !was_update && !was_reinvite;

        tag = was_reinvite
              ? (do_a ? "A-REINVITE" : "B-REINVITE")
              : was_update
              ? (do_a ? "A-UPDATE"   : "B-UPDATE")
              : (do_a ? "A-INVITE"   : "B-INVITE");
        cc_rewrite_sdp_audio_endpoint(pool, sdp, &target, tag);

        /* For hold/resume propagation (either direction), also rewrite the
         * direction attribute on all audio m= sections: sendonly (hold) or
         * sendrecv (resume). This is the only SDP change needed — RTP
         * addresses stay pointed at the bypass (direct) peer endpoint. */
        if (hold_direction != 0) {
            const char *dir_str = (hold_direction == 1) ? "sendonly" : "sendrecv";
            pj_size_t mi;
            for (mi = 0; mi < sdp->media_count; mi++) {
                pjmedia_sdp_media *m = sdp->media[mi];
                unsigned ai;
                if (!m || pj_strcmp2(&m->desc.media, "audio") != 0) continue;
                /* Remove existing direction attributes */
                for (ai = 0; ai < m->attr_count; ) {
                    if (m->attr[ai] &&
                        (pj_strcmp2(&m->attr[ai]->name, "sendonly") == 0 ||
                         pj_strcmp2(&m->attr[ai]->name, "recvonly") == 0 ||
                         pj_strcmp2(&m->attr[ai]->name, "sendrecv") == 0 ||
                         pj_strcmp2(&m->attr[ai]->name, "inactive") == 0))
                    {
                        unsigned j;
                        for (j = ai; j + 1 < m->attr_count; j++)
                            m->attr[j] = m->attr[j+1];
                        m->attr_count--;
                    } else {
                        ai++;
                    }
                }
                /* Append new direction */
                if (m->attr_count < PJMEDIA_MAX_SDP_ATTR) {
                    pjmedia_sdp_attr *a = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_attr);
                    a->name  = pj_strdup3(pool, dir_str);
                    a->value = pj_str("");
                    m->attr[m->attr_count++] = a;
                }
            }
            PJ_LOG(3, (THIS_FILE,
                       "[SDP-REWRITE] %s-leg hold/resume direction=%s endpoint=%s:%d",
                       leg_tag, dir_str, target.ip, target.port));
        } else if (initial_rtpengine) {
            PJ_LOG(3, (THIS_FILE,
                       "[SDP-REWRITE] %s-leg %s SDP rewritten to RTPengine "
                       "%s-facing %s:%d",
                       leg_tag, method, do_a ? "A" : "B",
                       target.ip, target.port));
        } else {
            PJ_LOG(3, (THIS_FILE,
                       "[SDP-REWRITE] %s-leg %s SDP rewritten to %s RTP %s:%d",
                       leg_tag, method, peer_tag, target.ip, target.port));
        }
    } else if (cc_rtpengine_enabled()) {
        /* No endpoint rewrite this time — still enforce PCMA+TE on wire. */
        cc_sdp_restrict_audio_pcma_te(pool, sdp, "SDP");
        if (leg == 2) {
            PJ_LOG(1, (THIS_FILE,
                       "[SDP-REWRITE] B-leg SDP has no RTPengine B-facing "
                       "endpoint — media may stay on 127.0.0.1 (no prompt)"));
        }
    }

    /* Log codecs after restrict/rewrite (what SIP will actually send). */
    {
        int has_pcma = cc_sdp_has_rtpmap(sdp, "PCMA/8000");
        int has_pcmu = cc_sdp_has_rtpmap(sdp, "PCMU/8000");
        int has_telephone_event =
            cc_sdp_has_rtpmap(sdp, "telephone-event/8000");

        if (has_pcma && has_telephone_event && !has_pcmu) {
            PJ_LOG(3, (THIS_FILE,
                       "[SDP] call_id=%d leg=%s PCMA/8000=yes PCMU/8000=no "
                       "telephone-event/8000=yes",
                       call_id,
                       leg == 1 ? "A" : (leg == 2 ? "B" : "UNKNOWN")));
        } else {
            PJ_LOG(1, (THIS_FILE,
                       "[SDP] call_id=%d leg=%s PCMA/8000=%s PCMU/8000=%s "
                       "telephone-event/8000=%s "
                       "(want PCMA+TE only)",
                       call_id,
                       leg == 1 ? "A" : (leg == 2 ? "B" : "UNKNOWN"),
                       has_pcma ? "yes" : "no",
                       has_pcmu ? "yes" : "no",
                       has_telephone_event ? "yes" : "no"));
        }
    }

    cc_session_release_reason(session, "callback-sdp");
}
