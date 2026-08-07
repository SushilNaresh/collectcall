/*
 * worker.c — Fixed worker thread pool replacing all per-call pthreads
 *
 * CC_WORKER_POOL_SIZE (64) worker threads share a single MPSC ring queue
 * of CC_WORKER_QUEUE_SIZE (16384) slots. Each slot holds a cc_event_t.
 *
 * All blocking work that previously ran in dedicated per-call threads
 * (sleep loops, RTP polls, WAV waits, timer countdowns) now runs inside
 * a worker thread from this pool. PJSUA callbacks post events and return
 * immediately — they never block.
 *
 * Thread count: fixed 64 regardless of session count.
 * Previously: up to 11 threads × 4096 sessions = 45,056 threads.
 *
 * Events processed here (one per former pthread):
 *   CC_EV_ORIGINATE_B          — was cc_originate_b_thread
 *   CC_EV_WAV_HANGUP_A         — was wav_then_hangup_thread
 *   CC_EV_MCA_WAIT             — was mca_wait_thread
 *   CC_EV_B_PROMPT_START       — was cc_b_prompt_start_thread
 *   CC_EV_B_PROMPT_DONE        — was cc_b_prompt_done_thread
 *   CC_EV_ACCEPT_TRANSITION    — was accept_transition_thread
 *   CC_EV_RING_TIMER           — was timer_thread (is_ring=1)
 *   CC_EV_DTMF_TIMER           — was timer_thread (is_ring=0)
 *   CC_EV_UPDATE_A_RETRY       — was update_a_retry_thread
 *   CC_EV_UPDATE_B_RETRY       — was update_b_retry_thread
 *   CC_EV_UPDATE_ACK_WATCHDOG  — was update_ack_watchdog_thread
 *   CC_EV_BYPASS_RTP_WATCHDOG  — was cc_bypass_rtp_watchdog_thread
 *   CC_EV_VASYNC_CB             — async validation callback from dispatcher
 */

#include "worker.h"
#include "handlers.h"
#include "b2bua.h"
#include "utils.h"
#include "config.h"
#include "prompt_mapping.h"
#include "runtime_config.h"
#include "validation_async.h"

#include <pjsua-lib/pjsua.h>
#include <pj/log.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

#define THIS_FILE "worker.c"

/* ── MPSC ring queue ─────────────────────────────────────────────────────── */
/*
 * Multiple producers (PJSUA callback threads) write to the ring.
 * Multiple consumers (worker threads) read from it.
 * Each slot has an atomic sequence number for lock-free coordination.
 */

typedef struct {
    cc_event_t          ev;
    _Atomic unsigned    seq;   /* sequence number for this slot */
} cc_ring_slot_t;

typedef struct {
    cc_ring_slot_t      slots[CC_WORKER_QUEUE_SIZE];
    _Atomic unsigned    head;  /* next slot to write (producers) */
    _Atomic unsigned    tail;  /* next slot to read  (consumers) */
    char                _pad[64];
} cc_ring_t;

static cc_ring_t g_ring;

/* Condvar used to wake idle workers when new events are posted */
static pthread_mutex_t  g_wake_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_wake_cond  = PTHREAD_COND_INITIALIZER;

static void ring_init(void)
{
    unsigned i;
    memset(&g_ring, 0, sizeof(g_ring));
    for (i = 0; i < CC_WORKER_QUEUE_SIZE; i++)
        atomic_store_explicit(&g_ring.slots[i].seq, i, memory_order_relaxed);
    atomic_store_explicit(&g_ring.head, 0, memory_order_relaxed);
    atomic_store_explicit(&g_ring.tail, 0, memory_order_relaxed);
}

/* Returns 0 on success, -1 if queue full */
static int ring_push(const cc_event_t *ev)
{
    unsigned head, seq;
    cc_ring_slot_t *slot;

    for (;;) {
        head = atomic_load_explicit(&g_ring.head, memory_order_relaxed);
        slot = &g_ring.slots[head & (CC_WORKER_QUEUE_SIZE - 1)];
        seq  = atomic_load_explicit(&slot->seq, memory_order_acquire);

        if (seq == head) {
            if (atomic_compare_exchange_weak_explicit(
                    &g_ring.head, &head, head + 1,
                    memory_order_relaxed, memory_order_relaxed))
            {
                slot->ev = *ev;
                atomic_store_explicit(&slot->seq, head + 1,
                                      memory_order_release);
                /* Wake one idle worker */
                pthread_mutex_lock(&g_wake_mutex);
                pthread_cond_signal(&g_wake_cond);
                pthread_mutex_unlock(&g_wake_mutex);
                return 0;
            }
        } else if ((int)(seq - head) < 0) {
            return -1; /* queue full */
        }
        /* another producer won the CAS — retry */
    }
}

/* Returns 1 if an event was popped, 0 if queue empty */
static int ring_pop(cc_event_t *ev)
{
    unsigned tail, seq;
    cc_ring_slot_t *slot;

    for (;;) {
        tail = atomic_load_explicit(&g_ring.tail, memory_order_relaxed);
        slot = &g_ring.slots[tail & (CC_WORKER_QUEUE_SIZE - 1)];
        seq  = atomic_load_explicit(&slot->seq, memory_order_acquire);

        if (seq == tail + 1) {
            if (atomic_compare_exchange_weak_explicit(
                    &g_ring.tail, &tail, tail + 1,
                    memory_order_relaxed, memory_order_relaxed))
            {
                *ev = slot->ev;
                atomic_store_explicit(&slot->seq,
                                      tail + CC_WORKER_QUEUE_SIZE,
                                      memory_order_release);
                return 1;
            }
        } else if (seq == tail) {
            return 0; /* queue empty */
        }
        /* another consumer won the CAS — retry */
    }
}

/* ── Worker pool ─────────────────────────────────────────────────────────── */

static pthread_t        g_workers[CC_WORKER_POOL_SIZE];
static volatile int     g_running = 0;

/* forward declaration */
static void process_event(cc_event_t *ev);

static void *worker_thread(void *arg)
{
    pj_thread_desc  desc;
    pj_thread_t    *pj_thread = NULL;
    char            name[32];
    int             idx = (int)(intptr_t)arg;

    snprintf(name, sizeof(name), "cc_worker_%d", idx);
    pj_bzero(desc, sizeof(desc));
    if (pj_thread_register(name, desc, &pj_thread) != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE, "[WORKER] pj_thread_register failed idx=%d", idx));
        return NULL;
    }

    PJ_LOG(3, (THIS_FILE, "[WORKER] thread %d started", idx));

    while (g_running) {
        cc_event_t ev;
        if (ring_pop(&ev)) {
            process_event(&ev);
        } else {
            /* Sleep until signalled by ring_push or shutdown */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;  /* 1s timeout — safety net for missed signals */
            pthread_mutex_lock(&g_wake_mutex);
            if (!ring_pop(&ev)) {
                pthread_cond_timedwait(&g_wake_cond, &g_wake_mutex, &ts);
                pthread_mutex_unlock(&g_wake_mutex);
            } else {
                pthread_mutex_unlock(&g_wake_mutex);
                process_event(&ev);
            }
        }
    }

    /* drain remaining events on shutdown */
    {
        cc_event_t ev;
        while (ring_pop(&ev))
            process_event(&ev);
    }

    PJ_LOG(3, (THIS_FILE, "[WORKER] thread %d stopped", idx));
    return NULL;
}

/* ── Timer queue (min-heap) ──────────────────────────────────────────────── */
/*
 * One timer thread sleeps until the nearest deadline using
 * pthread_cond_timedwait, then posts the event to the worker pool.
 * O(log n) insert/remove. Zero per-call threads.
 */

