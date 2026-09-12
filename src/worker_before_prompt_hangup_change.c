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
 *   CC_EV_MCA_WAIT             — MCA DTMF timeout (delayed timer)
 *   CC_EV_MCA_STOP_PROMPT      — stop offer WAV when file ends
 *   CC_EV_MCA_RESOLVE          — A DTMF during MCA wait
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

#define CC_TIMER_HEAP_MAX  16384  /* 100 CPS x 60s hold = 6000 sessions x ~2 timers each */

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
         * Guard: verify the session still owns this call_id slot before
         * stopping the player — the slot may have been reused by a new call
         * whose player_a happens to share the same id.
         * For CC_EV_HANGUP_A_ONLY: player_a is the treatment player —
         * leave it running, ev_hangup_a_only stops it after the delay. */
        if (ev.type == CC_EV_WAV_HANGUP_A && ev.player_a != PJSUA_INVALID_ID) {
            int session_owns_slot = 0;
            if (ev.session && ev.call_a != PJSUA_INVALID_ID) {
                cc_session_t *s = ev.session;
                pthread_mutex_t *lk = s->lock;
                pthread_mutex_lock(lk);
                session_owns_slot = (s->call_a == ev.call_a);
                pthread_mutex_unlock(lk);
            }
            if (session_owns_slot) {
                PJ_LOG(3, (THIS_FILE, "[TIMER] Stop A waiting prompt before treatment"));
                cc_stop_wav(ev.player_a, PJSUA_INVALID_ID);
                ev.player_a = PJSUA_INVALID_ID;
            } else {
                PJ_LOG(3, (THIS_FILE,
                           "[TIMER] Skipping stale player stop — call slot reused"));
                ev.player_a = PJSUA_INVALID_ID;
            }
        }

        /* Post to worker pool — cc_worker_post acquires its own ref for the
         * worker's copy of the event.  The timer held its own ref (ev.reason)
         * while the event sat in the heap; that timer ref must be released
         * here regardless of whether the post succeeded.
         * On failure also clear a_treatment_running so maybe_finalize can run. */
        int posted = (cc_worker_post(&ev) == 0);
        if (!posted && ev.session) {
            pthread_mutex_t *lock = ev.session->lock;
            pthread_mutex_lock(lock);
            ev.session->a_treatment_running = 0;
            pthread_mutex_unlock(lock);
        }
        if (ev.session)
            cc_session_release_reason(ev.session, ev.reason);

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
        /* Stamp the serial at post time under the session lock so the
         * worker can detect stale events from reused call_id slots. */
        CC_SESSION_LOCK(ev->session);
        ev->session_serial = ev->session->session_serial;
        CC_SESSION_UNLOCK(ev->session);

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
    return cc_wav_player_duration_ms(pid);
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

    /* Stale-session guard: if the session no longer owns this call_id slot
     * (slot was reused by a new call), abort entirely — do not stop any
     * player and do not hang up the new call. */
    {
        int session_owns_slot = 0;
        CC_SESSION_LOCK(s);
        session_owns_slot = (s->call_a == call_a);
        CC_SESSION_UNLOCK(s);
        if (!session_owns_slot) {
            PJ_LOG(3, (THIS_FILE,
                       "[WAV] stale CC_EV_WAV_HANGUP_A — call slot reused, aborting"));
            pthread_mutex_t *lock = s->lock;
            pthread_mutex_lock(lock);
            s->a_treatment_running = 0;
            pthread_mutex_unlock(lock);
            return;
        }
    }

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
            /* HANGUP_A_ONLY will not run — clear treatment flag now */
            pthread_mutex_t *lock = s->lock;
            pthread_mutex_lock(lock);
            s->a_treatment_running = 0;
            pthread_mutex_unlock(lock);
        }
        /* a_treatment_running stays set when post succeeded —
         * HANGUP_A_ONLY is the terminal event and clears it via process_event */
        return;
    }
    /* Stale path: call already gone, HANGUP_A_ONLY was never posted.
     * Clear a_treatment_running here so maybe_finalize can proceed.
     * process_event tail must NOT clear it for WAV_HANGUP_A (it would
     * race with an already-queued HANGUP_A_ONLY on the non-stale path). */
    {
        pthread_mutex_t *lock = s->lock;
        pthread_mutex_lock(lock);
        s->a_treatment_running = 0;
        pthread_mutex_unlock(lock);
    }
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

/* Take session->player_a if it still matches expected (or any if expected is
 * PJSUA_INVALID_ID). Returns the id to stop, or PJSUA_INVALID_ID. */
static pjsua_player_id mca_take_player_a(cc_session_t *s,
                                         pjsua_player_id expected)
{
    pjsua_player_id pid = PJSUA_INVALID_ID;

    CC_SESSION_LOCK(s);
    if (s->player_a != PJSUA_INVALID_ID &&
        (expected == PJSUA_INVALID_ID || s->player_a == expected))
    {
        pid = s->player_a;
        s->player_a = PJSUA_INVALID_ID;
    }
    CC_SESSION_UNLOCK(s);
    return pid;
}

static void mca_clear_treatment(cc_session_t *s)
{
    pthread_mutex_t *lock = s->lock;
    pthread_mutex_lock(lock);
    s->a_treatment_running = 0;
    pthread_mutex_unlock(lock);
}

/* Play follow-up WAV then hang up via delayed CC_EV_HANGUP_A_ONLY (no sleep). */
static void mca_followup_wav_then_hangup(cc_session_t *s,
                                         pjsua_call_id call_a,
                                         const char *path,
                                         pjsip_status_code code)
{
    pjsua_player_id pid;
    int wav_ms, cap, wait_ms;
    cc_event_t hev;

    if (!cc_session_call_is_current(s, call_a, 1)) {
        mca_clear_treatment(s);
        return;
    }

    pid = cc_start_wav(call_a, path, PJ_FALSE);
    wav_ms = worker_player_duration_ms(pid);
    cap = cc_cfg_free_period_ms();
    wait_ms = wav_ms > cap ? cap : wav_ms;
    if (wait_ms <= 0)
        wait_ms = 1;

    {
        int stored = 0;
        CC_SESSION_LOCK(s);
        if (s->call_a == call_a && s->player_a == PJSUA_INVALID_ID &&
            pid != PJSUA_INVALID_ID)
        {
            s->player_a = pid;
            stored = 1;
        }
        CC_SESSION_UNLOCK(s);
        if (!stored && pid != PJSUA_INVALID_ID) {
            cc_stop_wav(pid, PJSUA_INVALID_ID);
            pid = PJSUA_INVALID_ID;
        }
    }

    memset(&hev, 0, sizeof(hev));
    hev.type     = CC_EV_HANGUP_A_ONLY;
    hev.session  = s;
    hev.call_a   = call_a;
    hev.sip_code = (int)code;
    hev.player_a = pid;
    snprintf(hev.reason, sizeof(hev.reason), "mca-hangup-a");
    if (cc_worker_post_delayed(&hev, wait_ms) != 0) {
        if (pid != PJSUA_INVALID_ID)
            cc_stop_wav(pid, PJSUA_INVALID_ID);
        if (cc_session_call_is_current(s, call_a, 1))
            cc_safe_hangup(call_a, code);
        mca_clear_treatment(s);
    }
}

/* CC_EV_MCA_STOP_PROMPT — offer WAV finished; keep waiting for DTMF. */
static void ev_mca_stop_prompt(cc_event_t *ev)
{
    cc_session_t *s = ev->session;
    pjsua_player_id take;
    int waiting;

    CC_SESSION_LOCK(s);
    waiting = s->mca_waiting && s->call_a == ev->call_a && !s->mca_decided;
    CC_SESSION_UNLOCK(s);
    if (!waiting)
        return;

    take = mca_take_player_a(s, ev->player_a);
    if (take != PJSUA_INVALID_ID)
        cc_stop_wav(take, PJSUA_INVALID_ID);
}

