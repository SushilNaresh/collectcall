#ifndef CC_RTPENGINE_H
#define CC_RTPENGINE_H

#include <pjsua-lib/pjsua.h>
#include "session.h"

/*
 * rtpengine.h — ng/bencode control client for Sipwise RTPengine
 *
 * PJSIP stays on SIP + SDP. Conversation RTP is relayed by RTPengine
 * (userspace for the first packets, then xt_RTPENGINE kernel forwarding).
 *
 * ng UDP: one socket per calling thread (no global send/recv lock) so
 * concurrent offer/answer scale with worker count.
 *
 * Commands used:
 *   offer  — A-leg INVITE / re-INVITE / UPDATE offer SDP
 *   answer — B-leg 183/200 / re-INVITE / UPDATE answer SDP
 *   play media / stop media — prompts
 *   query  — per-call packet stats (call end / optional)
 *   delete — BYE / session teardown
 *
 * Note: block media during collect breaks detect-DTMF on some RE builds;
 * helpers remain but are not used on the collect path.
 */

/** Max SDP body size for ng offer/answer and async A-answer copies. */
#define CC_RTPENGINE_SDP_MAX  16384

int  cc_rtpengine_enabled(void);
int  cc_rtpengine_ready(const cc_session_t *s);

pj_status_t cc_rtpengine_init(void);
void        cc_rtpengine_shutdown(void);

/**
 * Copy application/sdp from an rx_data into buf (NUL-terminated).
 * Returns byte length on success, -1 on failure.
 */
int cc_rtpengine_copy_rdata_sdp(pjsip_rx_data *rdata, char *buf, size_t buflen);

/** Allocate call-id / from-tag / to-tag on the session if missing. */
void cc_rtpengine_prepare_session(cc_session_t *s);

/** offer: remote SDP from A (or A re-INVITE). Fills rtpengine_ep_b. */
pj_status_t cc_rtpengine_offer(cc_session_t *s, const char *sdp);

/** answer: remote SDP from B. Fills rtpengine_ep_a. */
pj_status_t cc_rtpengine_answer(cc_session_t *s, const char *sdp);

pj_status_t cc_rtpengine_offer_from_sdp(cc_session_t *s,
                                        const pjmedia_sdp_session *sdp);
pj_status_t cc_rtpengine_answer_from_sdp(cc_session_t *s,
                                         const pjmedia_sdp_session *sdp);
pj_status_t cc_rtpengine_offer_from_rdata(cc_session_t *s, pjsip_rx_data *rdata);
pj_status_t cc_rtpengine_answer_from_rdata(cc_session_t *s, pjsip_rx_data *rdata);

/** Pull stats and log them. Safe no-op if never offered. */
void cc_rtpengine_query(cc_session_t *s);

/** Tear down the RTPengine call. Idempotent. */
void cc_rtpengine_delete(cc_session_t *s);

/**
 * Rewrite target for this B2BUA leg when RTPengine is in control.
 * for_a_leg=1 → address A should send RTP to (answer SDP).
 * for_a_leg=0 → address B should send RTP to (offer SDP).
 * Returns 1 and fills out when RTPengine mode has a valid endpoint.
 */
int cc_rtpengine_sdp_target(const cc_session_t *s, int for_a_leg, cc_rtp_ep_t *out);

/** True if last answer changed A-facing IP:port (consume/clear). */
int cc_rtpengine_consume_a_ep_changed(cc_session_t *s);

/**
 * Play a WAV toward A (for_a_leg=1) or B (0) via RTPengine.
 * loop: repeat many times (MOH / dial tone). duration_ms may be NULL.
 */
pj_status_t cc_rtpengine_play(cc_session_t *s, int for_a_leg,
                              const char *wav_path, int loop, int *duration_ms);

/** Stop playback toward that participant. */
void cc_rtpengine_stop_play(cc_session_t *s, int for_a_leg);

/**
 * Stop forwarding A's media (directional, A party only) so the B collect
 * announcement is the only SSRC reaching B. B -> RE is untouched, so B's
 * RFC2833 still reaches DTMF-log-dest. play-media is unaffected.
 *
 * Must be paired with cc_rtpengine_unblock_media() on every collect-phase
 * exit — A's own DTMF is needed for the MCA wait that follows a failed B leg.
 */
void cc_rtpengine_block_media(cc_session_t *s);

/** Resume A's media forwarding. Idempotent. */
void cc_rtpengine_unblock_media(cc_session_t *s);

#endif /* CC_RTPENGINE_H */