#define CC_TIMER_HEAP_MAX  4096

typedef struct {
    long long   fire_at_ms;   /* CLOCK_MONOTONIC deadline */
    cc_event_t  ev;
} cc_timer_entry_t;

static cc_timer_entry_t  g_timer_heap[CC_TIMER_HEAP_MAX];
static int               g_timer_count = 0;
static pthread_mutex_t   g_timer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    g_timer_cond  = PTHREAD_COND_INITIALIZER;
static pthread_t         g_timer_thread;

static long long timer_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Min-heap: parent <= children by fire_at_ms */
static void timer_heap_up(int i)
{
    while (i > 0) {
        int p = (i - 1) / 2;
        if (g_timer_heap[p].fire_at_ms <= g_timer_heap[i].fire_at_ms) break;
        cc_timer_entry_t tmp = g_timer_heap[p];
        g_timer_heap[p] = g_timer_heap[i];
        g_timer_heap[i] = tmp;
        i = p;
    }
}

static void timer_heap_down(int i)
{
    for (;;) {
        int l = 2*i+1, r = 2*i+2, m = i;
        if (l < g_timer_count &&
            g_timer_heap[l].fire_at_ms < g_timer_heap[m].fire_at_ms) m = l;
        if (r < g_timer_count &&
            g_timer_heap[r].fire_at_ms < g_timer_heap[m].fire_at_ms) m = r;
        if (m == i) break;
        cc_timer_entry_t tmp = g_timer_heap[m];
        g_timer_heap[m] = g_timer_heap[i];
        g_timer_heap[i] = tmp;
        i = m;
    }
}

static void *timer_thread_fn(void *arg)
{
    (void)arg;

    /* Register with PJLIB so any future PJSIP calls from this thread work */
    pj_thread_desc desc;
    pj_thread_t   *pj_thr = NULL;
    pj_bzero(desc, sizeof(desc));
    pj_thread_register("cc_timer", desc, &pj_thr);

    pthread_mutex_lock(&g_timer_mutex);
    while (g_running) {
        if (g_timer_count == 0) {
            /* Nothing pending — wait indefinitely for a signal */
            pthread_cond_wait(&g_timer_cond, &g_timer_mutex);
            continue;
        }

        long long now  = timer_now_ms();
        long long fire = g_timer_heap[0].fire_at_ms;

        if (now < fire) {
            /* Sleep until nearest deadline */
            struct timespec abs;
            clock_gettime(CLOCK_REALTIME, &abs);
            long long wait = fire - now;
            abs.tv_sec  += wait / 1000;
            abs.tv_nsec += (wait % 1000) * 1000000L;
            if (abs.tv_nsec >= 1000000000L) {
                abs.tv_sec++;
                abs.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&g_timer_cond, &g_timer_mutex, &abs);
            continue;
        }

        /* Fire: pop the min entry */
        cc_event_t ev = g_timer_heap[0].ev;
        g_timer_heap[0] = g_timer_heap[--g_timer_count];
        timer_heap_down(0);
        pthread_mutex_unlock(&g_timer_mutex);

        /* For CC_EV_WAV_HANGUP_A: stop the waiting-prompt player immediately
         * to prevent EOF spam while the event waits in the worker queue.
         * For CC_EV_HANGUP_A_ONLY: player_a is the treatment player —
         * leave it running, ev_hangup_a_only stops it after the delay. */
        if (ev.type == CC_EV_WAV_HANGUP_A && ev.player_a != PJSUA_INVALID_ID) {
            PJ_LOG(3, (THIS_FILE, "[TIMER] Stop A waiting prompt before treatment"));
            cc_stop_wav(ev.player_a, PJSUA_INVALID_ID);
            ev.player_a = PJSUA_INVALID_ID;
        }

        /* Post to worker pool — cc_worker_post acquires its own ref.
         * On success: release the timer's ref (worker owns it now).
         * On failure: release the timer's ref and clean up. */
        int posted = (cc_worker_post(&ev) == 0);
        if (!posted && ev.session) {
            CC_SESSION_LOCK(ev.session);
            ev.session->a_treatment_running = 0;
            CC_SESSION_UNLOCK(ev.session);
        }
        if (ev.session) {
            cc_session_t    *s    = ev.session;
            pthread_mutex_t *lock = s->lock;   /* capture before any release can free s */
            unsigned refs;
            int destroy;

            cc_session_acquire_reason(s, "timer-fire-guard");
            cc_session_maybe_finalize(s);

            /* Release both ev.reason and timer-fire-guard inside one locked
             * block — same reasoning as process_event: cc_session_release_reason
             * for ev.reason could call cc_session_destroy and free s, making
             * any subsequent read of s->ref_count a UAF. */
            pthread_mutex_lock(lock);
            refs = s->ref_count;
            if (refs > 0) { refs--; s->ref_count = refs; }  /* ev.reason */
            if (refs > 0) { refs--; s->ref_count = refs; }  /* timer-fire-guard */
            destroy = (refs == 0);
            pthread_mutex_unlock(lock);

            PJ_LOG(4, ("session",
                       "[SESSION] release reason=%s session=%p refs=%u",
                       ev.reason, s, refs > 0 ? refs + 1 : 0));
            PJ_LOG(4, ("session",
                       "[SESSION] release reason=timer-fire-guard session=%p refs=%u",
                       s, refs));
            if (destroy)
                cc_session_destroy(s);
        }

        pthread_mutex_lock(&g_timer_mutex);
    }
    pthread_mutex_unlock(&g_timer_mutex);
    return NULL;
}

int cc_worker_post_delayed(cc_event_t *ev, int wait_ms)
{
    if (!ev || wait_ms <= 0)
        return cc_worker_post(ev);

    if (ev->reason[0] == '\0')
        snprintf(ev->reason, sizeof(ev->reason), "worker-ev-%d", ev->type);

    /* Acquire ref for the timer entry */
    if (ev->session && !cc_session_acquire_reason(ev->session, ev->reason))
        return -1;

    pthread_mutex_lock(&g_timer_mutex);
    if (g_timer_count >= CC_TIMER_HEAP_MAX) {
        pthread_mutex_unlock(&g_timer_mutex);
        if (ev->session)
            cc_session_release_reason(ev->session, ev->reason);
        PJ_LOG(1, (THIS_FILE, "[TIMER] heap full — delayed event dropped"));
        return -1;
    }
    g_timer_heap[g_timer_count].fire_at_ms = timer_now_ms() + wait_ms;
    g_timer_heap[g_timer_count].ev         = *ev;
    timer_heap_up(g_timer_count++);
    pthread_cond_signal(&g_timer_cond);
    pthread_mutex_unlock(&g_timer_mutex);
    return 0;
}
int cc_worker_start(void)
{
    int i;
    pthread_attr_t attr;

    ring_init();
    g_running = 1;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024); /* 256 KB per worker */

    for (i = 0; i < CC_WORKER_POOL_SIZE; i++) {
        if (pthread_create(&g_workers[i], &attr,
                           worker_thread, (void *)(intptr_t)i) != 0)
        {
            PJ_LOG(1, (THIS_FILE,
                       "[WORKER] failed to create worker thread %d", i));
            pthread_attr_destroy(&attr);
            return -1;
        }
    }

    pthread_attr_destroy(&attr);
    PJ_LOG(3, (THIS_FILE,
               "[WORKER] pool started: %d threads, queue=%d slots",
               CC_WORKER_POOL_SIZE, CC_WORKER_QUEUE_SIZE));

    if (pthread_create(&g_timer_thread, NULL, timer_thread_fn, NULL) != 0) {
        PJ_LOG(1, (THIS_FILE, "[WORKER] failed to create timer thread"));
        return -1;
    }

    return 0;
}

