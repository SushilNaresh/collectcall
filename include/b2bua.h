#ifndef CC_B2BUA_H
#define CC_B2BUA_H

#include <pjsua-lib/pjsua.h>
#include "session.h"

/*
 * b2bua.h — Core B2BUA logic
 *
 * cc_on_incoming_call()  is the top-level handler registered in
 *                         pjsua_callback.on_incoming_call.
 *
 * cc_originate_b()       spawns the outbound leg to B in a thread.
 */

/**
 * Global pjsua incoming call callback.
 * Validates the collect prefix, answers A-leg, then B-leg starts after A ACK.
 */
void cc_on_incoming_call(pjsua_acc_id acc_id,
                          pjsua_call_id call_id,
                          pjsip_rx_data *rdata);

/**
 * Global pjsua call-state callback.
 * Dispatches to leg_a or leg_b handler based on call user_data.
 */
void cc_on_call_state(pjsua_call_id call_id, pjsip_event *e);

/**
 * Global pjsua call-media-state callback.
 */
void cc_on_call_media_state(pjsua_call_id call_id);

/**
 * Called by PJSUA when local SDP is created.
 * Used to rewrite UPDATE SDP for RTP bypass.
 */
void cc_on_call_sdp_created(pjsua_call_id call_id,
                            pjmedia_sdp_session *sdp,
                            pj_pool_t *pool,
                            const pjmedia_sdp_session *rem_sdp);

/**
 * Global pjsua DTMF callback.
 * Legacy callback handles RFC2833-style digit events.
 */
void cc_on_dtmf_digit(pjsua_call_id call_id, int digit);

/**
 * Global pjsua DTMF callback with method information.
 * Used for SIP INFO DTMF and RFC2833 DTMF when supported by PJSUA.
 */
void cc_on_dtmf_digit2(pjsua_call_id call_id,
                       const pjsua_dtmf_info *info);


/**
 * Thread arg for B-leg origination.
 */
typedef struct {
    cc_session_t   *session;
    char            b_dial_number[128];
    char            b_from_user[128];
    char            service_key[64];
    char            service_key_mode[32];
    pjsua_acc_id    acc_id;
} cc_originate_arg_t;

/**
 * Originate the outbound call to B.
 * Runs in a detached pthread.
 */
void *cc_originate_b_thread(void *arg);

/**
 * Start outbound B-leg only after A-leg is confirmed/ACKed.
 * Safe to call multiple times; it starts B-leg once only.
 * Returns the A-leg call ID when validation rejection must be hung up after
 * the current call-state callback releases its session reference.
 */
pjsua_call_id cc_start_b_leg_after_a_confirmed(cc_session_t *session);

#endif /* CC_B2BUA_H */