/* CC_EV_MCA_WAIT — DTMF window expired. Worker runs only at fire time. */
static void ev_mca_wait(cc_event_t *ev)
{
    cc_session_t *s = ev->session;
    pjsua_call_id call_a = ev->call_a;
    pjsua_player_id take;
    int decided;
    int waiting;

    CC_SESSION_LOCK(s);
    decided = s->mca_decided;
    waiting = s->mca_waiting;
    s->mca_waiting = 0;
    CC_SESSION_UNLOCK(s);

    /* Digit already claimed this wait — follow-up event owns treatment. */
    if (decided)
        return;

    take = mca_take_player_a(s, PJSUA_INVALID_ID);
    if (take != PJSUA_INVALID_ID)
        cc_stop_wav(take, PJSUA_INVALID_ID);

    if (waiting && cc_session_call_is_current(s, call_a, 1)) {
        PJ_LOG(3, (THIS_FILE, "[MCA] timeout — no DTMF, hangup A"));
        cc_session_mark_end(s, "FAILED", "SPONSOR_UNREACHABLE_NoMCA");
        cc_safe_hangup(call_a, PJSIP_SC_TEMPORARILY_UNAVAILABLE);
    }

    mca_clear_treatment(s);
}

/* CC_EV_MCA_RESOLVE — A pressed a key during the DTMF window. */
static void ev_mca_resolve(cc_event_t *ev)
{
    cc_session_t *s = ev->session;
    pjsua_call_id call_a = ev->call_a;
    pjsua_player_id take;
    int decided;

    CC_SESSION_LOCK(s);
    decided = s->mca_decided;
    s->mca_waiting = 0;
    CC_SESSION_UNLOCK(s);

    take = mca_take_player_a(s, PJSUA_INVALID_ID);
    if (take != PJSUA_INVALID_ID)
        cc_stop_wav(take, PJSUA_INVALID_ID);

    if (decided == 1) {
        cc_session_mark_end(s, "FAILED", "SPONSOR_UNREACHABLE_MCA");
        mca_followup_wav_then_hangup(s, call_a,
                                     cc_prompt_get_path(CC_PROMPT_MCA_SENT),
                                     PJSIP_SC_OK);
    } else if (decided == 2) {
        cc_session_mark_end(s, "FAILED", "SPONSOR_UNREACHABLE_NoMCA");
        mca_followup_wav_then_hangup(s, call_a,
                                     cc_prompt_get_path(CC_PROMPT_MCA_NOT_SENT),
                                     PJSIP_SC_OK);
    } else {
        cc_session_mark_end(s, "FAILED", "SPONSOR_UNREACHABLE_NoMCA");
        if (cc_session_call_is_current(s, call_a, 1))
            cc_safe_hangup(call_a, PJSIP_SC_TEMPORARILY_UNAVAILABLE);
        mca_clear_treatment(s);
    }
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

        /* Post b_prompt_done event delayed by prompt duration —
         * ev_b_prompt_done is now fire-and-return; the timer heap
         * handles the wait so no worker is blocked. */
        {
            cc_event_t done_ev;
            memset(&done_ev, 0, sizeof(done_ev));
            done_ev.type      = CC_EV_B_PROMPT_DONE;
            done_ev.session   = session;
            done_ev.call_b    = call_id;
            done_ev.prompt_ms = prompt_ms;
            snprintf(done_ev.reason, sizeof(done_ev.reason), "b-prompt-done");
            if (cc_worker_post_delayed(&done_ev, prompt_ms) != 0) {
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

/* CC_EV_B_PROMPT_DONE
 *
 * No sleep loop. Posted via cc_worker_post_delayed(prompt_ms) from
 * ev_b_prompt_start. When the timer fires the worker sets b_collect_done.
 * The 1s RTP log is handled by a separate CC_EV_B_RTP_LOG delayed event.
 * Zero workers blocked for the prompt duration.
 */
static void ev_b_prompt_done(cc_event_t *ev)
{
    cc_session_t *s = ev->session;
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

/* CC_EV_RING_TIMER / CC_EV_DTMF_TIMER
 *
 * No sleep loop. The caller posts this event via cc_worker_post_delayed
 * with the full timeout as wait_ms. When the timer fires, the worker
 * checks whether the session is still waiting and acts immediately.
 * Zero workers blocked for the duration of the timeout.
 */
static void ev_timer(cc_event_t *ev, int is_ring)
{
    cc_session_t  *s      = ev->session;
    pjsua_call_id  call_b = ev->call_b;
    int done = 0;

    CC_SESSION_LOCK(s);
    done = s->accepted || s->torn_down ||
           s->call_b != call_b || s->final_cleanup_started;
    if (is_ring)
        s->ring_timer_started = 0;
    else
        s->dtmf_timer_started = 0;
    CC_SESSION_UNLOCK(s);

    if (done)
        return;

    if (!cc_session_call_is_current(s, call_b, 0))
        return;

    if (is_ring) {
        int fired = 0;
        PJ_LOG(2, (THIS_FILE, "[WORKER] ring timeout — NO_ANSWER"));
        CC_SESSION_LOCK(s);
        if (!s->accepted && !s->torn_down && s->call_b == call_b) {
            s->decision_completed = 1;
            s->decision_digit = '\0';
            s->torn_down = 1;
            fired = 1;
        }
        CC_SESSION_UNLOCK(s);
        if (fired) {
            cc_session_mark_end(s, "FAILED", "NO_ANSWER");
            if (cc_session_call_is_current(s, call_b, 0))
                cc_safe_hangup(call_b, PJSIP_SC_REQUEST_TIMEOUT);
            /* NOT_AVAILABLE_TO_PAY (1.45.wav) — one-shot then hangup, no MCA */
            leg_a_play_prompt_then_hangup(s,
                                          CC_PROMPT_NOT_AVAILABLE_TO_PAY,
                                          PJSIP_SC_TEMPORARILY_UNAVAILABLE);
        }
    } else {
        PJ_LOG(2, (THIS_FILE, "[WORKER] DTMF timeout — ELIGIBILITY_TIMEOUT"));
        leg_b_on_dtmf_timeout(call_b, s);
    }
}

/* After hold, PJSUA transport src_rtp is often invalid:0 because the SBC
 * no longer sends RTP to the B2BUA. Use the bypass endpoints learned at
 * the first UPDATE (session->rtp_a / rtp_b) instead of giving up. */
static int cc_rtp_live_or_cached(pjsua_call_id call_id,
                                 const cc_rtp_ep_t *cached,
                                 cc_rtp_ep_t *out,
                                 const char *tag)
{
    if (cc_get_call_remote_rtp(call_id, out) == PJ_SUCCESS && out->port != 0)
        return 1;
    if (cached && cached->valid && cached->port != 0) {
        *out = *cached;
        PJ_LOG(3, (THIS_FILE, "[%s] using cached RTP %s:%d (transport src empty)",
                   tag, out->ip, out->port));
        return 1;
    }
    return 0;
}

/*
 * UPDATE-ack timeout watchdog tuning. Defined here (not just above
 * ev_update_ack_watchdog further down) because cc_maybe_arm_update_ack_watchdog
 * needs it too, and it's now armed from the send path.
 */
#define CC_UPDATE_ACK_TIMEOUT_MS  8000
#define CC_UPDATE_ACK_POLL_MS       100

/*
 * Arm the UPDATE-ack timeout watchdog as soon as BOTH legs' bypass
 * negotiation has actually been sent to the network — whether via UPDATE
 * (update_a_sent/update_b_sent) or the re-INVITE fallback
 * (reinvite_a_pending/reinvite_b_pending, set true on successful send and
 * only cleared back to 0 if the send itself failed).
 *
 * WHY THIS LIVES HERE AND NOT IN cc_on_call_media_state():
 * The previous implementation only spawned the watchdog from inside
 * cc_on_call_media_state(), which PJSUA only invokes on a SUCCESSFUL media
 * renegotiation. A rejected UPDATE (e.g. 405 Method Not Allowed) or one
 * that never gets any response at all produces no media-state change, so
 * that callback — and the watchdog living inside it — never ran in exactly
 * the failure case it was meant to catch (confirmed against
 * rtp_test_1.pcap / collect_call log: 10s of silence with no UPDATE ack,
 * no watchdog log line, ending in the call being killed by a PJSIP-level
 * transaction timeout instead of falling back to local bridge).
 *
 * Calling this from the send path itself arms the watchdog unconditionally
 * the moment both UPDATEs/re-INVITEs are out on the wire, independent of
 * whether a response — successful or not — ever arrives.
 *
 * Safe to call from both legs' send paths and from cc_on_call_media_state()
 * concurrently: update_ack_watchdog_started is checked-and-set atomically
 * under the session lock, so only the first caller actually posts the
 * event; the rest are no-ops.
 */
static void cc_maybe_arm_update_ack_watchdog(cc_session_t *s,
                                              pjsua_call_id call_a,
                                              pjsua_call_id call_b)
{
    int a_sent, b_sent, need_watchdog = 0;
    cc_event_t ev;

    CC_SESSION_LOCK(s);
    a_sent = s->update_a_sent || s->reinvite_a_pending;
    b_sent = s->update_b_sent || s->reinvite_b_pending;
    if (a_sent && b_sent &&
        !s->media_bypassed &&
        !s->torn_down &&
        s->accepted &&
        s->call_a == call_a &&
        s->call_b == call_b &&
        !s->update_ack_watchdog_started)
    {
        s->update_ack_watchdog_started = 1;
        need_watchdog = 1;
    }
    CC_SESSION_UNLOCK(s);

    if (!need_watchdog)
        return;

    memset(&ev, 0, sizeof(ev));
    ev.type    = CC_EV_UPDATE_ACK_WATCHDOG;
    ev.session = s;
    ev.call_a  = call_a;
    ev.call_b  = call_b;
    snprintf(ev.reason, sizeof(ev.reason), "update-ack-watchdog-send-path");

    if (cc_worker_post(&ev) != 0) {
        CC_SESSION_LOCK(s);
        s->update_ack_watchdog_started = 0;
        CC_SESSION_UNLOCK(s);
        PJ_LOG(1, (THIS_FILE, "[UPDATE-WD] watchdog post failed at send path"));
        return;
    }

    PJ_LOG(3, (THIS_FILE,
               "[UPDATE-WD] watchdog armed at send path — fallback to bridge if no 200 OK in %dms",
               CC_UPDATE_ACK_TIMEOUT_MS));
}

/* CC_EV_UPDATE_A_BYPASS — non-blocking A-leg UPDATE with RTP poll re-post
 *
 * Polls b_reinvite_active + B RTP readiness every 50ms via re-post.
 * Once both conditions are met, arms update_a_pending and sends UPDATE.
 * ev->delay_ms tracks elapsed wait time (max 5000ms).
 */
static void ev_update_a_bypass(cc_event_t *ev)
{
    cc_session_t  *s      = ev->session;
    pjsua_call_id  call_a = ev->call_a;
    pjsua_call_id  call_b = ev->call_b;
    cc_rtp_ep_t    rtp_a, rtp_b, cached_b;
    int            ri_active, torn;
    pjsua_msg_data msg_data;
    pj_status_t    status;

    CC_SESSION_LOCK(s);
    ri_active = s->b_reinvite_active;
    torn = s->torn_down || s->call_a != call_a || s->call_b != call_b;
    cached_b = s->rtp_b;
    CC_SESSION_UNLOCK(s);

    if (torn) return;

    /* Check B-leg CONFIRMED */
    {
        pjsua_call_info ci_b;
        if (pjsua_call_get_info(call_b, &ci_b) != PJ_SUCCESS ||
            ci_b.state != PJSIP_INV_STATE_CONFIRMED)
        {
            PJ_LOG(1, (THIS_FILE, "[A] UPDATE skipped: B-leg not CONFIRMED"));
            return;
        }
    }

    /* If re-INVITE still active or B RTP not ready, re-post after 50ms */
    if (ri_active || !cc_rtp_live_or_cached(call_b, &cached_b, &rtp_b, "A"))
    {
        if (ev->delay_ms >= 5000) {
            PJ_LOG(1, (THIS_FILE, "[A] UPDATE bypass: B RTP not ready after 5s, giving up"));
            return;
        }
        ev->delay_ms += 50;
        cc_worker_post_delayed(ev, 50);
        /* release_reason will be called by process_event for this iteration;
         * post_delayed acquires a new ref for the next iteration */
        return;
    }

    PJ_LOG(3, (THIS_FILE, "[A] B RTP ready after %dms: %s:%d",
               ev->delay_ms, rtp_b.ip, rtp_b.port));

    /* A-leg UPDATE SDP only needs rtp_b (what to tell A to send to).
     * rtp_a is not required here — after bypass the SBC stops sending
     * RTP to B2BUA so cc_get_call_remote_rtp(call_a) returns stale/zero.
     * Conditioning update_a_pending on rtp_a caused the SDP rewrite to
     * be skipped, sending the UPDATE with B2BUA's own IP instead. */
    if (cc_get_call_remote_rtp(call_a, &rtp_a) == PJ_SUCCESS && rtp_a.port != 0)
        PJ_LOG(3, (THIS_FILE, "[A] A RTP: %s:%d", rtp_a.ip, rtp_a.port));
    else
        PJ_LOG(3, (THIS_FILE, "[A] A RTP not readable (post-bypass normal) — arming rewrite with rtp_b only"));

    CC_SESSION_LOCK(s);
    if (s->call_a == call_a && s->call_b == call_b && !s->torn_down) {
        s->rtp_b = rtp_b;
        s->update_a_pending = 1;
    }
    CC_SESSION_UNLOCK(s);
    PJ_LOG(3, (THIS_FILE, "[A] UPDATE rewrite armed: A will receive B RTP %s:%d",
               rtp_b.ip, rtp_b.port));

    pjsua_msg_data_init(&msg_data);
    PJ_LOG(3, (THIS_FILE, "[A] Sending SIP UPDATE"));
    status = pjsua_call_update(call_a, 0, &msg_data);

    CC_SESSION_LOCK(s);
    if (status == PJ_SUCCESS) {
        s->update_a_sent = 1;
        PJ_LOG(3, (THIS_FILE, "[A] SIP UPDATE sent"));
    } else {
        s->update_a_pending = 0;
        PJ_LOG(1, (THIS_FILE, "[A] SIP UPDATE failed: %d — retry path will handle", status));
    }
    CC_SESSION_UNLOCK(s);

    if (status == PJ_SUCCESS)
        cc_maybe_arm_update_ack_watchdog(s, call_a, call_b);
}

/* CC_EV_UPDATE_B_BYPASS — non-blocking B-leg UPDATE with RTP poll re-post */
static void ev_update_b_bypass(cc_event_t *ev)
{
    cc_session_t  *s      = ev->session;
    pjsua_call_id  call_a = ev->call_a;
    pjsua_call_id  call_b = ev->call_b;
    cc_rtp_ep_t    rtp_a, rtp_b, cached_a, cached_b;
    int            ri_active, torn;
    pjsua_msg_data msg_data;
    pj_status_t    status;

    CC_SESSION_LOCK(s);
    ri_active = s->b_reinvite_active;
    torn = s->torn_down || s->call_a != call_a || s->call_b != call_b;
    cached_a = s->rtp_a;
    cached_b = s->rtp_b;
    CC_SESSION_UNLOCK(s);

    if (torn) return;

    /* Check B-leg CONFIRMED */
    {
        pjsua_call_info ci_b;
        if (pjsua_call_get_info(call_b, &ci_b) != PJ_SUCCESS ||
            ci_b.state != PJSIP_INV_STATE_CONFIRMED)
        {
            PJ_LOG(1, (THIS_FILE, "[B] UPDATE skipped: B-leg not CONFIRMED"));
            return;
        }
    }

    /* If re-INVITE still active or B RTP not ready, re-post after 50ms */
    if (ri_active || !cc_rtp_live_or_cached(call_b, &cached_b, &rtp_b, "B"))
    {
        if (ev->delay_ms >= 5000) {
            PJ_LOG(1, (THIS_FILE, "[B] UPDATE bypass: B RTP not ready after 5s, giving up"));
            return;
        }
        ev->delay_ms += 50;
        cc_worker_post_delayed(ev, 50);
        return;
    }

    PJ_LOG(3, (THIS_FILE, "[B] B RTP ready after %dms: %s:%d",
               ev->delay_ms, rtp_b.ip, rtp_b.port));

    /* Also wait for A RTP (typically instant) */
    if (!cc_rtp_live_or_cached(call_a, &cached_a, &rtp_a, "B")) {
        if (ev->delay_ms >= 5000) {
            PJ_LOG(1, (THIS_FILE, "[B] UPDATE bypass: A RTP not ready after 5s, giving up"));
            return;
        }
        ev->delay_ms += 50;
        cc_worker_post_delayed(ev, 50);
        return;
    }

    PJ_LOG(3, (THIS_FILE, "[B] A RTP ready: %s:%d", rtp_a.ip, rtp_a.port));

    CC_SESSION_LOCK(s);
    if (s->call_a == call_a && s->call_b == call_b && !s->torn_down) {
        s->rtp_a = rtp_a;
        s->rtp_b = rtp_b;
        s->update_b_pending = 1;
    }
    CC_SESSION_UNLOCK(s);
    PJ_LOG(3, (THIS_FILE, "[B] UPDATE rewrite armed: B will receive A RTP %s:%d",
               rtp_a.ip, rtp_a.port));

    pjsua_msg_data_init(&msg_data);
    PJ_LOG(3, (THIS_FILE, "[B] Sending SIP UPDATE"));
    status = pjsua_call_update(call_b, 0, &msg_data);

    CC_SESSION_LOCK(s);
    if (status == PJ_SUCCESS) {
        s->update_b_sent = 1;
        PJ_LOG(3, (THIS_FILE, "[B] SIP UPDATE sent"));
    } else {
        s->update_b_pending = 0;
        PJ_LOG(1, (THIS_FILE, "[B] SIP UPDATE failed: %d — retry path will handle", status));
    }
    CC_SESSION_UNLOCK(s);

    if (status == PJ_SUCCESS)
        cc_maybe_arm_update_ack_watchdog(s, call_a, call_b);
}

/* CC_EV_REINVITE_A_BYPASS — non-blocking A-leg re-INVITE with RTP poll re-post
 *
 * Mirrors ev_update_a_bypass exactly:
 *   - checks b_reinvite_active (SBC late-offer re-INVITE in progress)
 *   - checks B RTP ready (src_rtp_name valid)
 *   - checks A RTP ready
 *   - re-posts every 50ms if not ready (max 5s via ev->delay_ms)
 *   - arms reinvite_a_pending, stores rtp_a/rtp_b, calls pjsua_call_reinvite once
 */
static void ev_reinvite_a_bypass(cc_event_t *ev)
{
    cc_session_t  *s      = ev->session;
    pjsua_call_id  call_a = ev->call_a;
    pjsua_call_id  call_b = ev->call_b;
    cc_rtp_ep_t    rtp_a, rtp_b;
    int            ri_active, torn;
    pjsua_msg_data msg_data;
    pj_status_t    status;

    CC_SESSION_LOCK(s);
    ri_active = s->b_reinvite_active;
    torn = s->torn_down || s->call_a != call_a || s->call_b != call_b;
    CC_SESSION_UNLOCK(s);

    if (torn) return;

    /* Check B-leg CONFIRMED */
    {
        pjsua_call_info ci_b;
        if (pjsua_call_get_info(call_b, &ci_b) != PJ_SUCCESS ||
            ci_b.state != PJSIP_INV_STATE_CONFIRMED)
        {
            PJ_LOG(1, (THIS_FILE, "[A] re-INVITE skipped: B-leg not CONFIRMED"));
            return;
        }
    }

    /* If SBC re-INVITE active or B RTP not ready, re-post after 50ms */
    if (ri_active ||
        cc_get_call_remote_rtp(call_b, &rtp_b) != PJ_SUCCESS ||
        rtp_b.port == 0)
    {
        if (ev->delay_ms >= 5000) {
            PJ_LOG(1, (THIS_FILE, "[A] re-INVITE bypass: B RTP not ready after 5s, giving up"));
            return;
        }
        ev->delay_ms += 50;
        cc_worker_post_delayed(ev, 50);
        return;
    }

    PJ_LOG(3, (THIS_FILE, "[A] re-INVITE: B RTP ready after %dms: %s:%d",
               ev->delay_ms, rtp_b.ip, rtp_b.port));

    /* Also wait for A RTP */
    if (cc_get_call_remote_rtp(call_a, &rtp_a) != PJ_SUCCESS || rtp_a.port == 0) {
        if (ev->delay_ms >= 5000) {
            PJ_LOG(1, (THIS_FILE, "[A] re-INVITE bypass: A RTP not ready after 5s, giving up"));
            return;
        }
        ev->delay_ms += 50;
        cc_worker_post_delayed(ev, 50);
        return;
    }

    CC_SESSION_LOCK(s);
    if (s->call_a == call_a && s->call_b == call_b && !s->torn_down) {
        s->rtp_a = rtp_a;
        s->rtp_b = rtp_b;
        s->reinvite_a_pending = 1;
    }
    CC_SESSION_UNLOCK(s);

    PJ_LOG(3, (THIS_FILE, "[A] re-INVITE rewrite armed: A will receive B RTP %s:%d",
               rtp_b.ip, rtp_b.port));

    pjsua_msg_data_init(&msg_data);
    PJ_LOG(3, (THIS_FILE, "[A] Sending SIP re-INVITE"));
    status = pjsua_call_reinvite(call_a, 0, &msg_data);

    if (status != PJ_SUCCESS) {
        CC_SESSION_LOCK(s);
        s->reinvite_a_pending = 0;
        CC_SESSION_UNLOCK(s);
        PJ_LOG(1, (THIS_FILE, "[A] SIP re-INVITE failed: %d", status));
    } else {
        PJ_LOG(3, (THIS_FILE, "[A] SIP re-INVITE sent"));
        cc_maybe_arm_update_ack_watchdog(s, call_a, call_b);
    }
}

/* CC_EV_REINVITE_B_BYPASS — non-blocking B-leg re-INVITE with RTP poll re-post */
static void ev_reinvite_b_bypass(cc_event_t *ev)
{
    cc_session_t  *s      = ev->session;
    pjsua_call_id  call_a = ev->call_a;
    pjsua_call_id  call_b = ev->call_b;
    cc_rtp_ep_t    rtp_a, rtp_b;
    int            ri_active, torn;
    pjsua_msg_data msg_data;
    pj_status_t    status;

    CC_SESSION_LOCK(s);
    ri_active = s->b_reinvite_active;
    torn = s->torn_down || s->call_a != call_a || s->call_b != call_b;
    CC_SESSION_UNLOCK(s);

    if (torn) return;

    /* Check B-leg CONFIRMED */
    {
        pjsua_call_info ci_b;
        if (pjsua_call_get_info(call_b, &ci_b) != PJ_SUCCESS ||
            ci_b.state != PJSIP_INV_STATE_CONFIRMED)
        {
            PJ_LOG(1, (THIS_FILE, "[B] re-INVITE skipped: B-leg not CONFIRMED"));
            return;
        }
    }

    /* If SBC re-INVITE active or B RTP not ready, re-post after 50ms */
    if (ri_active ||
        cc_get_call_remote_rtp(call_b, &rtp_b) != PJ_SUCCESS ||
        rtp_b.port == 0)
    {
        if (ev->delay_ms >= 5000) {
            PJ_LOG(1, (THIS_FILE, "[B] re-INVITE bypass: B RTP not ready after 5s, giving up"));
            return;
        }
        ev->delay_ms += 50;
        cc_worker_post_delayed(ev, 50);
        return;
    }

    PJ_LOG(3, (THIS_FILE, "[B] re-INVITE: B RTP ready after %dms: %s:%d",
               ev->delay_ms, rtp_b.ip, rtp_b.port));

    /* Also wait for A RTP */
    if (cc_get_call_remote_rtp(call_a, &rtp_a) != PJ_SUCCESS || rtp_a.port == 0) {
        if (ev->delay_ms >= 5000) {
            PJ_LOG(1, (THIS_FILE, "[B] re-INVITE bypass: A RTP not ready after 5s, giving up"));
            return;
        }
        ev->delay_ms += 50;
        cc_worker_post_delayed(ev, 50);
        return;
    }

    PJ_LOG(3, (THIS_FILE, "[B] re-INVITE: A RTP ready: %s:%d", rtp_a.ip, rtp_a.port));

    CC_SESSION_LOCK(s);
    if (s->call_a == call_a && s->call_b == call_b && !s->torn_down) {
        s->rtp_a = rtp_a;
        s->rtp_b = rtp_b;
        s->reinvite_b_pending = 1;
    }
    CC_SESSION_UNLOCK(s);

    PJ_LOG(3, (THIS_FILE, "[B] re-INVITE rewrite armed: B will receive A RTP %s:%d",
               rtp_a.ip, rtp_a.port));

    pjsua_msg_data_init(&msg_data);
    PJ_LOG(3, (THIS_FILE, "[B] Sending SIP re-INVITE"));
    status = pjsua_call_reinvite(call_b, 0, &msg_data);

    if (status != PJ_SUCCESS) {
        CC_SESSION_LOCK(s);
        s->reinvite_b_pending = 0;
        CC_SESSION_UNLOCK(s);
        PJ_LOG(1, (THIS_FILE, "[B] SIP re-INVITE failed: %d", status));
    } else {
        PJ_LOG(3, (THIS_FILE, "[B] SIP re-INVITE sent"));
        cc_maybe_arm_update_ack_watchdog(s, call_a, call_b);
    }
}

/* CC_EV_HOLD_PROPAGATE_B — Mode 2 (REQ-11..REQ-15)
 *
 * A put the call on hold (sendonly re-INVITE). PJSUA already answered A
 * locally with recvonly. This worker propagates the hold to B:
 *   1. Send UPDATE sendonly to B (fallback: re-INVITE if !b_update_allowed)
 *   2. Poll hold_update_b_acked up to 5s (set by cc_on_call_media_state
 *      when B's media state fires after the UPDATE 200 OK)
 *   3. On success: play MOH to A (A hears audio while B is on hold)
 *   4. On failure (REQ-15): log and play MOH anyway — call stays up
 *
 * REQ-20: hold_propagate_pending guards against overlapping hold UPDATEs.
 */
static void ev_hold_propagate_b(cc_event_t *ev)
{
    cc_session_t  *s      = ev->session;
    pjsua_call_id  call_a = ev->call_a;
    pjsua_call_id  call_b = ev->call_b;
    int            torn, b_update_ok;
    pjsua_msg_data msg_data;
    pj_status_t    status;

    CC_SESSION_LOCK(s);
    torn        = s->torn_down || s->call_a != call_a || s->call_b != call_b;
    b_update_ok = s->b_update_allowed;
    /* Reset ack flag for this new hold transaction */
    s->hold_update_b_sent  = 0;
    s->hold_update_b_acked = 0;
    CC_SESSION_UNLOCK(s);

    if (torn) {
        CC_SESSION_LOCK(s);
        s->hold_propagate_pending = 0;
        CC_SESSION_UNLOCK(s);
        return;
    }

    PJ_LOG(3, (THIS_FILE, "[HOLD/M2] propagating sendonly to B via %s",
               b_update_ok ? "UPDATE" : "re-INVITE"));

    /* Arm SDP rewrite before pjsua_call_update/reinvite: cc_on_call_sdp_created
     * runs synchronously inside that call and must see hold_sdp_b_pending set. */
    CC_SESSION_LOCK(s);
    s->resume_sdp_b_pending = 0;
    s->update_b_pending = 0;
    s->reinvite_b_pending = 0;
    if (s->rtp_a.port != 0 && !s->torn_down && s->call_b == call_b)
        s->hold_sdp_b_pending = 1;
    else {
        s->hold_sdp_b_pending = 0;
        PJ_LOG(1, (THIS_FILE,
                   "[HOLD/M2] hold SDP rewrite not armed (rtp_a=%s:%d) — "
                   "UPDATE would advertise B2BUA local IP",
                   s->rtp_a.ip, s->rtp_a.port));
    }
    CC_SESSION_UNLOCK(s);

    pjsua_msg_data_init(&msg_data);
    if (b_update_ok)
        status = pjsua_call_update(call_b, 0, &msg_data);
    else
        status = pjsua_call_reinvite(call_b, 0, &msg_data);

    if (status != PJ_SUCCESS) {
        CC_SESSION_LOCK(s);
        s->hold_sdp_b_pending = 0;
        CC_SESSION_UNLOCK(s);
    }

    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE, "[HOLD/M2] hold %s to B failed: %d — playing MOH anyway",
                   b_update_ok ? "UPDATE" : "re-INVITE", status));
        goto hold_play_moh;
    }

    CC_SESSION_LOCK(s);
    s->hold_update_b_sent = 1;
    CC_SESSION_UNLOCK(s);

    /* Poll for B's 200 OK (hold_update_b_acked set by cc_on_call_media_state) */
    {
        int waited = 0, acked = 0;
        while (waited < 5000) {
            cc_sleep_ms(100);
            waited += 100;
            CC_SESSION_LOCK(s);
            acked = s->hold_update_b_acked;
            torn  = s->torn_down || s->call_b != call_b;
            CC_SESSION_UNLOCK(s);
            if (acked || torn) break;
        }
        if (!acked)
            PJ_LOG(2, (THIS_FILE, "[HOLD/M2] B hold UPDATE 200 OK not received in 5s — continuing"));
        else
            PJ_LOG(3, (THIS_FILE, "[HOLD/M2] B confirmed hold UPDATE"));
    }

hold_play_moh:
    /* MOH policy (REQ-5):
     * If CC_HOLD_MOH_SOURCE=b2bua (default when SBC does not inject MOH):
     *   re-enter RTP path and play WAV toward A.
     * If CC_HOLD_MOH_SOURCE=sbc or CC_HOLD_MOH_SOURCE=none:
     *   do nothing — SBC injects MOH or A's handset plays local hold tone.
     * In Mode 2 the B2BUA is a signalling relay; MOH from B2BUA is only
     * needed when the SBC has no MOH capability. */
    {
        const char *moh_src = getenv("CC_HOLD_MOH_SOURCE");
        int b2bua_moh = (!moh_src || moh_src[0] == '\0' ||
                         strcasecmp(moh_src, "b2bua") == 0);
        if (b2bua_moh && !torn && cc_session_call_is_current(s, call_a, 1)) {
            pjsua_player_id moh_pid = PJSUA_INVALID_ID;
            /* Undo the TX pause set by cc_silence_call() at bypass time —
             * otherwise the stream is still paused and this WAV would
             * never actually reach the wire even though conf_connect
             * succeeds. Only call_a needs TX back (it's the leg carrying
             * MOH); call_b stays paused so no stray B2BUA-origin RTP goes
             * toward B's real address while B is signaled sendonly. */
            cc_resume_call_tx(call_a);
            cc_bridge_calls(call_a, call_b);
            moh_pid = cc_start_wav(call_a, cc_prompt_get_path(CC_PROMPT_MOH), PJ_TRUE);
            CC_SESSION_LOCK(s);
            if (moh_pid != PJSUA_INVALID_ID &&
                s->hold_player_a == PJSUA_INVALID_ID &&
                !s->torn_down)
            {
                s->hold_player_a = moh_pid;
            } else if (moh_pid != PJSUA_INVALID_ID) {
                CC_SESSION_UNLOCK(s);
                cc_stop_wav(moh_pid, PJSUA_INVALID_ID);
                goto hold_done;
            }
            CC_SESSION_UNLOCK(s);
            PJ_LOG(3, (THIS_FILE, "[HOLD/M2] MOH source=b2bua: playing WAV to A"));
        } else if (!b2bua_moh) {
            PJ_LOG(3, (THIS_FILE, "[HOLD/M2] MOH source=%s: no B2BUA MOH injection",
                       moh_src));
        }
    }
hold_done:
    CC_SESSION_LOCK(s);
    s->hold_propagate_pending = 0;
    CC_SESSION_UNLOCK(s);
}

/* CC_EV_RESUME_PROPAGATE_B — Mode 2 (REQ-17..REQ-20)
 *
 * A resumed (sendrecv re-INVITE). PJSUA already answered A locally.
 * This worker propagates the resume to B:
 *   1. Stop MOH on A
 *   2. Send UPDATE sendrecv to B (fallback: re-INVITE)
 *   3. Poll for B's 200 OK up to 5s
 *   4. Reset hold state and re-run bypass UPDATEs to restore direct RTP
 *
 * REQ-20: resume_propagate_pending guards against overlapping transactions.
 */
static void ev_resume_propagate_b(cc_event_t *ev)
{
    cc_session_t  *s      = ev->session;
    pjsua_call_id  call_a = ev->call_a;
    pjsua_call_id  call_b = ev->call_b;
    int            torn, b_update_ok;
    pjsua_player_id moh_pid = PJSUA_INVALID_ID;
    pjsua_msg_data  msg_data;
    pj_status_t     status;

    CC_SESSION_LOCK(s);
    torn        = s->torn_down || s->call_a != call_a || s->call_b != call_b;
    b_update_ok = s->b_update_allowed;
    if (s->hold_player_a != PJSUA_INVALID_ID) {
        moh_pid = s->hold_player_a;
        s->hold_player_a = PJSUA_INVALID_ID;
    }
    CC_SESSION_UNLOCK(s);

    /* Stop MOH on A immediately */
    if (moh_pid != PJSUA_INVALID_ID)
        cc_stop_wav(moh_pid, PJSUA_INVALID_ID);

    if (torn) {
        CC_SESSION_LOCK(s);
        s->resume_propagate_pending = 0;
        CC_SESSION_UNLOCK(s);
        return;
    }

    PJ_LOG(3, (THIS_FILE, "[HOLD/M2] propagating sendrecv resume to B via %s",
               b_update_ok ? "UPDATE" : "re-INVITE"));

    /* Arm before pjsua_call_update/reinvite — sdp_created is synchronous. */
    CC_SESSION_LOCK(s);
    s->hold_sdp_b_pending = 0;
    s->hold_sdp_direction = 0;
    s->update_b_pending = 0;
    s->reinvite_b_pending = 0;
    if (s->rtp_a.port != 0 && !s->torn_down && s->call_b == call_b)
        s->resume_sdp_b_pending = 1;
    else {
        s->resume_sdp_b_pending = 0;
        PJ_LOG(1, (THIS_FILE,
                   "[HOLD/M2] resume SDP rewrite not armed (rtp_a=%s:%d)",
                   s->rtp_a.ip, s->rtp_a.port));
    }
    CC_SESSION_UNLOCK(s);

    pjsua_msg_data_init(&msg_data);
    if (b_update_ok)
        status = pjsua_call_update(call_b, 0, &msg_data);
    else
        status = pjsua_call_reinvite(call_b, 0, &msg_data);

    if (status != PJ_SUCCESS) {
        CC_SESSION_LOCK(s);
        s->resume_sdp_b_pending = 0;
        CC_SESSION_UNLOCK(s);
        PJ_LOG(1, (THIS_FILE, "[HOLD/M2] resume %s to B failed: %d — attempting bypass anyway",
                   b_update_ok ? "UPDATE" : "re-INVITE", status));
        goto resume_bypass;
    }

    /* Poll for B's media state to return ACTIVE (resume confirmed) */
    {
        int waited = 0, active = 0;
        while (waited < 5000) {
            cc_sleep_ms(100);
            waited += 100;
            CC_SESSION_LOCK(s);
            torn = s->torn_down || s->call_b != call_b;
            CC_SESSION_UNLOCK(s);
            if (torn) goto resume_done;
            pjsua_call_info ci_b;
            if (pjsua_call_get_info(call_b, &ci_b) == PJ_SUCCESS &&
                ci_b.media_cnt > 0 &&
                ci_b.media[0].status == PJSUA_CALL_MEDIA_ACTIVE)
            {
                active = 1;
                break;
            }
        }
        if (!active)
            PJ_LOG(2, (THIS_FILE, "[HOLD/M2] B resume not confirmed in 5s — continuing"));
        else
            PJ_LOG(3, (THIS_FILE, "[HOLD/M2] B confirmed resume"));
    }

resume_bypass:
    /* Reset bypass flags and re-run bypass UPDATEs to restore direct RTP */
    CC_SESSION_LOCK(s);
    torn = s->torn_down || s->call_a != call_a || s->call_b != call_b;
    if (!torn) {
        s->hold_update_b_sent    = 0;
        s->hold_update_b_acked   = 0;
        s->media_bypassed        = 0;
        s->update_a_sent         = 0;
        s->update_b_sent         = 0;
        s->update_a_acked        = 0;
        s->update_b_acked        = 0;
        s->update_b_retry_pending  = 0;
        s->update_a_retry_pending  = 0;
        s->update_ack_watchdog_started = 0;
        s->b_reinvite_active     = 0;
    }
    CC_SESSION_UNLOCK(s);

    if (!torn) {
        PJ_LOG(3, (THIS_FILE, "[HOLD/M2] resume: re-running bypass UPDATEs"));
        leg_a_send_update_bypass(call_a, s);
        leg_b_send_update_bypass(call_b, s);
    }

resume_done:
    CC_SESSION_LOCK(s);
    s->resume_propagate_pending = 0;
    CC_SESSION_UNLOCK(s);
}

/* CC_EV_HOLD_PROPAGATE_A — mirror of ev_hold_propagate_b (A<->B swapped)
 *
 * B put the call on hold (sendonly re-INVITE). PJSUA already answered B
 * locally. This worker propagates the hold to A:
 *   1. Send UPDATE sendonly to A (fallback: re-INVITE if !a_update_allowed)
 *   2. Poll hold_update_a_acked up to 5s (set by cc_on_call_media_state
 *      when A's media state fires after the UPDATE 200 OK)
 *   3. On success: play MOH to B (B hears audio while A is on hold) —
 *      matches the target convention ev_hold_propagate_b already uses
 *      (MOH goes to the leg that is NOT being re-signaled).
 *   4. On failure: log and play MOH anyway — call stays up
 *
 * hold_propagate_a_pending guards against overlapping hold UPDATEs.
 *
 * IMPORTANT (stray RTP): only call_b's TX is resumed here (cc_resume_call_tx)
 * before re-entering the conf bridge for MOH. call_a's B2BUA-side stream
 * stays paused throughout — A's SBC is expecting direct bypass RTP from B's
 * real address, and resuming call_a's TX too would put a second,
 * B2BUA-origin RTP stream on the wire toward A. That duplicate-source
 * scenario is exactly the "stray RTP after UPDATE" issue seen in the pcap.
 */
static void ev_hold_propagate_a(cc_event_t *ev)
{
    cc_session_t  *s      = ev->session;
    pjsua_call_id  call_a = ev->call_a;
    pjsua_call_id  call_b = ev->call_b;
    int            torn, a_update_ok;
    pjsua_msg_data msg_data;
    pj_status_t    status;

    CC_SESSION_LOCK(s);
    torn        = s->torn_down || s->call_a != call_a || s->call_b != call_b;
    a_update_ok = s->a_update_allowed;
    /* Reset ack flag for this new hold transaction */
    s->hold_update_a_sent  = 0;
    s->hold_update_a_acked = 0;
    CC_SESSION_UNLOCK(s);

    if (torn) {
        CC_SESSION_LOCK(s);
        s->hold_propagate_a_pending = 0;
        CC_SESSION_UNLOCK(s);
        return;
    }

    PJ_LOG(3, (THIS_FILE, "[HOLD/M2] propagating sendonly to A via %s",
               a_update_ok ? "UPDATE" : "re-INVITE"));

    /* Arm SDP rewrite before pjsua_call_update/reinvite: cc_on_call_sdp_created
     * runs synchronously inside that call and must see hold_sdp_a_pending set. */
    CC_SESSION_LOCK(s);
    s->resume_sdp_a_pending = 0;
    s->update_a_pending = 0;
    s->reinvite_a_pending = 0;
    if (s->rtp_b.port != 0 && !s->torn_down && s->call_a == call_a)
        s->hold_sdp_a_pending = 1;
    else {
        s->hold_sdp_a_pending = 0;
        PJ_LOG(1, (THIS_FILE,
                   "[HOLD/M2] hold SDP rewrite not armed (rtp_b=%s:%d) — "
                   "UPDATE would advertise B2BUA local IP",
                   s->rtp_b.ip, s->rtp_b.port));
    }
    CC_SESSION_UNLOCK(s);

    pjsua_msg_data_init(&msg_data);
    if (a_update_ok)
        status = pjsua_call_update(call_a, 0, &msg_data);
    else
        status = pjsua_call_reinvite(call_a, 0, &msg_data);

    if (status != PJ_SUCCESS) {
        CC_SESSION_LOCK(s);
        s->hold_sdp_a_pending = 0;
        CC_SESSION_UNLOCK(s);
    }

    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE, "[HOLD/M2] hold %s to A failed: %d — playing MOH anyway",
                   a_update_ok ? "UPDATE" : "re-INVITE", status));
        goto hold_play_moh_b;
    }

    CC_SESSION_LOCK(s);
    s->hold_update_a_sent = 1;
    CC_SESSION_UNLOCK(s);

    /* Poll for A's 200 OK (hold_update_a_acked set by cc_on_call_media_state) */
    {
        int waited = 0, acked = 0;
        while (waited < 5000) {
            cc_sleep_ms(100);
            waited += 100;
            CC_SESSION_LOCK(s);
            acked = s->hold_update_a_acked;
            torn  = s->torn_down || s->call_a != call_a;
            CC_SESSION_UNLOCK(s);
            if (acked || torn) break;
        }
        if (!acked)
            PJ_LOG(2, (THIS_FILE, "[HOLD/M2] A hold UPDATE 200 OK not received in 5s — continuing"));
        else
            PJ_LOG(3, (THIS_FILE, "[HOLD/M2] A confirmed hold UPDATE"));
    }