void cc_worker_stop(void)
{
    int i;
    g_running = 0;
    pthread_cond_signal(&g_timer_cond);   /* wake timer thread to exit */
    pthread_join(g_timer_thread, NULL);
    /* Wake all workers so they see g_running=0 and exit */
    pthread_mutex_lock(&g_wake_mutex);
    pthread_cond_broadcast(&g_wake_cond);
    pthread_mutex_unlock(&g_wake_mutex);
    for (i = 0; i < CC_WORKER_POOL_SIZE; i++)
        pthread_join(g_workers[i], NULL);
    PJ_LOG(3, (THIS_FILE, "[WORKER] pool stopped"));
}

/* ── Public post API ─────────────────────────────────────────────────────── */

int cc_worker_post(cc_event_t *ev)
{
    if (!ev)
        return -1;

    if (ev->reason[0] == '\0')
        snprintf(ev->reason, sizeof(ev->reason), "worker-ev-%d", ev->type);

    if (ev->session) {
        if (!cc_session_acquire_reason(ev->session, ev->reason))
            return -1;
    }

    if (ring_push(ev) != 0) {
        if (ev->session)
            cc_session_release_reason(ev->session, ev->reason);
        PJ_LOG(1, (THIS_FILE,
                   "[WORKER] queue full — event type=%d dropped", ev->type));
        return -1;
    }

    return 0;
}

/* ── Helper: WAV player duration ─────────────────────────────────────────── */

static int worker_player_duration_ms(pjsua_player_id pid)
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

/* ── Event handlers ──────────────────────────────────────────────────────── */

/* CC_EV_ORIGINATE_B — was cc_originate_b_thread */
static void ev_originate_b(cc_event_t *ev)
{
    cc_session_t       *session = ev->session;
    cc_originate_arg_t *arg;

    CC_SESSION_LOCK(session);
    arg = (cc_originate_arg_t *)session->originate_arg;
    session->originate_arg = NULL;
    CC_SESSION_UNLOCK(session);

    if (!arg) {
        PJ_LOG(1, (THIS_FILE, "[WORKER] ev_originate_b: arg is NULL"));
        CC_SESSION_LOCK(session);
        session->b_origination_pending = 0;
        CC_SESSION_UNLOCK(session);
        return;
    }

    /* Run the origination logic directly (same body as cc_originate_b_thread) */
    cc_originate_b_thread(arg);
}


/* CC_EV_WAV_HANGUP_A — was wav_then_hangup_thread */
static void ev_wav_hangup_a(cc_event_t *ev)
{
    cc_session_t      *s       = ev->session;
    const char        *wav_path = ev->wav_path;
    pjsip_status_code  code    = (pjsip_status_code)ev->sip_code;
    pjsua_call_id      call_a  = ev->call_a;

    /* Step 1: stop the waiting-prompt player immediately — prevents EOF spam. */
    if (ev->player_a != PJSUA_INVALID_ID) {
        PJ_LOG(3, (THIS_FILE, "[VOICE] Stop A waiting prompt before treatment"));
        cc_stop_wav(ev->player_a, PJSUA_INVALID_ID);
        ev->player_a = PJSUA_INVALID_ID;
    }

    /* Step 2: start treatment WAV, then post a delayed CC_EV_HANGUP_A_ONLY
     * so this worker is free immediately — no sleeping for wav duration. */
    if (cc_session_call_is_current(s, call_a, 1)) {
        pjsua_player_id pid = cc_start_wav(call_a, wav_path, PJ_FALSE);
        int wav_ms = worker_player_duration_ms(pid);
        cc_event_t hev;
        memset(&hev, 0, sizeof(hev));
        hev.type     = CC_EV_HANGUP_A_ONLY;
        hev.session  = s;
        hev.call_a   = call_a;
        hev.sip_code = (int)code;
        hev.player_a = pid;   /* treatment player — stopped in HANGUP_A_ONLY */
        snprintf(hev.reason, sizeof(hev.reason), "hangup-a-only");
        if (cc_worker_post_delayed(&hev, wav_ms) != 0) {
            /* fallback: stop player and hang up inline */
            cc_stop_wav(pid, PJSUA_INVALID_ID);
            if (cc_session_call_is_current(s, call_a, 1))
                cc_safe_hangup(call_a, code);
        }
        /* a_treatment_running stays set — HANGUP_A_ONLY clears it via process_event */
        return;
    }
    /* a_treatment_running cleared by process_event after maybe_finalize */
}

/* CC_EV_HANGUP_A_ONLY — fires after treatment WAV duration expires */
static void ev_hangup_a_only(cc_event_t *ev)
{
    cc_session_t      *s      = ev->session;
    pjsua_call_id      call_a = ev->call_a;
    pjsip_status_code  code   = (pjsip_status_code)ev->sip_code;

    /* Stop the treatment player */
    if (ev->player_a != PJSUA_INVALID_ID) {
        cc_stop_wav(ev->player_a, PJSUA_INVALID_ID);
        ev->player_a = PJSUA_INVALID_ID;
    }
    if (cc_session_call_is_current(s, call_a, 1))
        cc_safe_hangup(call_a, code);
    /* a_treatment_running cleared by process_event after maybe_finalize */
}

