#ifndef CC_WORKER_H
#define CC_WORKER_H

/*
 * worker.h — Fixed worker thread pool + event queue
 *
 * 64 worker threads (CC_WORKER_POOL_SIZE) share a single MPSC ring queue
 * (CC_WORKER_QUEUE_SIZE slots). All blocking work (sleep loops, RTP polls,
 * WAV waits) runs inside a worker. PJSUA callbacks post events and return
 * immediately.
 *
 * Thread count: fixed 64 regardless of session count.
 * Previously: up to 11 threads × 4096 sessions = 45,056 threads.
 */

#include "session.h"
#include <pjsua-lib/pjsua.h>
#include <stdint.h>

#define CC_WORKER_POOL_SIZE   64     /* fixed worker threads */
#define CC_WORKER_QUEUE_SIZE  16384  /* MPSC ring slots      */

/* ── Event types ─────────────────────────────────────────────────────────── */
typedef enum {
    CC_EV_NONE = 0,

    /* B-leg origination (was cc_originate_b_thread) */
    CC_EV_ORIGINATE_B,

    /* A-leg: play WAV then hangup (was wav_then_hangup_thread) */
    CC_EV_WAV_HANGUP_A,
    /* play treatment WAV (non-blocking) then hangup via timer */
    CC_EV_HANGUP_A_ONLY,

    /* A-leg: MCA wait loop (was mca_wait_thread) */
    CC_EV_MCA_WAIT,

    /* B-leg: start collect prompt after CONFIRMED+RTP ready (was cc_b_prompt_start_thread) */
    CC_EV_B_PROMPT_START,

    /* B-leg: signal b_collect_done after prompt duration (was cc_b_prompt_done_thread) */
    CC_EV_B_PROMPT_DONE,

    /* B-leg: accept transition phases (continuation pattern — no worker sleep) */
    CC_EV_ACCEPT_TRANSITION,      /* phase 1: wait b_collect_done, start 4.1.wav */
    CC_EV_ACCEPT_BRIDGE_WAIT,     /* phase 2: after 4.1.wav, wait remaining free period */
    CC_EV_ACCEPT_BRIDGE,          /* phase 3: stop prompts, bridge/UPDATE */

    /* Ring / DTMF timers (was timer_thread) */
    CC_EV_RING_TIMER,
    CC_EV_DTMF_TIMER,

    /* UPDATE 491 retry (was update_a/b_retry_thread) */
    CC_EV_UPDATE_A_RETRY,
    CC_EV_UPDATE_B_RETRY,

    /* UPDATE ack watchdog (was update_ack_watchdog_thread) */
    CC_EV_UPDATE_ACK_WATCHDOG,

    /* RTP bypass watchdog (was cc_bypass_rtp_watchdog_thread) */
    CC_EV_BYPASS_RTP_WATCHDOG,

    /* Async validation callback — fired by validation_async dispatcher */
    CC_EV_VASYNC_CB,
} cc_ev_type_t;

/* ── Event payload ───────────────────────────────────────────────────────── */
/*
 * Fixed 56-byte payload — fits in one 64-byte cache line with the type tag.
 * All fields are optional; unused ones are zero.
 */
typedef struct {
    cc_ev_type_t    type;           /* discriminator                        */
    cc_session_t   *session;        /* always set                           */
    pjsua_call_id   call_a;         /* PJSUA_INVALID_ID if not applicable   */
    pjsua_call_id   call_b;         /* PJSUA_INVALID_ID if not applicable   */
    int             delay_ms;       /* free-period delay for accept         */
    int             timeout_sec;    /* ring/dtmf timer duration             */
    int             prompt_ms;      /* WAV duration for b_prompt_done       */
    const char     *wav_path;       /* static string — no ownership         */
    int             sip_code;       /* hangup SIP status code               */
    int             prompt_tag;     /* cc_prompt_tag_t cast to int          */
    int             wait_ms;        /* WAV_HANGUP_A: waiting-prompt deferral sleep ms */
    pjsua_player_id player_a;       /* WAV_HANGUP_A: waiting-prompt player to stop */
    char            reason[32];     /* session release reason tag           */
    void           *data;           /* CC_EV_VASYNC_CB: vasync_cb_event_t * */
} cc_event_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/** Start the single worker thread. Call once from main() after pjsua_start(). */
int  cc_worker_start(void);

/** Stop the worker thread. Call before pjsua_destroy(). */
void cc_worker_stop(void);

/**
 * Post an event to the worker queue.
 * Acquires a session ref with reason ev->reason before enqueuing.
 * Returns 0 on success, -1 if queue full or session already torn down.
 */
int  cc_worker_post(cc_event_t *ev);

/**
 * Post an event to fire after wait_ms milliseconds.
 * Uses a single timer thread (min-heap); does not block the caller.
 * Returns 0 on success, -1 on failure.
 */
int  cc_worker_post_delayed(cc_event_t *ev, int wait_ms);

#endif /* CC_WORKER_H */
