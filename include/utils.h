#ifndef CC_UTILS_H
#define CC_UTILS_H

#include <pjsua-lib/pjsua.h>
#include <pjmedia/sdp.h>
#include "session.h"

typedef struct {
    char dialed_digits[128];
    char matched_prefix[32];
    char sponsor_raw[128];
    int  prefix_matched;
    int  already_stripped;
} cc_collect_number_t;

/*
 * utils.h — SDP, SIP header, URI and media helpers
 */

/* ── URI / number ───────────────────────────────────────────────────────── */

/**
 * Return the effective collect-call prefix.
 * CC_COLLECT_PREFIX from the environment overrides the compile-time default
 * when it contains only digits and fits the configured prefix buffer.
 */
const char *cc_collect_prefix(void);

/** Return PJ_TRUE when the effective prefix came from the environment. */
pj_bool_t cc_collect_prefix_is_env_override(void);

/** Extract the user part from a raw URI/header value without normalizing it. */
pj_status_t cc_extract_uri_user(const char *identity,
                                char *user,
                                pj_size_t user_len);

/** Extract Request-URI text and user from the raw incoming INVITE. */
pj_status_t cc_extract_request_uri_user(pjsip_rx_data *rdata,
                                        char *raw_uri,
                                        pj_size_t raw_uri_len,
                                        char *user,
                                        pj_size_t user_len);

/** Extract To header user from the raw incoming INVITE. */
pj_status_t cc_extract_to_header_user(pjsip_rx_data *rdata,
                                      char *user,
                                      pj_size_t user_len);

/** Extract user from the Diversion header (call-forward scenarios). */
pj_status_t cc_extract_diversion_user(pjsip_msg *msg,
                                      char *user,
                                      pj_size_t user_len);

/** Extract user from a pj_str_t URI-like value. */
pj_status_t cc_extract_pj_uri_user(const pj_str_t *uri,
                                   char *user,
                                   pj_size_t user_len);

/** Apply configured collect-call prefix list and prefix mode. */
pj_status_t cc_split_collect_number(const char *dialed_raw,
                                    cc_collect_number_t *result);

/** Normalize phone/user input into an MSISDN using CC_DEFAULT_COUNTRY_CODE. */
pj_status_t cc_normalize_msisdn(const char *input,
                                char *normalized,
                                pj_size_t normalized_len);

/**
 * Strip the collect prefix from a Request-URI user part.
 * Input:  "sip:<prefix>9876543@ims.operator.net"
 * Output: fills b_number (e.g. "9876543"), returns PJ_SUCCESS or PJ_ENOTFOUND
 */
pj_status_t cc_extract_b_number(const pj_str_t *local_uri,
                                 char *b_number, pj_size_t b_number_len);

/**
 * Build a SIP Request-URI for B (uses SBC host:port).
 * Output: "sip:9876543@sbc_host:sbc_port" in buf.
 */
pj_status_t cc_build_b_uri(const char *b_number,
                           char *buf,
                           pj_size_t buf_len);

/**
 * Build a SIP From URI for the B-leg INVITE (uses local host:port).
 * Output: "sip:9876543@local_host:local_port;user=phone" in buf.
 */
pj_status_t cc_build_b_from_uri(const char *b_number,
                                char *buf,
                                pj_size_t buf_len);

/** Extract the user part from a SIP/SIPS/TEL identity string. */
pj_status_t cc_extract_identity_user(const char *identity,
                                     char *user,
                                     pj_size_t user_len);

/** Extract icid-value from a P-Charging-Vector header value. */
pj_status_t cc_extract_pcv_icid(const char *pcv,
                                char *icid,
                                pj_size_t icid_len);

/** Format epoch seconds as fixed Nigeria local time (UTC+01:00). */
pj_status_t cc_format_nigeria_time(time_t timestamp,
                                   char *buf,
                                   size_t buf_len);

/* ── SDP helpers ────────────────────────────────────────────────────────── */