/* CC_EV_MCA_WAIT — was mca_wait_thread */
static void ev_mca_wait(cc_event_t *ev)
{
    cc_session_t   *s         = ev->session;
    pjsua_call_id   call_a    = ev->call_a;
    cc_prompt_tag_t prompt_tag = (cc_prompt_tag_t)ev->prompt_tag;
    int remaining_ms, decided;

    if (!cc_session_call_is_current(s, call_a, 1)) {
        CC_SESSION_LOCK(s);
        s->mca_waiting = 0;
        CC_SESSION_UNLOCK(s);
        /* a_treatment_running cleared by process_event after maybe_finalize */
        return;
    }

    /* Play prompt loop to A while waiting for DTMF */
    {
        const char *path = cc_prompt_get_path(prompt_tag);
        pjsua_player_id pid = cc_start_wav(call_a, path, PJ_FALSE);
        if (pid != PJSUA_INVALID_ID) {
            int stored = 0;
            CC_SESSION_LOCK(s);
            if (s->call_a == call_a && s->player_a == PJSUA_INVALID_ID &&
                !s->torn_down)
            {
                s->player_a = pid;
                stored = 1;
            }
            CC_SESSION_UNLOCK(s);
            if (!stored)
                cc_stop_wav(pid, PJSUA_INVALID_ID);
        }
    }

    remaining_ms = cc_cfg_b_dtmf_timeout_sec() * 1000;
    decided = 0;
    while (remaining_ms > 0) {
        int slice = remaining_ms > 100 ? 100 : remaining_ms;
        cc_sleep_ms(slice);
        remaining_ms -= slice;
        CC_SESSION_LOCK(s);
        decided = s->mca_decided;
        CC_SESSION_UNLOCK(s);
        if (decided) break;
        if (!cc_session_call_is_current(s, call_a, 1)) {
            decided = -1;
            break;
        }
    }

    /* Stop looping prompt */
    {
        pjsua_player_id pid = PJSUA_INVALID_ID;
        CC_SESSION_LOCK(s);
        if (s->player_a != PJSUA_INVALID_ID) {
            pid = s->player_a;
            s->player_a = PJSUA_INVALID_ID;
        }
        s->mca_waiting = 0;
        CC_SESSION_UNLOCK(s);
        if (pid != PJSUA_INVALID_ID)
            cc_stop_wav(pid, PJSUA_INVALID_ID);
    }

    if (decided == 1) {
        cc_session_mark_end(s, "FAILED", "SPONSOR_UNREACHABLE_MCA");
        if (cc_session_call_is_current(s, call_a, 1)) {
            const char *path = cc_prompt_get_path(CC_PROMPT_MCA_SENT);
            pjsua_player_id pid = cc_start_wav(call_a, path, PJ_FALSE);
            int w = worker_player_duration_ms(pid);
            int cap = cc_cfg_free_period_ms();
            int sleep_ms = w > cap ? cap : w;
            /* Store in session so disconnect handler can clean up if call ends
             * during the sleep — avoids leaking the player slot. */
            int stored = 0;
            CC_SESSION_LOCK(s);
            if (s->player_a == PJSUA_INVALID_ID && pid != PJSUA_INVALID_ID)
            { s->player_a = pid; stored = 1; }
            CC_SESSION_UNLOCK(s);
            if (!stored && pid != PJSUA_INVALID_ID)
                cc_stop_wav(pid, PJSUA_INVALID_ID);
            else {
                cc_sleep_ms(sleep_ms);
                pjsua_player_id take = PJSUA_INVALID_ID;
                CC_SESSION_LOCK(s);
                if (s->player_a == pid) { take = pid; s->player_a = PJSUA_INVALID_ID; }
                CC_SESSION_UNLOCK(s);
                if (take != PJSUA_INVALID_ID)
                    cc_stop_wav(take, PJSUA_INVALID_ID);
            }
            if (cc_session_call_is_current(s, call_a, 1))
                cc_safe_hangup(call_a, PJSIP_SC_OK);
        }
    } else if (decided == 2) {
        cc_session_mark_end(s, "FAILED", "SPONSOR_UNREACHABLE_NoMCA");
        if (cc_session_call_is_current(s, call_a, 1)) {
            const char *path = cc_prompt_get_path(CC_PROMPT_MCA_NOT_SENT);
            pjsua_player_id pid = cc_start_wav(call_a, path, PJ_FALSE);
            int w = worker_player_duration_ms(pid);
            int cap = cc_cfg_free_period_ms();
            int sleep_ms = w > cap ? cap : w;
            int stored = 0;
            CC_SESSION_LOCK(s);
            if (s->player_a == PJSUA_INVALID_ID && pid != PJSUA_INVALID_ID)
            { s->player_a = pid; stored = 1; }
            CC_SESSION_UNLOCK(s);
            if (!stored && pid != PJSUA_INVALID_ID)
                cc_stop_wav(pid, PJSUA_INVALID_ID);
            else {
                cc_sleep_ms(sleep_ms);
                pjsua_player_id take = PJSUA_INVALID_ID;
                CC_SESSION_LOCK(s);
                if (s->player_a == pid) { take = pid; s->player_a = PJSUA_INVALID_ID; }
                CC_SESSION_UNLOCK(s);
                if (take != PJSUA_INVALID_ID)
                    cc_stop_wav(take, PJSUA_INVALID_ID);
            }
            if (cc_session_call_is_current(s, call_a, 1))
                cc_safe_hangup(call_a, PJSIP_SC_OK);
        }
    } else if (decided == 0) {
        cc_session_mark_end(s, "FAILED", "SPONSOR_UNREACHABLE_NoMCA");
        if (cc_session_call_is_current(s, call_a, 1))
            cc_safe_hangup(call_a, PJSIP_SC_TEMPORARILY_UNAVAILABLE);
    } else {
        cc_session_mark_end(s, "FAILED", "SPONSOR_UNREACHABLE_NoMCA");
    }

    /* a_treatment_running cleared by process_event after maybe_finalize */
}

/* CC_EV_B_PROMPT_START — was cc_b_prompt_start_thread */
static void ev_b_prompt_start(cc_event_t *ev)
{
    cc_session_t  *session = ev->session;
    pjsua_call_id  call_id = ev->call_b;
    pjsua_player_id pid;
    int keep_player = 0;
    const char *path;

    /* Poll until CONFIRMED (max 30s) */
    {
        int wait_ms = 0, aborted = 0;
        while (wait_ms < 30000) {
            pjsua_call_info ci;
            if (pjsua_call_get_info(call_id, &ci) == PJ_SUCCESS &&
                ci.state == PJSIP_INV_STATE_CONFIRMED)
                break;
            cc_sleep_ms(100);
            wait_ms += 100;
            CC_SESSION_LOCK(session);
            aborted = session->accepted || session->torn_down ||
                      session->call_b != call_id;
            CC_SESSION_UNLOCK(session);
            if (aborted) goto bps_done;
        }
        CC_SESSION_LOCK(session);
        aborted = session->accepted || session->torn_down ||
                  session->call_b != call_id;
        CC_SESSION_UNLOCK(session);
        if (aborted) goto bps_done;
    }

    /* Poll until B RTP ready (max 3s) */
    {
        cc_rtp_ep_t rtp;
        int rtp_ms = 0, aborted = 0;
        while (rtp_ms < 3000) {
            if (cc_get_call_remote_rtp(call_id, &rtp) == PJ_SUCCESS &&
                rtp.port != 0)
                break;
            cc_sleep_ms(100);
            rtp_ms += 100;
            CC_SESSION_LOCK(session);
            aborted = session->accepted || session->torn_down ||
                      session->call_b != call_id;
            CC_SESSION_UNLOCK(session);
            if (aborted) goto bps_done;
        }
    }

    path = cc_prompt_get_path(CC_PROMPT_COLLECT_PROMPT);
    CC_SESSION_LOCK(session);
    if (session->b_prompt_start_ts == 0)
        session->b_prompt_start_ts = time(NULL);
    CC_SESSION_UNLOCK(session);

    pid = cc_start_wav(call_id, path, PJ_FALSE);

    CC_SESSION_LOCK(session);
    if (pid != PJSUA_INVALID_ID && !session->accepted &&
        !session->torn_down && session->call_b == call_id &&
        session->player_b == PJSUA_INVALID_ID)
    {
        session->player_b = pid;
        keep_player = 1;
    }
    session->b_prompt_starting = 0;
    CC_SESSION_UNLOCK(session);

    if (keep_player) {
        int prompt_ms = worker_player_duration_ms(pid);
        CC_SESSION_LOCK(session);
        session->b_collect_done = 0;
        CC_SESSION_UNLOCK(session);

        /* Post b_prompt_done event */
        {
            cc_event_t done_ev;
            memset(&done_ev, 0, sizeof(done_ev));
            done_ev.type      = CC_EV_B_PROMPT_DONE;
            done_ev.session   = session;
            done_ev.call_b    = call_id;
            done_ev.prompt_ms = prompt_ms;
            snprintf(done_ev.reason, sizeof(done_ev.reason), "b-prompt-done");
            if (cc_worker_post(&done_ev) != 0) {
                CC_SESSION_LOCK(session);
                session->b_collect_done = 1;
                CC_SESSION_UNLOCK(session);
            }
        }
        leg_b_start_dtmf_timer(session);
    } else if (pid != PJSUA_INVALID_ID) {
        cc_stop_wav(pid, PJSUA_INVALID_ID);
    }

    return;

bps_done:
    CC_SESSION_LOCK(session);
    session->b_prompt_starting = 0;
    CC_SESSION_UNLOCK(session);
}