hold_play_moh_b:
    /* MOH policy — same CC_HOLD_MOH_SOURCE env var as ev_hold_propagate_b. */
    {
        const char *moh_src = getenv("CC_HOLD_MOH_SOURCE");
        int b2bua_moh = (!moh_src || moh_src[0] == '\0' ||
                         strcasecmp(moh_src, "b2bua") == 0);
        if (b2bua_moh && !torn && cc_session_call_is_current(s, call_b, 0)) {
            pjsua_player_id moh_pid = PJSUA_INVALID_ID;
            /* Resume only call_b's TX — call_a stays paused (see header
             * comment: no stray B2BUA-origin RTP toward A). */
            cc_resume_call_tx(call_b);
            cc_bridge_calls(call_a, call_b);
            moh_pid = cc_start_wav(call_b, cc_prompt_get_path(CC_PROMPT_MOH), PJ_TRUE);
            CC_SESSION_LOCK(s);
            if (moh_pid != PJSUA_INVALID_ID &&
                s->hold_player_b == PJSUA_INVALID_ID &&
                !s->torn_down)
            {
                s->hold_player_b = moh_pid;
            } else if (moh_pid != PJSUA_INVALID_ID) {
                CC_SESSION_UNLOCK(s);
                cc_stop_wav(moh_pid, PJSUA_INVALID_ID);
                goto hold_a_done;
            }
            CC_SESSION_UNLOCK(s);
            PJ_LOG(3, (THIS_FILE, "[HOLD/M2] MOH source=b2bua: playing WAV to B"));
        } else if (!b2bua_moh) {
            PJ_LOG(3, (THIS_FILE, "[HOLD/M2] MOH source=%s: no B2BUA MOH injection",
                       moh_src));
        }
    }