/**
 * Extract the RTP ip:port from a parsed SDP session.
 * Returns PJ_SUCCESS and fills ep on success.
 */
pj_status_t cc_sdp_extract_rtp(const pjmedia_sdp_session *sdp,
                                cc_rtp_ep_t *ep);

/**
 * Detect bypass mode from the SDP connection address.
 * Compares c= IP against CC_MGW_SUBNETS.
 */
cc_bypass_mode_t cc_sdp_detect_bypass(const pjmedia_sdp_session *sdp);

/**
 * Build a new SDP session (cloned from orig) with the connection address
 * and audio port replaced by new_ip:new_port.
 * Used for DIRECT hairpin UPDATE SDP.
 * Returns a pool-allocated pjmedia_sdp_session on success, NULL on error.
 */
pjmedia_sdp_session *cc_sdp_rewrite_rtp(pj_pool_t *pool,
                                          const pjmedia_sdp_session *orig,
                                          const char *new_ip, int new_port);
/**
 * Log local/remote RTP transport info for a call.
 * Returns PJ_SUCCESS if info was read.
 */
pj_status_t cc_log_call_rtp_info(pjsua_call_id call_id, const char *tag);

/**
 * Read learned remote RTP source address for a call into ep.
 * Works after RTP packets have started flowing.
 */
pj_status_t cc_get_call_remote_rtp(pjsua_call_id call_id, cc_rtp_ep_t *ep);

/* ── SIP header helpers ─────────────────────────────────────────────────── */

/**
 * Capture forwarded operator headers from a received SIP message into
 * the session's fwd_hdrs array.
 * msg is the pjsip_rx_data->msg_info.msg from the incoming INVITE.
 */
void cc_capture_fwd_headers(pjsip_msg *msg, cc_session_t *session);

/**
 * Append all forwarded headers from the session to a pjsip_tx_data
 * (used when sending the outbound INVITE to B).
 */
void cc_append_fwd_headers(pjsip_tx_data *tdata, const cc_session_t *session);

/**
 * Append a single name:value header to a tx_data.
 */
void cc_append_header(pjsip_tx_data *tdata,
                      pj_pool_t *pool,
                      const char *name,
                      const char *value);

/* ── Media / conf helpers ───────────────────────────────────────────────── */

/**
 * Create a looping (or one-shot) WAV file player and connect it to the
 * given call's conference port.
 * Returns the player_id (pjsua_player_id) on success, PJSUA_INVALID_ID on error.
 */
pjsua_player_id cc_start_wav(pjsua_call_id call_id,
                              const char *wav_path,
                              pj_bool_t loop);

/**
 * Stop and destroy a WAV player, disconnecting it from the call.
 */
void cc_stop_wav(pjsua_player_id player_id, pjsua_call_id call_id);

/**
 * Disconnect a call's conf slot from the master mix (slot 0) in both
 * directions, preventing audio leaking between unrelated calls.
 */
void cc_isolate_call_from_master(pjsua_call_id call_id);

/**
 * Bridge two calls — connect their conference ports bidirectionally.
 */
pj_status_t cc_bridge_calls(pjsua_call_id call_a, pjsua_call_id call_b);


/**
 * Unbridge two calls — disconnect their conference ports bidirectionally.
 */
pj_status_t cc_unbridge_calls(pjsua_call_id call_a, pjsua_call_id call_b);


/* ── Misc ───────────────────────────────────────────────────────────────── */

/** Mark and log the final collect-call end status once. */
void cc_session_mark_end(cc_session_t *session,
                         const char *status,
                         const char *reason);

/** Log final collect-call end fields once using status/reason already in session. */
void cc_session_log_end(cc_session_t *session);

/** Hangup a call safely, ignoring all errors. */
pj_status_t cc_safe_hangup(pjsua_call_id call_id, pjsip_status_code code);

/** Portable millisecond sleep. */
void cc_sleep_ms(int ms);

#endif /* CC_UTILS_H */