/* CC_EV_B_PROMPT_DONE — was cc_b_prompt_done_thread */
static void ev_b_prompt_done(cc_event_t *ev)
{
    cc_session_t *s       = ev->session;
    int           wait_ms = ev->prompt_ms;
    int           remaining = wait_ms;
    int           rtp_checked = 0;

    while (remaining > 0) {
        int slice = remaining > 100 ? 100 : remaining;
        cc_sleep_ms(slice);
        remaining -= slice;

        if (!rtp_checked && (wait_ms - remaining) >= 1000) {
            rtp_checked = 1;
            pjsua_call_id cid;
            CC_SESSION_LOCK(s);
            cid = s->call_b;
            CC_SESSION_UNLOCK(s);
            if (cid != PJSUA_INVALID_ID)
                cc_log_call_rtp_info(cid, "B-rtp-1s");
        }

        int done;
        CC_SESSION_LOCK(s);
        done = s->torn_down || s->final_cleanup_started;
        CC_SESSION_UNLOCK(s);
        if (done) {
            CC_SESSION_LOCK(s);
            s->b_collect_done = 1;
            CC_SESSION_UNLOCK(s);
            return;
        }
    }

    CC_SESSION_LOCK(s);
    s->b_collect_done = 1;
    CC_SESSION_UNLOCK(s);
    PJ_LOG(3, (THIS_FILE, "[WORKER] B collect prompt finished"));
}

/* ── Accept transition — 3-phase continuation, zero worker sleep ─────────── *
 *
 * Phase 1 (CC_EV_ACCEPT_TRANSITION):
 *   - If b_collect_done not yet set AND free period not expired, re-arm via
 *     cc_worker_post_delayed(50ms) and return immediately.
 *   - Once b_collect_done or free-period expired: stop B collect prompt,
 *     stop A waiting prompt, start 4.1.wav on B (whitelisted) or dial-tone
 *     on both legs (non-whitelisted), then post CC_EV_ACCEPT_BRIDGE_WAIT
 *     delayed by the WAV/tone duration.
 *
 * Phase 2 (CC_EV_ACCEPT_BRIDGE_WAIT):
 *   - Recompute remaining free period from wall clock.
 *   - If still remaining, post CC_EV_ACCEPT_BRIDGE delayed by that amount.
 *   - Otherwise post CC_EV_ACCEPT_BRIDGE immediately.
 *
 * Phase 3 (CC_EV_ACCEPT_BRIDGE):
 *   - Stop any lingering players, set call_connected_ts, bridge/UPDATE.
 *   - Clear accept_transition_pending.
 */

/* Shared stale-check: returns 1 if session is no longer valid for transition */
static int accept_is_stale(cc_session_t *s,
                            pjsua_call_id call_a,
                            pjsua_call_id call_b)
{
    int stale;
    CC_SESSION_LOCK(s);
    stale = s->torn_down || !s->accepted ||
            s->call_a != call_a || s->call_b != call_b;
    CC_SESSION_UNLOCK(s);
    return stale;
}

/* Post next phase; on failure clear accept_transition_pending */
static void post_accept_phase(cc_session_t *s,
                               cc_ev_type_t type,
                               pjsua_call_id call_a,
                               pjsua_call_id call_b,
                               int delay_ms,
                               pjsua_player_id player_phase)
{
    cc_event_t nev;
    memset(&nev, 0, sizeof(nev));
    nev.type     = type;
    nev.session  = s;
    nev.call_a   = call_a;
    nev.call_b   = call_b;
    nev.delay_ms = delay_ms;
    nev.player_a = player_phase;  /* reused to carry phase player across events */
    snprintf(nev.reason, sizeof(nev.reason), "accept-transition-worker");

    int rc = (delay_ms > 0)
             ? cc_worker_post_delayed(&nev, delay_ms)
             : cc_worker_post(&nev);

    if (rc != 0) {
        PJ_LOG(1, (THIS_FILE, "[ACCEPT] phase post failed type=%d — clearing pending", type));
        CC_SESSION_LOCK(s);
        s->accept_transition_pending = 0;
        CC_SESSION_UNLOCK(s);
        cc_session_maybe_finalize(s);
    }
}