hold_a_done:
    CC_SESSION_LOCK(s);
    s->hold_propagate_a_pending = 0;
    CC_SESSION_UNLOCK(s);
}

/* CC_EV_RESUME_PROPAGATE_A — mirror of ev_resume_propagate_b (A<->B swapped)
 *
 * B resumed (sendrecv re-INVITE). PJSUA already answered B locally.
 * This worker propagates the resume to A:
 *   1. Stop MOH on B
 *   2. Send UPDATE sendrecv to A (fallback: re-INVITE)
 *   3. Poll for A's 200 OK up to 5s
 *   4. Reset hold state and re-run bypass UPDATEs to restore direct RTP —
 *      this also re-silences (pauses TX on) both legs via the normal
 *      do_silence path once both bypass UPDATEs are re-acked, so no extra
 *      cc_silence_call() call is needed here.
 *
 * resume_propagate_a_pending guards against overlapping transactions.
 */
static void ev_resume_propagate_a(cc_event_t *ev)
{
    cc_session_t  *s      = ev->session;
    pjsua_call_id  call_a = ev->call_a;
    pjsua_call_id  call_b = ev->call_b;
    int            torn, a_update_ok;
    pjsua_player_id moh_pid = PJSUA_INVALID_ID;
    pjsua_msg_data  msg_data;
    pj_status_t     status;

    CC_SESSION_LOCK(s);
    torn        = s->torn_down || s->call_a != call_a || s->call_b != call_b;
    a_update_ok = s->a_update_allowed;
    if (s->hold_player_b != PJSUA_INVALID_ID) {
        moh_pid = s->hold_player_b;
        s->hold_player_b = PJSUA_INVALID_ID;
    }
    CC_SESSION_UNLOCK(s);

    /* Stop MOH on B immediately */
    if (moh_pid != PJSUA_INVALID_ID)
        cc_stop_wav(moh_pid, PJSUA_INVALID_ID);

    if (torn) {
        CC_SESSION_LOCK(s);
        s->resume_propagate_a_pending = 0;
        CC_SESSION_UNLOCK(s);
        return;
    }

    PJ_LOG(3, (THIS_FILE, "[HOLD/M2] propagating sendrecv resume to A via %s",
               a_update_ok ? "UPDATE" : "re-INVITE"));

    /* Arm before pjsua_call_update/reinvite — sdp_created is synchronous. */
    CC_SESSION_LOCK(s);
    s->hold_sdp_a_pending = 0;
    s->hold_sdp_direction = 0;
    s->update_a_pending = 0;
    s->reinvite_a_pending = 0;
    if (s->rtp_b.port != 0 && !s->torn_down && s->call_a == call_a)
        s->resume_sdp_a_pending = 1;
    else {
        s->resume_sdp_a_pending = 0;
        PJ_LOG(1, (THIS_FILE,
                   "[HOLD/M2] resume SDP rewrite not armed (rtp_b=%s:%d)",
                   s->rtp_b.ip, s->rtp_b.port));
    }
    CC_SESSION_UNLOCK(s);

    pjsua_msg_data_init(&msg_data);
    if (a_update_ok)
        status = pjsua_call_update(call_a, 0, &msg_data);
    else
        status = pjsua_call_reinvite(call_a, 0, &msg_data);

    if (status != PJ_SUCCESS) {
        CC_SESSION_LOCK(s);
        s->resume_sdp_a_pending = 0;
        CC_SESSION_UNLOCK(s);
        PJ_LOG(1, (THIS_FILE, "[HOLD/M2] resume %s to A failed: %d — attempting bypass anyway",
                   a_update_ok ? "UPDATE" : "re-INVITE", status));
        goto resume_bypass_a;
    }

    /* Poll for A's media state to return ACTIVE (resume confirmed) */
    {
        int waited = 0, active = 0;
        while (waited < 5000) {
            cc_sleep_ms(100);
            waited += 100;
            CC_SESSION_LOCK(s);
            torn = s->torn_down || s->call_a != call_a;
            CC_SESSION_UNLOCK(s);
            if (torn) goto resume_a_done;
            pjsua_call_info ci_a;
            if (pjsua_call_get_info(call_a, &ci_a) == PJ_SUCCESS &&
                ci_a.media_cnt > 0 &&
                ci_a.media[0].status == PJSUA_CALL_MEDIA_ACTIVE)
            {
                active = 1;
                break;
            }
        }
        if (!active)
            PJ_LOG(2, (THIS_FILE, "[HOLD/M2] A resume not confirmed in 5s — continuing"));
        else
            PJ_LOG(3, (THIS_FILE, "[HOLD/M2] A confirmed resume"));
    }

resume_bypass_a:
    /* Reset bypass flags and re-run bypass UPDATEs to restore direct RTP */
    CC_SESSION_LOCK(s);
    torn = s->torn_down || s->call_a != call_a || s->call_b != call_b;
    if (!torn) {
        s->hold_update_a_sent    = 0;
        s->hold_update_a_acked   = 0;
        s->media_bypassed        = 0;
        s->update_a_sent         = 0;
        s->update_b_sent         = 0;
        s->update_a_acked        = 0;
        s->update_b_acked        = 0;
        s->update_b_retry_pending  = 0;
        s->update_a_retry_pending  = 0;
        s->update_ack_watchdog_started = 0;
        s->b_reinvite_active     = 0;
    }
    CC_SESSION_UNLOCK(s);

    if (!torn) {
        PJ_LOG(3, (THIS_FILE, "[HOLD/M2] resume: re-running bypass UPDATEs"));
        leg_a_send_update_bypass(call_a, s);
        leg_b_send_update_bypass(call_b, s);
    }

resume_a_done:
    CC_SESSION_LOCK(s);
    s->resume_propagate_a_pending = 0;
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

/* CC_EV_UPDATE_ACK_WATCHDOG — was update_ack_watchdog_thread.
 * CC_UPDATE_ACK_TIMEOUT_MS/CC_UPDATE_ACK_POLL_MS now defined earlier
 * (above cc_maybe_arm_update_ack_watchdog) since they're needed there too. */

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

    CC_SESSION_LOCK(s);
    s->bypass_rtp_watchdog_started = 0;
    CC_SESSION_UNLOCK(s);
}

