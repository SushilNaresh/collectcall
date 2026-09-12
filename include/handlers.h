#ifndef CC_HANDLERS_H
#define CC_HANDLERS_H

#include <pjsua-lib/pjsua.h>
#include "session.h"
#include "prompt_mapping.h"

/*
 * handlers.h — Per-leg call logic declarations
 *
 * In the PJSUA C API, callbacks are global functions registered in
 * pjsua_callback.  These per-leg handlers are called from those globals
 * after the session pointer is resolved from the call's user_data.
 */

/* ── Leg-A handlers (inbound / calling party) ───────────────────────────── */

/**
 * Called when A's call state changes.
 * Handles DISCONNECTED to tear down the session.
 * Returns a deferred A-leg hangup ID, or PJSUA_INVALID_ID.
 */
pjsua_call_id leg_a_on_call_state(pjsua_call_id call_id,
                                  cc_session_t *session);

/**
 * Called when A's media state becomes ACTIVE.
 * Starts the waiting WAV once (no loop), captures A's RTP endpoint.
 */
void leg_a_on_media_state(pjsua_call_id call_id, cc_session_t *session);

/**
 * Called when A-leg receives DTMF while mca_waiting is set.
 * Digit 1 triggers MCA API + MCA_SENT prompt.
 */
void leg_a_on_dtmf_mca(pjsua_call_id call_id, int digit, cc_session_t *session);

/**
 * Send 200 OK to A — called only after B accepts.
 * CDR billing clock starts here.
 */
void leg_a_answer_200(pjsua_call_id call_id);

/**
 * Play the rejection WAV to A, then hang up.
 * Runs in a detached thread so it does not block the callback path.
 */
void leg_a_play_rejected_then_hangup(cc_session_t *session);

/**
 * Play the unavailable WAV to A, then hang up.
 */
void leg_a_play_unavailable_then_hangup(cc_session_t *session);

/**
 * Play specified prompt to A once (no loop). DTMF wait is a delayed timer
 * (b_dtmf_timeout_sec), not a sleeping worker. Digit 1 → MCA_SENT then hangup;
 * other digit → MCA_NOT_SENT then hangup; no digit → hangup NoMCA.
 */
void leg_a_play_mca_wait(cc_session_t *session, cc_prompt_tag_t prompt_tag);

/**
 * Play a specific prompt to A, then hang up with the given SIP code.
 */
void leg_a_play_prompt_then_hangup(cc_session_t *session,
                                   cc_prompt_tag_t tag,
                                   pjsip_status_code code);

/**
 * Send SIP UPDATE on A's leg to exit the RTP path.
 * Uses bypass_mode from session to choose DIRECT or MGW strategy.
 */
void leg_a_send_update_bypass(pjsua_call_id call_id, cc_session_t *session);

/**
 * After B-leg answer changes RTPengine A-facing ports: re-INVITE A and
 * restart any in-progress A-leg play-media (waiting / MOH).
 */
void leg_a_on_rtpengine_a_ep_changed(cc_session_t *session);

/**
 * After async ng offer completes (or A CONFIRMED with ports ready):
 * re-INVITE to advertise RTPengine A-facing ports and start waiting prompt.
 */
void leg_a_advertise_rtpengine_if_ready(cc_session_t *session);

/**
 * Send SIP re-INVITE on A's leg to test RTP bypass.
 */
void leg_a_send_reinvite_bypass(cc_session_t *session);

/* ── Leg-B handlers (outbound / called party) ───────────────────────────── */

/**
 * Called when B's call state changes.
 * Handles DISCONNECTED (before accept) → rejection treatment to A.
 */
void leg_b_on_call_state(pjsua_call_id call_id, cc_session_t *session);

/**
 * Called when B's media becomes ACTIVE.
 * Plays the collect prompt, starts DTMF accept timer.
 */
void leg_b_on_media_state(pjsua_call_id call_id, cc_session_t *session);

/**
 * Called when a DTMF digit is received on B's leg.
 * Routes to on_accept() or on_reject() logic.
 */
void leg_b_on_dtmf(pjsua_call_id call_id, int digit, cc_session_t *session);

/**
 * Send SIP UPDATE on B's leg to exit the RTP path.
 */
void leg_b_send_update_bypass(pjsua_call_id call_id, cc_session_t *session);

/**
 * Send SIP re-INVITE on B's leg to test RTP bypass.
 */
void leg_b_send_reinvite_bypass(cc_session_t *session);

/**
 * Start the ring timeout watchdog for B's leg.
 * If B does not answer within CC_B_RING_TIMEOUT_SEC, play
 * NOT_AVAILABLE_TO_PAY once then hang up.
 */
void leg_b_start_ring_timer(cc_session_t *session);

/**
 * Start the DTMF accept timer for B's leg.
 * If B does not press a key within CC_B_DTMF_TIMEOUT_SEC, treat as reject.
 */
void leg_b_start_dtmf_timer(cc_session_t *session);

/**
 * Called by worker on DTMF timer expiry — triggers ELIGIBILITY_TIMEOUT reject.
 */
void leg_b_on_dtmf_timeout(pjsua_call_id call_b, cc_session_t *session);

/* ── SDP helpers ────────────────────────────────────────────────────────── */

/**
 * Strip every audio payload type except PCMA/8000 and telephone-event/8000
 * (PCMA is forced first, and inserted if the peer omitted it).
 *
 * Applied both to the SDP we put on the wire and to the SDP handed to
 * RTPengine: if RTPengine still sees the peer's full list it will pick that
 * peer's first codec for its own direction and transcode prompts into it.
 */
void cc_sdp_restrict_audio_pcma_te(pj_pool_t *pool,
                                   pjmedia_sdp_session *sdp,
                                   const char *tag);

#endif /* CC_HANDLERS_H */