/* Phase 1 */
static void ev_accept_transition(cc_event_t *ev)
{
    cc_session_t  *s      = ev->session;
    pjsua_call_id  call_a = ev->call_a;
    pjsua_call_id  call_b = ev->call_b;

    if (accept_is_stale(s, call_a, call_b)) {
        PJ_LOG(3, (THIS_FILE, "[ACCEPT] phase1 stale — abort"));
        CC_SESSION_LOCK(s);
        s->accept_transition_pending = 0;
        CC_SESSION_UNLOCK(s);
        return;
    }

    /* Check if b_collect_done or free period expired */
    {
        int collect_done;
        long long b_confirmed_ms;
        int free_period;
        CC_SESSION_LOCK(s);
        collect_done   = s->b_collect_done || s->torn_down;
        b_confirmed_ms = s->b_confirmed_ms;
        free_period    = s->free_period_ms > 0
                         ? s->free_period_ms : cc_cfg_free_period_ms();
        CC_SESSION_UNLOCK(s);

        if (!collect_done && b_confirmed_ms > 0) {
            long long now_ms = timer_now_ms();
            if ((now_ms - b_confirmed_ms) >= free_period) {
                /* Free period expired — force-stop collect prompt */
                pjsua_player_id pb = PJSUA_INVALID_ID;
                CC_SESSION_LOCK(s);
                if (s->player_b != PJSUA_INVALID_ID) {
                    pb = s->player_b;
                    s->player_b = PJSUA_INVALID_ID;
                }
                s->b_collect_done = 1;
                CC_SESSION_UNLOCK(s);
                if (pb != PJSUA_INVALID_ID)
                    cc_stop_wav(pb, PJSUA_INVALID_ID);
                collect_done = 1;
            }
        }

        if (!collect_done) {
            /* Not ready yet — re-check in 50ms, return worker immediately */
            post_accept_phase(s, CC_EV_ACCEPT_TRANSITION,
                              call_a, call_b, 50, PJSUA_INVALID_ID);
            return;
        }
    }

    /* b_collect_done: stop B collect prompt if still running */
    {
        pjsua_player_id pb = PJSUA_INVALID_ID;
        CC_SESSION_LOCK(s);
        if (s->player_b != PJSUA_INVALID_ID) {
            pb = s->player_b;
            s->player_b = PJSUA_INVALID_ID;
        }
        CC_SESSION_UNLOCK(s);
        if (pb != PJSUA_INVALID_ID)
            cc_stop_wav(pb, PJSUA_INVALID_ID);
    }

    if (accept_is_stale(s, call_a, call_b)) {
        PJ_LOG(3, (THIS_FILE, "[ACCEPT] phase1 stale after collect-done — abort"));
        CC_SESSION_LOCK(s);
        s->accept_transition_pending = 0;
        CC_SESSION_UNLOCK(s);
        return;
    }

    PJ_LOG(3, (THIS_FILE, "[B] ACCEPTED — charging/communication starts now"));

    /* Start phase WAV and post phase 2 delayed by its duration */
    {
        int whitelisted;
        CC_SESSION_LOCK(s);
        whitelisted = s->whitelisted;
        CC_SESSION_UNLOCK(s);

        if (whitelisted) {
            /* Play 4.1.wav to B; A keeps 4.wav */
            pjsua_player_id b_conn_pid = PJSUA_INVALID_ID;
            const char *b_conn_path = cc_prompt_get_path(CC_PROMPT_B_CONNECTED);
            if (cc_session_call_is_current(s, call_b, 0))
                b_conn_pid = cc_start_wav(call_b, b_conn_path, PJ_FALSE);
            int b_conn_ms = worker_player_duration_ms(b_conn_pid);
            PJ_LOG(3, (THIS_FILE,
                       "[WHITELIST] Playing 4.1.wav to B: %s (%dms); A keeps 4.wav",
                       b_conn_path, b_conn_ms));
            /* Phase 2 fires after 4.1.wav finishes; carry player in player_a field.
             * If post fails, stop the player here to avoid a leaked port. */
            cc_event_t nev;
            memset(&nev, 0, sizeof(nev));
            nev.type     = CC_EV_ACCEPT_BRIDGE_WAIT;
            nev.session  = s;
            nev.call_a   = call_a;
            nev.call_b   = call_b;
            nev.delay_ms = b_conn_ms;
            nev.player_a = b_conn_pid;
            snprintf(nev.reason, sizeof(nev.reason), "accept-transition-worker");
            int rc = (b_conn_ms > 0)
                     ? cc_worker_post_delayed(&nev, b_conn_ms)
                     : cc_worker_post(&nev);
            if (rc != 0) {
                PJ_LOG(1, (THIS_FILE, "[ACCEPT] phase2 post failed — stopping 4.1.wav and clearing pending"));
                if (b_conn_pid != PJSUA_INVALID_ID)
                    cc_stop_wav(b_conn_pid, PJSUA_INVALID_ID);
                CC_SESSION_LOCK(s);
                s->accept_transition_pending = 0;
                CC_SESSION_UNLOCK(s);
                cc_session_maybe_finalize(s);
            }
        } else {
            /* Non-whitelisted: stop A prompt now, play dial tone for free period */
            pjsua_player_id pa = PJSUA_INVALID_ID;
            CC_SESSION_LOCK(s);
            if (s->player_a != PJSUA_INVALID_ID) {
                pa = s->player_a;
                s->player_a = PJSUA_INVALID_ID;
            }
            CC_SESSION_UNLOCK(s);
            if (pa != PJSUA_INVALID_ID)
                cc_stop_wav(pa, PJSUA_INVALID_ID);

            /* Compute remaining free period */
            int fp_remaining = 0;
            {
                long long b_confirmed_ms;
                int free_period;
                CC_SESSION_LOCK(s);
                b_confirmed_ms = s->b_confirmed_ms;
                free_period    = s->free_period_ms > 0
                                 ? s->free_period_ms : cc_cfg_free_period_ms();
                CC_SESSION_UNLOCK(s);
                if (b_confirmed_ms > 0) {
                    long long elapsed = timer_now_ms() - b_confirmed_ms;
                    long long rem = (long long)free_period - elapsed;
                    fp_remaining = rem > 0 ? (int)rem : 0;
                }
            }

            if (fp_remaining > 0) {
                const char *tone = cc_prompt_get_path(CC_PROMPT_DIAL_TONE);
                pjsua_player_id ta = PJSUA_INVALID_ID, tb = PJSUA_INVALID_ID;
                if (cc_session_call_is_current(s, call_a, 1))
                    ta = cc_start_wav(call_a, tone, PJ_TRUE);
                if (cc_session_call_is_current(s, call_b, 0))
                    tb = cc_start_wav(call_b, tone, PJ_TRUE);
                PJ_LOG(3, (THIS_FILE,
                           "[FREE-PERIOD] waiting %dms before bridge; playing dial tone",
                           fp_remaining));
                /* Store tone players in session so phase 3 can stop them */
                CC_SESSION_LOCK(s);
                if (ta != PJSUA_INVALID_ID && s->player_a == PJSUA_INVALID_ID)
                    s->player_a = ta;
                else if (ta != PJSUA_INVALID_ID)
                    cc_stop_wav(ta, PJSUA_INVALID_ID);
                if (tb != PJSUA_INVALID_ID && s->player_b == PJSUA_INVALID_ID)
                    s->player_b = tb;
                else if (tb != PJSUA_INVALID_ID)
                    cc_stop_wav(tb, PJSUA_INVALID_ID);
                CC_SESSION_UNLOCK(s);
                post_accept_phase(s, CC_EV_ACCEPT_BRIDGE,
                                  call_a, call_b, fp_remaining, PJSUA_INVALID_ID);
            } else {
                PJ_LOG(3, (THIS_FILE,
                           "[FREE-PERIOD] already elapsed; bridging immediately"));
                post_accept_phase(s, CC_EV_ACCEPT_BRIDGE,
                                  call_a, call_b, 0, PJSUA_INVALID_ID);
            }
        }
    }
}

/* Phase 2 — after 4.1.wav finished (whitelisted only) */
static void ev_accept_bridge_wait(cc_event_t *ev)
{
    cc_session_t  *s        = ev->session;
    pjsua_call_id  call_a   = ev->call_a;
    pjsua_call_id  call_b   = ev->call_b;
    pjsua_player_id b_conn_pid = (pjsua_player_id)ev->player_a;

    /* Stop 4.1.wav player */
    if (b_conn_pid != PJSUA_INVALID_ID)
        cc_stop_wav(b_conn_pid, PJSUA_INVALID_ID);

    if (accept_is_stale(s, call_a, call_b)) {
        PJ_LOG(3, (THIS_FILE, "[ACCEPT] phase2 stale after 4.1.wav — abort"));
        CC_SESSION_LOCK(s);
        s->accept_transition_pending = 0;
        CC_SESSION_UNLOCK(s);
        return;
    }

    /* Recompute remaining free period from wall clock */
    int fp_remaining = 0;
    {
        long long b_confirmed_ms;
        int free_period;
        CC_SESSION_LOCK(s);
        b_confirmed_ms = s->b_confirmed_ms;
        free_period    = s->free_period_ms > 0
                         ? s->free_period_ms : cc_cfg_free_period_ms();
        CC_SESSION_UNLOCK(s);
        if (b_confirmed_ms > 0) {
            long long elapsed = timer_now_ms() - b_confirmed_ms;
            long long rem = (long long)free_period - elapsed;
            fp_remaining = rem > 0 ? (int)rem : 0;
        }
    }

    if (fp_remaining > 0)
        PJ_LOG(3, (THIS_FILE,
                   "[FREE-PERIOD] whitelist: waiting remaining %dms after 4.1.wav",
                   fp_remaining));

    post_accept_phase(s, CC_EV_ACCEPT_BRIDGE,
                      call_a, call_b, fp_remaining, PJSUA_INVALID_ID);
}