/* ── Central dispatcher ──────────────────────────────────────────────────── */

static void process_event(cc_event_t *ev)
{
    /* Serial guard: reject any event whose session has been replaced.
     * This catches call_id slot reuse where a new session was created
     * on the same slot before a stale timer fired. */
    if (ev->session && ev->session_serial != 0) {
        unsigned current_serial;
        CC_SESSION_LOCK(ev->session);
        current_serial = ev->session->session_serial;
        CC_SESSION_UNLOCK(ev->session);
        if (current_serial != ev->session_serial) {
            PJ_LOG(3, (THIS_FILE,
                       "[WORKER] stale event type=%d dropped — serial mismatch "
                       "ev=%u session=%u",
                       ev->type, ev->session_serial, current_serial));
            cc_session_release_reason(ev->session, ev->reason);
            return;
        }
    }

    switch (ev->type) {
    case CC_EV_ORIGINATE_B:         ev_originate_b(ev);              break;
    case CC_EV_WAV_HANGUP_A:        ev_wav_hangup_a(ev);             break;
    case CC_EV_HANGUP_A_ONLY:       ev_hangup_a_only(ev);            break;
    case CC_EV_MCA_WAIT:            ev_mca_wait(ev);                 break;
    case CC_EV_MCA_STOP_PROMPT:     ev_mca_stop_prompt(ev);          break;
    case CC_EV_MCA_RESOLVE:         ev_mca_resolve(ev);              break;
    case CC_EV_B_PROMPT_START:      ev_b_prompt_start(ev);           break;
    case CC_EV_B_PROMPT_DONE:       ev_b_prompt_done(ev);            break;
    case CC_EV_ACCEPT_TRANSITION:   ev_accept_transition(ev);        break;
    case CC_EV_ACCEPT_BRIDGE_WAIT:  ev_accept_bridge_wait(ev);       break;
    case CC_EV_ACCEPT_BRIDGE:       ev_accept_bridge(ev);            break;
    case CC_EV_RING_TIMER:          ev_timer(ev, 1);                 break;
    case CC_EV_DTMF_TIMER:          ev_timer(ev, 0);                 break;
    case CC_EV_UPDATE_A_BYPASS:      ev_update_a_bypass(ev);          break;
    case CC_EV_UPDATE_B_BYPASS:      ev_update_b_bypass(ev);          break;
    case CC_EV_REINVITE_A_BYPASS:    ev_reinvite_a_bypass(ev);        break;
    case CC_EV_REINVITE_B_BYPASS:    ev_reinvite_b_bypass(ev);        break;
    case CC_EV_HOLD_PROPAGATE_B:     ev_hold_propagate_b(ev);         break;
    case CC_EV_RESUME_PROPAGATE_B:   ev_resume_propagate_b(ev);       break;
    case CC_EV_HOLD_PROPAGATE_A:     ev_hold_propagate_a(ev);         break;
    case CC_EV_RESUME_PROPAGATE_A:   ev_resume_propagate_a(ev);       break;
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
    if (ev->session) {
        cc_session_t    *s    = ev->session;
        pthread_mutex_t *lock = s->lock;  /* heap-allocated; outlives pool */

        /* Clear a_treatment_running and run maybe_finalize while the worker
         * ref still keeps s alive — safe to touch s->lock here. */
        /* CC_EV_WAV_HANGUP_A must NOT clear a_treatment_running here:
         * it posts CC_EV_HANGUP_A_ONLY which is the real terminal event.
         * Clearing here would allow maybe_finalize to destroy the session
         * while HANGUP_A_ONLY is still queued, causing a destroyed-mutex
         * crash at line 1447 when HANGUP_A_ONLY reaches process_event.
         * Only the true terminal treatment events clear the flag. */
        if (ev->type == CC_EV_HANGUP_A_ONLY) {
            pthread_mutex_lock(lock);
            s->a_treatment_running = 0;
            pthread_mutex_unlock(lock);
        }

        cc_session_maybe_finalize(s);

        /* release_reason drops the worker ref.  If ref_count hits 0 it calls
         * cc_session_destroy which calls pthread_mutex_destroy(lock)+free(lock).
         * After this point s and lock must not be touched. */
        cc_session_release_reason(s, ev->reason);
    }
}