/* Phase 3 — do the actual bridge/UPDATE */
static void ev_accept_bridge(cc_event_t *ev)
{
    cc_session_t  *s      = ev->session;
    pjsua_call_id  call_a = ev->call_a;
    pjsua_call_id  call_b = ev->call_b;

    /* Stop A MOH / dial-tone player */
    {
        pjsua_player_id pa = PJSUA_INVALID_ID;
        CC_SESSION_LOCK(s);
        if (s->player_a != PJSUA_INVALID_ID) {
            pa = s->player_a;
            s->player_a = PJSUA_INVALID_ID;
        }
        CC_SESSION_UNLOCK(s);
        if (pa != PJSUA_INVALID_ID)
            cc_stop_wav(pa, PJSUA_INVALID_ID);
    }
    /* Stop B dial-tone player (non-whitelisted free-period tone) */
    {
        pjsua_player_id pb = PJSUA_INVALID_ID;
        CC_SESSION_LOCK(s);
        if (s->player_b != PJSUA_INVALID_ID) {
            pb = s->player_b;
            s->player_b = PJSUA_INVALID_ID;
        }
        CC_SESSION_UNLOCK(s);
        if (pb != PJSUA_INVALID_ID)
            cc_stop_wav(pb, PJSUA_INVALID_ID);
    }

    if (accept_is_stale(s, call_a, call_b)) {
        PJ_LOG(3, (THIS_FILE, "[ACCEPT] phase3 stale — abort"));
        CC_SESSION_LOCK(s);
        s->accept_transition_pending = 0;
        CC_SESSION_UNLOCK(s);
        return;
    }

    if (!cc_session_call_is_current(s, call_a, 1) ||
        !cc_session_call_is_current(s, call_b, 0))
    {
        PJ_LOG(3, (THIS_FILE, "[ACCEPT] phase3 call gone — abort"));
        CC_SESSION_LOCK(s);
        s->accept_transition_pending = 0;
        CC_SESSION_UNLOCK(s);
        return;
    }

    /* Set call_connected_ts */
    CC_SESSION_LOCK(s);
    if (s->call_connected_ts == 0)
        s->call_connected_ts = s->b_prompt_start_ts > 0
                               ? s->b_prompt_start_ts : time(NULL);
    CC_SESSION_UNLOCK(s);

    /* Bridge / UPDATE */
    {
        cc_media_mode_t mode = cc_cfg_media_mode();
        if (mode == CC_MEDIA_MODE_LOCAL_BRIDGE) {
            PJ_LOG(3, (THIS_FILE, "[MEDIA-MODE] local_bridge: bridging A<->B"));
            cc_bridge_calls(call_a, call_b);
        } else if (mode == CC_MEDIA_MODE_REINVITE) {
            PJ_LOG(3, (THIS_FILE, "[MEDIA-MODE] reinvite: bridge then re-INVITEs"));
            cc_bridge_calls(call_a, call_b);
            leg_a_send_reinvite_bypass(s);
            leg_b_send_reinvite_bypass(s);
        } else {
            PJ_LOG(3, (THIS_FILE, "[MEDIA-MODE] update: sending SIP UPDATEs"));
            leg_a_send_update_bypass(call_a, s);
            leg_b_send_update_bypass(call_b, s);
        }
    }

    CC_SESSION_LOCK(s);
    s->accept_transition_pending = 0;
    CC_SESSION_UNLOCK(s);
}

/* CC_EV_RING_TIMER / CC_EV_DTMF_TIMER — was timer_thread */
static void ev_timer(cc_event_t *ev, int is_ring)
{
    cc_session_t  *s        = ev->session;
    int            timeout  = ev->timeout_sec;
    pjsua_call_id  call_b   = ev->call_b;
    int remaining_ms = timeout * 1000;
    int done = 0;

    while (remaining_ms > 0) {
        int slice = remaining_ms > 100 ? 100 : remaining_ms;
        cc_sleep_ms(slice);
        remaining_ms -= slice;
        CC_SESSION_LOCK(s);
        done = s->accepted || s->torn_down ||
               s->call_b != call_b || s->final_cleanup_started;
        CC_SESSION_UNLOCK(s);
        if (done) break;
    }

    CC_SESSION_LOCK(s);
    done = s->accepted || s->torn_down ||
           s->call_b != call_b || s->final_cleanup_started;
    CC_SESSION_UNLOCK(s);

    if (!done && cc_session_call_is_current(s, call_b, 0)) {
        if (is_ring) {
            PJ_LOG(2, (THIS_FILE, "[WORKER] ring timeout %ds — NO_ANSWER", timeout));
            CC_SESSION_LOCK(s);
            if (!s->accepted && !s->torn_down && s->call_b == call_b) {
                s->decision_completed = 1;
                s->decision_digit = '\0';
                s->torn_down = 1;
            } else {
                done = 1;
            }
            CC_SESSION_UNLOCK(s);
            if (!done) {
                cc_session_mark_end(s, "FAILED", "NO_ANSWER");
                if (cc_session_call_is_current(s, call_b, 0))
                    cc_safe_hangup(call_b, PJSIP_SC_REQUEST_TIMEOUT);
                leg_a_play_prompt_then_hangup(s,
                    CC_PROMPT_NOT_AVAILABLE_TO_PAY,
                    PJSIP_SC_TEMPORARILY_UNAVAILABLE);
            }
        } else {
            PJ_LOG(2, (THIS_FILE, "[WORKER] DTMF timeout %ds — ELIGIBILITY_TIMEOUT", timeout));
            /* call on_reject_mapped via leg_b public path */
            leg_b_on_dtmf_timeout(call_b, s);
        }
    }

    CC_SESSION_LOCK(s);
    if (is_ring)
        s->ring_timer_started = 0;
    else
        s->dtmf_timer_started = 0;
    CC_SESSION_UNLOCK(s);

}

/* CC_EV_UPDATE_A_RETRY — was update_a_retry_thread */
static void ev_update_a_retry(cc_event_t *ev)
{
    cc_session_t  *s      = ev->session;
    pjsua_call_id  call_a = ev->call_a;
    int skip;

    cc_sleep_ms(3000);

    CC_SESSION_LOCK(s);
    skip = s->torn_down || s->media_bypassed ||
           s->update_a_acked || s->call_a != call_a;
    s->update_a_retry_pending = 0;
    CC_SESSION_UNLOCK(s);

    if (!skip) {
        PJ_LOG(3, (THIS_FILE, "[WORKER] retrying A-leg UPDATE after 491"));
        leg_a_send_update_bypass(call_a, s);
    }

}

/* CC_EV_UPDATE_B_RETRY — was update_b_retry_thread */
static void ev_update_b_retry(cc_event_t *ev)
{
    cc_session_t  *s      = ev->session;
    pjsua_call_id  call_b = ev->call_b;
    int skip;

    cc_sleep_ms(3000);

    CC_SESSION_LOCK(s);
    skip = s->torn_down || s->media_bypassed ||
           s->update_b_acked || s->call_b != call_b;
    s->update_b_retry_pending = 0;
    CC_SESSION_UNLOCK(s);

    if (!skip) {
        PJ_LOG(3, (THIS_FILE, "[WORKER] retrying B-leg UPDATE after 491"));
        leg_b_send_update_bypass(call_b, s);
    }

}

/* CC_EV_UPDATE_ACK_WATCHDOG — was update_ack_watchdog_thread */
#define CC_UPDATE_ACK_TIMEOUT_MS  8000
#define CC_UPDATE_ACK_POLL_MS       100

static void ev_update_ack_watchdog(cc_event_t *ev)
{
    cc_session_t  *s      = ev->session;
    pjsua_call_id  call_a = ev->call_a;
    pjsua_call_id  call_b = ev->call_b;
    int waited_ms = 0;

    while (waited_ms < CC_UPDATE_ACK_TIMEOUT_MS) {
        int done, torn;
        CC_SESSION_LOCK(s);
        torn = s->torn_down || s->call_a != call_a || s->call_b != call_b;
        done = s->media_bypassed || torn;
        CC_SESSION_UNLOCK(s);
        if (torn) goto wd_done;
        if (done) goto wd_done;
        cc_sleep_ms(CC_UPDATE_ACK_POLL_MS);
        waited_ms += CC_UPDATE_ACK_POLL_MS;
    }

    {
        int need_fallback = 0;
        CC_SESSION_LOCK(s);
        if (!s->media_bypassed && !s->torn_down && s->accepted &&
            s->call_a == call_a && s->call_b == call_b)
        {
            PJ_LOG(2, (THIS_FILE,
                       "[WORKER] UPDATE ack timeout %dms — fallback to bridge",
                       CC_UPDATE_ACK_TIMEOUT_MS));
            s->update_a_sent = s->update_b_sent = 0;
            s->update_a_acked = s->update_b_acked = 0;
            s->update_a_pending = s->update_b_pending = 0;
            s->update_a_retry_pending = s->update_b_retry_pending = 0;
            need_fallback = 1;
        }
        CC_SESSION_UNLOCK(s);
        if (need_fallback)
            cc_bridge_calls(call_a, call_b);
    }

wd_done:
    CC_SESSION_LOCK(s);
    s->update_ack_watchdog_started = 0;
    CC_SESSION_UNLOCK(s);
}

/* CC_EV_BYPASS_RTP_WATCHDOG — was cc_bypass_rtp_watchdog_thread */
#define CC_BYPASS_RTP_WATCHDOG_MS  2500
#define CC_BYPASS_RTP_POLL_MS        50

static void ev_bypass_rtp_watchdog(cc_event_t *ev)
{
    cc_session_t  *s      = ev->session;
    pjsua_call_id  call_a = ev->call_a;
    pjsua_call_id  call_b = ev->call_b;
    int waited_ms = 0, rtp_ok = 0;
    unsigned long pkt_a_start = 0, pkt_b_start = 0;
    int snapped = 0;

    while (waited_ms < CC_BYPASS_RTP_WATCHDOG_MS) {
        int torn;
        CC_SESSION_LOCK(s);
        torn = s->torn_down || s->call_a != call_a || s->call_b != call_b;
        CC_SESSION_UNLOCK(s);
        if (torn) { rtp_ok = 1; break; }

        {
            pjsua_stream_stat sa, sb;
            if (pjsua_call_get_stream_stat(call_a, 0, &sa) == PJ_SUCCESS &&
                pjsua_call_get_stream_stat(call_b, 0, &sb) == PJ_SUCCESS)
            {
                if (!snapped) {
                    pkt_a_start = sa.rtcp.rx.pkt;
                    pkt_b_start = sb.rtcp.rx.pkt;
                    snapped = 1;
                } else if (sa.rtcp.rx.pkt > pkt_a_start + 2 &&
                           sb.rtcp.rx.pkt > pkt_b_start + 2)
                {
                    PJ_LOG(3, (THIS_FILE, "[WORKER] bypass RTP flowing — OK"));
                    rtp_ok = 1;
                    break;
                }
            }
        }

        cc_sleep_ms(CC_BYPASS_RTP_POLL_MS);
        waited_ms += CC_BYPASS_RTP_POLL_MS;
    }

    if (!rtp_ok) {
        int do_fallback = 0;
        CC_SESSION_LOCK(s);
        if (!s->torn_down && s->accepted && s->media_bypassed &&
            s->call_a == call_a && s->call_b == call_b)
        {
            s->media_bypassed = s->update_a_sent = s->update_b_sent = 0;
            s->update_a_acked = s->update_b_acked = 0;
            do_fallback = 1;
        }
        CC_SESSION_UNLOCK(s);
        if (do_fallback) {
            PJ_LOG(2, (THIS_FILE,
                       "[WORKER] no direct RTP after %dms — fallback to bridge",
                       CC_BYPASS_RTP_WATCHDOG_MS));
            cc_bridge_calls(call_a, call_b);
        }
    }

    CC_SESSION_LOCK(s);
    s->bypass_rtp_watchdog_started = 0;
    CC_SESSION_UNLOCK(s);
}

/* ── Central dispatcher ──────────────────────────────────────────────────── */

static void process_event(cc_event_t *ev)
{
    switch (ev->type) {
    case CC_EV_ORIGINATE_B:         ev_originate_b(ev);              break;
    case CC_EV_WAV_HANGUP_A:        ev_wav_hangup_a(ev);             break;
    case CC_EV_HANGUP_A_ONLY:       ev_hangup_a_only(ev);            break;
    case CC_EV_MCA_WAIT:            ev_mca_wait(ev);                 break;
    case CC_EV_B_PROMPT_START:      ev_b_prompt_start(ev);           break;
    case CC_EV_B_PROMPT_DONE:       ev_b_prompt_done(ev);            break;
    case CC_EV_ACCEPT_TRANSITION:   ev_accept_transition(ev);        break;
    case CC_EV_ACCEPT_BRIDGE_WAIT:  ev_accept_bridge_wait(ev);       break;
    case CC_EV_ACCEPT_BRIDGE:       ev_accept_bridge(ev);            break;
    case CC_EV_RING_TIMER:          ev_timer(ev, 1);                 break;
    case CC_EV_DTMF_TIMER:          ev_timer(ev, 0);                 break;
    case CC_EV_UPDATE_A_RETRY:      ev_update_a_retry(ev);           break;
    case CC_EV_UPDATE_B_RETRY:      ev_update_b_retry(ev);           break;
    case CC_EV_UPDATE_ACK_WATCHDOG: ev_update_ack_watchdog(ev);      break;
    case CC_EV_BYPASS_RTP_WATCHDOG: ev_bypass_rtp_watchdog(ev);      break;
    case CC_EV_VASYNC_CB:
        /* session is NULL — skip session release */
        if (ev->data) {
            vasync_cb_event_t *e = (vasync_cb_event_t *)ev->data;
            e->cb(e->cb_arg, &e->result);
            free(e);
        }
        return;
    default:
        PJ_LOG(1, (THIS_FILE, "[WORKER] unknown event type=%d", ev->type));
        break;
    }
    /* Clear a_treatment_running before maybe_finalize so the session can be
     * finalized if both legs are gone. Must happen while we still hold the
     * worker's ref (release_reason comes after), so the session is alive. */
    if (ev->session &&
        (ev->type == CC_EV_WAV_HANGUP_A || ev->type == CC_EV_MCA_WAIT ||
         ev->type == CC_EV_HANGUP_A_ONLY)) {
        CC_SESSION_LOCK(ev->session);
        ev->session->a_treatment_running = 0;
        CC_SESSION_UNLOCK(ev->session);
    }
    /* Acquire a local guard ref so the session pointer stays valid across
     * both maybe_finalize and release_reason regardless of which one drops
     * ref_count to 0 and triggers cc_session_destroy. */
    if (ev->session) {
        cc_session_t    *s    = ev->session;
        pthread_mutex_t *lock = s->lock;   /* capture before any release can free s */
        unsigned refs;
        int destroy;

        cc_session_acquire_reason(s, "process-event-guard");
        cc_session_maybe_finalize(s);

        /* Release both ev->reason and process-event-guard inside a single
         * locked block so s is never read after pj_pool_release frees it.
         * cc_session_release_reason is NOT used here — it would read s->lock
         * and s->ref_count after s may already be freed by cc_session_destroy
         * triggered by the first decrement. */
        pthread_mutex_lock(lock);
        refs = s->ref_count;
        /* Decrement ev->reason ref */
        if (refs > 0) { refs--; s->ref_count = refs; }
        /* Decrement process-event-guard ref */
        if (refs > 0) { refs--; s->ref_count = refs; }
        destroy = (refs == 0);
        pthread_mutex_unlock(lock);

        PJ_LOG(4, ("session",
                   "[SESSION] release reason=%s session=%p refs=%u",
                   ev->reason, s, refs > 0 ? refs + 1 : 0));
        PJ_LOG(4, ("session",
                   "[SESSION] release reason=process-event-guard session=%p refs=%u",
                   s, refs));
        if (destroy)
            cc_session_destroy(s);
    }
}
