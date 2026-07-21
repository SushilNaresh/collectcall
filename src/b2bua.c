/*
 * b2bua.c — Global PJSUA callbacks + B-leg origination thread
 *
 * PJSUA uses a single global pjsua_callback struct.  Each callback
 * resolves which session the call belongs to via call user_data, then
 * dispatches to the appropriate leg handler.
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
#include "api_mapping.h"
#include "runtime_config.h"

#include <pjsua-lib/pjsua.h>
#include <pjsip/sip_msg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>

#define THIS_FILE "b2bua.c"
#define CC_SDP_SESSION_NAME "ccmedia"

static const char *validation_end_reason(int status)
{
    switch (status) {
    case CC_VALIDATION_CALLER_BLACKLISTED:
        return "CALLER_BLACKLISTED";
    case CC_VALIDATION_SPONSOR_BALANCE_FAIL:
        return "SPONSOR_BALANCE_FAIL";
    case CC_VALIDATION_SPONSOR_DND_ACTIVE:
        return "SPONSOR_DND_ACTIVE";
    case CC_VALIDATION_SPONSOR_ROAMING:
        return "SPONSOR_ROAMING";
    case CC_VALIDATION_API_FAILURE:
        return "API_FAILURE";
    default:
        return "SYSTEM_ERROR";
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

    status = pjsua_call_get_info(call_id, &ci);
    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] incoming call info failed call=%d status=%d",
                   call_id, status));
        pjsua_call_answer(call_id, PJSIP_SC_SERVICE_UNAVAILABLE, NULL, NULL);
        return;
    }
    PJ_LOG(3, (THIS_FILE, "Incoming call: from=%.*s to=%.*s",
               (int)ci.remote_info.slen, ci.remote_info.ptr,
               (int)ci.local_info.slen,  ci.local_info.ptr));

    /* Extract B's number (strips collect prefix) */
    status = cc_extract_request_uri_user(rdata,
                                         raw_ruri,
                                         sizeof(raw_ruri),
                                         ruri_user,
                                         sizeof(ruri_user));
    PJ_LOG(3, (THIS_FILE,
               "[INCOMING] raw_ruri=%s",
               raw_ruri[0] ? raw_ruri : "<unavailable>"));
    if (status == PJ_SUCCESS) {
        snprintf(dialed_raw, sizeof(dialed_raw), "%s", ruri_user);
        snprintf(dialed_source, sizeof(dialed_source), "%s", "Request-URI");
        PJ_LOG(3, (THIS_FILE,
                   "[INCOMING] ruri_user=%s",
                   ruri_user));
    } else {
        PJ_LOG(3, (THIS_FILE,
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

    PJ_LOG(3, (THIS_FILE,
               "[DIALED] raw=%s source=%s",
               dialed_raw,
               dialed_source));

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

    PJ_LOG(3, (THIS_FILE,
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

    PJ_LOG(3, (THIS_FILE,
               "[B-PARTY] raw=%s normalized=%s",
               collect_number.sponsor_raw,
               sponsor_normalized));
    /* Create session */
    session = cc_session_create(pjsua_get_pool_factory());
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
        PJ_LOG(3, (THIS_FILE,
                   "[CALLER] raw=%s normalized=%s source=%s",
                   session->caller_msisdn_raw,
                   session->caller_msisdn,
                   session->caller_msisdn_source[0] ?
                       session->caller_msisdn_source : "UNKNOWN"));
        PJ_LOG(3, (THIS_FILE,
                   "[INITIATE-API] callerMsisdn=%s",
                   session->caller_msisdn));
    }

    PJ_LOG(3, (THIS_FILE,
               "[END-API] ICID=%s",
               session->icid));

    /* Attach session as call user_data so callbacks can find it */
    pjsua_call_set_user_data(call_id, session);

    PJ_LOG(3, (THIS_FILE, "Collect call: A=%.*s B=%s",
               (int)ci.remote_info.slen,
               ci.remote_info.ptr,
               sponsor_normalized));

    /*
     * Expected customer flow:
     * First complete A-leg: INVITE -> 200 OK -> ACK.
     * B-leg must start only after A-leg reaches CONFIRMED state.
     */
    pjsua_call_setting_default(&cs);

    status = pjsua_call_answer2(call_id, &cs,
                                 PJSIP_SC_OK, NULL, NULL);
    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE, "[ERROR] A-leg 200 OK failed: %d", status));
        cc_session_mark_end(session, "FAILED", "SYSTEM_ERROR");
        CC_SESSION_LOCK(session);
        session->torn_down = 1;
        CC_SESSION_UNLOCK(session);
        cc_session_invalidate_a(session, call_id);
        cc_session_maybe_finalize(session);
        return;
    }

    PJ_LOG(3, (THIS_FILE, "A-leg 200 OK sent; waiting for ACK/CONFIRMED before starting B-leg"));
}

/* ── Start B leg after A-leg ACK/CONFIRMED ───────────────────────────────── */

pjsua_call_id cc_start_b_leg_after_a_confirmed(cc_session_t *session)
{
    cc_originate_arg_t *arg;
    char original_b[64];
    pthread_t t;
    int rc;

    if (!session)
        return PJSUA_INVALID_ID;

    CC_SESSION_LOCK(session);

    if (session->torn_down || session->b_leg_started) {
        CC_SESSION_UNLOCK(session);
        return PJSUA_INVALID_ID;
    }

    session->b_leg_started = 1;

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

    /*
     * UDP validation trigger:
     * status 0 = allow call
     * status 1/2/3 or timeout/error = reject/disconnect
     */
    {
        char call_id[128];
        char caller_msisdn[64];
        char time_str[64];
        cc_validation_result_t validation_result;
        time_t now;
        int vstatus;

        memset(&validation_result, 0, sizeof(validation_result));

        now = time(NULL);
        if (cc_format_nigeria_time(now,
                                   time_str,
                                   sizeof(time_str)) != PJ_SUCCESS)
        {
            PJ_LOG(1, (THIS_FILE,
                       "[API-TIME] failed to format initiate timestamp"));
            time_str[0] = '\0';
        }
        PJ_LOG(3, (THIS_FILE,
                   "[API-TIME] timestamp=%s",
                   time_str));

        CC_SESSION_LOCK(session);
        snprintf(call_id, sizeof(call_id), "%s", session->call_id);
        snprintf(caller_msisdn,
                 sizeof(caller_msisdn),
                 "%s",
                 session->caller_msisdn);
        CC_SESSION_UNLOCK(session);

        PJ_LOG(3, (THIS_FILE,
                   "[INITIATE-API] callerMsisdn=%s sponsorMsisdn=%s callId=%s source=%s timestamp=%s",
                   caller_msisdn,
                   original_b,
                   call_id,
                   CC_INITIATE_SOURCE,
                   time_str));
        PJ_LOG(3, (THIS_FILE,
                   "[API] initiate caller=%s sponsor=%s",
                   caller_msisdn,
                   original_b));

        vstatus = cc_udp_validate_call(caller_msisdn,
                                       original_b,
                                       call_id,
                                       CC_INITIATE_SOURCE,
                                       time_str,
                                       &validation_result);

        /*
         * TODO(load): validation remains synchronous and may block this
         * callback path for CC_VALIDATION_TIMEOUT_MS.
         */
        CC_SESSION_LOCK(session);
        if (session->torn_down ||
            session->call_a == PJSUA_INVALID_ID ||
            session->final_cleanup_started)
        {
            CC_SESSION_UNLOCK(session);
            PJ_LOG(3, (THIS_FILE,
                       "[VALIDATION] call already torn down after validation, skip B-leg"));
            free(arg);
            return PJSUA_INVALID_ID;
        }
        CC_SESSION_UNLOCK(session);

        PJ_LOG(3, (THIS_FILE,
                   "UDP validation result: status=%d reason=%s",
                   vstatus, validation_result.reason));

        if (vstatus != 0) {
            const char *end_status;
            const char *end_reason;

            PJ_LOG(2, (THIS_FILE,
                       "UDP validation rejected call: status=%d reason=%s",
                       vstatus, validation_result.reason));

            CC_SESSION_LOCK(session);
            if (session->torn_down ||
                session->call_a == PJSUA_INVALID_ID)
            {
                CC_SESSION_UNLOCK(session);
                PJ_LOG(3, (THIS_FILE,
                           "[VALIDATION] call already torn down after validation, skip B-leg"));
                free(arg);
                return PJSUA_INVALID_ID;
            }
            session->torn_down = 1;
            CC_SESSION_UNLOCK(session);

            end_status = (vstatus < 0 ||
                          vstatus == CC_VALIDATION_API_FAILURE)
                         ? "FAILED" : "CANCELLED";
            end_reason = validation_result.reason[0]
                         ? validation_result.reason
                         : validation_end_reason(vstatus);

            cc_session_mark_end(session,
                                end_status,
                                end_reason);

            free(arg);

            /*
             * Play the appropriate rejection prompt to A before hanging up.
             * The prompt thread will hangup A after playback completes.
             */
            {
                cc_prompt_tag_t prompt_tag;
                pjsip_status_code sip_code;

                switch (vstatus) {
                case CC_VALIDATION_CALLER_BLACKLISTED:
                    prompt_tag = CC_PROMPT_NOT_AVAILABLE_TO_PAY;
                    sip_code = PJSIP_SC_FORBIDDEN;
                    break;
                case CC_VALIDATION_SPONSOR_BALANCE_FAIL:
                    prompt_tag = CC_PROMPT_LOW_BALANCE;
                    sip_code = PJSIP_SC_FORBIDDEN;
                    break;
                case CC_VALIDATION_SPONSOR_DND_ACTIVE:
                case CC_VALIDATION_SPONSOR_ROAMING:
                    prompt_tag = CC_PROMPT_NOT_AVAILABLE_TO_PAY;
                    sip_code = PJSIP_SC_FORBIDDEN;
                    break;
                default:
                    /* API_FAILURE, ELIGIBILITY_TIMEOUT, SYSTEM_ERROR */
                    prompt_tag = CC_PROMPT_NOT_AVAILABLE_TO_PAY;
                    sip_code = PJSIP_SC_SERVICE_UNAVAILABLE;
                    break;
                }

                leg_a_play_prompt_then_hangup(session, prompt_tag, sip_code);
            }

            return PJSUA_INVALID_ID;
        }

        /* Check if caller is whitelisted — skip B-leg collect prompt */
        PJ_LOG(3, (THIS_FILE,
                   "[WHITELIST-CHECK] details='%s' len=%d",
                   validation_result.details,
                   (int)strlen(validation_result.details)));
        if (validation_result.details[0] != '\0' &&
            strcasestr(validation_result.details, "IS_WHITELISTED") != NULL)
        {
            CC_SESSION_LOCK(session);
            session->whitelisted = 1;
            CC_SESSION_UNLOCK(session);
            PJ_LOG(3, (THIS_FILE,
                       "[WHITELIST] caller whitelisted; B-leg will bridge directly"));
        }

        {
            cc_service_key_mode_t sk_mode = cc_cfg_service_key_mode();
            const char *sk_mode_name = cc_cfg_service_key_mode_name();
            const char *api_service_key = validation_result.service_key;
            const char *placeholder = cc_cfg_service_key_placeholder();
            char service_key[64];
            int have_service_key = api_service_key[0] != '\0';
            char keyed_user[128];
            int keyed_len;

            if (have_service_key) {
                snprintf(service_key,
                         sizeof(service_key),
                         "%s",
                         api_service_key);
                PJ_LOG(3, (THIS_FILE,
                           "[SERVICEKEY] source=api service_key=%s",
                           service_key));
            } else {
                snprintf(service_key,
                         sizeof(service_key),
                         "%s",
                         placeholder ? placeholder : "");
                have_service_key = service_key[0] != '\0';
                if (have_service_key) {
                    PJ_LOG(3, (THIS_FILE,
                               "[SERVICEKEY] source=placeholder service_key=%s reason=missing_in_eligible_response",
                               service_key));
                }
            }

            snprintf(arg->service_key,
                     sizeof(arg->service_key),
                     "%s",
                     service_key);
            snprintf(arg->service_key_mode,
                     sizeof(arg->service_key_mode),
                     "%s",
                     sk_mode_name);
            snprintf(arg->b_dial_number,
                     sizeof(arg->b_dial_number),
                     "%s",
                     original_b);
            snprintf(arg->b_from_user,
                     sizeof(arg->b_from_user),
                     "%s",
                     session->caller_msisdn);

            if (sk_mode != CC_SERVICE_KEY_MODE_DISABLED) {
                if (have_service_key) {
                    keyed_len = snprintf(keyed_user,
                                         sizeof(keyed_user),
                                         "%s%s",
                                         service_key,
                                         original_b);
                    if (keyed_len < 0 ||
                        (size_t)keyed_len >= sizeof(keyed_user))
                    {
                        keyed_user[0] = '\0';
                        PJ_LOG(1, (THIS_FILE,
                                   "[ERROR] serviceKey+B number too long; using sponsorMsisdn"));
                    }

                    if (keyed_user[0] != '\0') {
                        snprintf(arg->b_from_user,
                                 sizeof(arg->b_from_user),
                                 "%s",
                                 (sk_mode == CC_SERVICE_KEY_MODE_FROM_ONLY ||
                                  sk_mode ==
                                      CC_SERVICE_KEY_MODE_REQUEST_URI_AND_FROM)
                                     ? keyed_user
                                     : session->caller_msisdn);

                        if (sk_mode ==
                                CC_SERVICE_KEY_MODE_REQUEST_URI ||
                            sk_mode ==
                            CC_SERVICE_KEY_MODE_REQUEST_URI_AND_FROM)
                        {
                            snprintf(arg->b_dial_number,
                                     sizeof(arg->b_dial_number),
                                     "%s",
                                     keyed_user);
                        }
                    }
                } else {
                    PJ_LOG(2, (THIS_FILE,
                               "[B-LEG] service_key_mode=%s requested but service_key is empty; using sponsorMsisdn",
                               sk_mode_name));
                }
            }

            CC_SESSION_LOCK(session);
            snprintf(session->service_key,
                     sizeof(session->service_key),
                     "%s",
                     service_key);
            snprintf(session->b_dial_number,
                     sizeof(session->b_dial_number),
                     "%s",
                     arg->b_dial_number);
            CC_SESSION_UNLOCK(session);

            PJ_LOG(3, (THIS_FILE,
                       "[B-LEG] service_key_mode=%s service_key=%s request_user=%s from_user=%s",
                       sk_mode_name,
                       have_service_key ? service_key : "<none>",
                       arg->b_dial_number,
                       arg->b_from_user));
            PJ_LOG(3, (THIS_FILE,
                       "[B-LEG] original_b_user=%s service_key=%s final_request_user=%s",
                       original_b,
                       have_service_key ? service_key : "<none>",
                       arg->b_dial_number));
        }
    }

    PJ_LOG(3, (THIS_FILE,
               "A-leg confirmed; starting B-leg to %s",
               arg->b_dial_number));

    CC_SESSION_LOCK(session);
    if (session->torn_down ||
        session->call_a == PJSUA_INVALID_ID ||
        session->final_cleanup_started)
    {
        CC_SESSION_UNLOCK(session);
        PJ_LOG(3, (THIS_FILE,
                   "[VALIDATION] call already torn down before B worker, skip B-leg"));
        free(arg);
        return PJSUA_INVALID_ID;
    }
    session->b_origination_pending = 1;
    CC_SESSION_UNLOCK(session);

    if (!cc_session_acquire_reason(session, "b-origination-worker")) {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] B-leg worker could not retain session"));
        CC_SESSION_LOCK(session);
        session->b_origination_pending = 0;
        CC_SESSION_UNLOCK(session);
        free(arg);
        cc_session_maybe_finalize(session);
        return PJSUA_INVALID_ID;
    }

    rc = pthread_create(&t, NULL, cc_originate_b_thread, arg);
    if (rc != 0) {
        PJ_LOG(1, (THIS_FILE, "[ERROR] B-leg pthread_create failed: %d", rc));
        free(arg);
        CC_SESSION_LOCK(session);
        session->b_origination_pending = 0;
        CC_SESSION_UNLOCK(session);
        cc_session_release_reason(session, "b-origination-create-failed");

        CC_SESSION_LOCK(session);
        session->torn_down = 1;
        pjsua_call_id call_a = session->call_a;
        CC_SESSION_UNLOCK(session);

        cc_session_mark_end(session, "FAILED", "SYSTEM_ERROR");

        if (call_a != PJSUA_INVALID_ID)
            cc_safe_hangup(call_a, PJSIP_SC_SERVICE_UNAVAILABLE);
        cc_session_maybe_finalize(session);
        return PJSUA_INVALID_ID;
    }

    rc = pthread_detach(t);
    if (rc != 0) {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] B-leg pthread_detach failed: %d", rc));
    }

    return PJSUA_INVALID_ID;
}

/* ── Originate B leg ─────────────────────────────────────────────────────── */

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
        cc_session_maybe_finalize(session);
        cc_session_release_reason(session, "b-origination-register-failed");
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
        CC_SESSION_UNLOCK(session);
        cc_session_mark_end(session, "FAILED", "SYSTEM_ERROR");
        leg_a_play_unavailable_then_hangup(session);
        cc_session_maybe_finalize(session);
        cc_session_release_reason(session, "b-origination-uri-failed");
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
        cc_session_maybe_finalize(session);
        cc_session_release_reason(session, "b-origination-stale");
        return NULL;
    }
    CC_SESSION_UNLOCK(session);

    /* Wait for A's one-shot waiting prompt to finish before ringing B.
     * This ensures B does not ring while A is still hearing the prompt. */
    {
        int prompt_ms;
        CC_SESSION_LOCK(session);
        prompt_ms = session->a_prompt_duration_ms;
        CC_SESSION_UNLOCK(session);

        if (prompt_ms > 0) {
            int waited_ms = 0;
            const int max_wait_ms = prompt_ms + 1000; /* cap: prompt + 1s slack */
            PJ_LOG(3, (THIS_FILE,
                       "[B-LEG] waiting %dms for A prompt to finish before originating B",
                       prompt_ms));
            while (waited_ms < max_wait_ms) {
                int done;
                cc_sleep_ms(50);
                waited_ms += 50;
                CC_SESSION_LOCK(session);
                done = session->torn_down ||
                       session->call_a == PJSUA_INVALID_ID ||
                       session->final_cleanup_started;
                CC_SESSION_UNLOCK(session);
                if (done) {
                    PJ_LOG(3, (THIS_FILE,
                               "[B-LEG] A prompt wait aborted: call torn down"));
                    CC_SESSION_LOCK(session);
                    session->b_origination_pending = 0;
                    CC_SESSION_UNLOCK(session);
                    cc_session_maybe_finalize(session);
                    cc_session_release_reason(session, "b-origination-stale");
                    return NULL;
                }
                if (waited_ms >= prompt_ms)
                    break;
            }
            PJ_LOG(3, (THIS_FILE,
                       "[B-LEG] A prompt wait done after %dms; originating B",
                       waited_ms));
        }
    }

    /* Start MOH on A-leg while B is ringing — all callers */
    {
        pjsua_call_id call_a;
        CC_SESSION_LOCK(session);
        call_a = session->call_a;
        CC_SESSION_UNLOCK(session);

        if (call_a != PJSUA_INVALID_ID) {
            const char *moh_path = cc_prompt_get_path(CC_PROMPT_MOH);
            pjsua_player_id moh_pid = cc_start_wav(call_a, moh_path, PJ_FALSE);
            if (moh_pid != PJSUA_INVALID_ID) {
                CC_SESSION_LOCK(session);
                if (session->call_a == call_a &&
                    !session->accepted &&
                    !session->torn_down &&
                    session->player_a == PJSUA_INVALID_ID)
                {
                    session->player_a = moh_pid;
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

    pjsua_call_setting_default(&cs);
	
	/* B-leg is audio only; disable video/text m= sections. */
	cs.aud_cnt = 1;
	cs.vid_cnt = 0;
	cs.txt_cnt = 0; 



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
    int p_early_media_added = 0;

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
        p_early_media_added = cc_add_msg_header(hdr_pool,
                                                &msg_data,
                                                "P-Early-Media",
                                                "Supported");

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

    /* Route header to SBC (loose-route) — added only if configured */
    {
        const char *route_host = cc_cfg_sbc_host();
        int route_port = cc_cfg_sbc_port();

        if (route_host && route_host[0] != '\0' && hdr_pool) {
            char route_buf[256];
            int rlen = snprintf(route_buf, sizeof(route_buf),
                                "<sip:%s:%d;transport=udp;lr>",
                                route_host, route_port);
            if (rlen > 0 && (size_t)rlen < sizeof(route_buf)) {
                cc_add_msg_header(hdr_pool, &msg_data, "Route", route_buf);
                PJ_LOG(3, (THIS_FILE,
                           "[B-LEG-HDR] Route=%s", route_buf));
            }
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
        CC_SESSION_UNLOCK(session);
        cc_session_mark_end(session, "FAILED", "SYSTEM_ERROR");
        leg_a_play_unavailable_then_hangup(session);
        cc_session_maybe_finalize(session);
        cc_session_release_reason(session, "b-origination-failed");
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
            cc_session_maybe_finalize(session);
            cc_session_release_reason(session, "b-origination-late-stale");
            return NULL;
        }
    }

    /* make_call() received session as user_data; reinforce the association. */
    pjsua_call_set_user_data(call_b, session);

    /* Start ring timeout watchdog */
    leg_b_start_ring_timer(session);

    PJ_LOG(3, (THIS_FILE, "B leg call_id=%d started", call_b));
    cc_session_release_reason(session, "b-origination-complete");
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

    (void)e;
    session = (cc_session_t *)pjsua_call_get_user_data(call_id);
    if (!session || !cc_session_acquire_reason(session, "callback-call-state"))
        return;

    leg = cc_resolve_leg(session, call_id);
    if (leg == 1)
        deferred_hangup = leg_a_on_call_state(call_id, session);
    else if (leg == 2)
        leg_b_on_call_state(call_id, session);

    cc_session_release_reason(session, "callback-call-state");

    if (deferred_hangup != PJSUA_INVALID_ID) {
        PJ_LOG(3, (THIS_FILE,
                   "[VALIDATION] callback reference released; hangup rejected A-leg call=%d",
                   deferred_hangup));
        cc_safe_hangup(deferred_hangup, PJSIP_SC_FORBIDDEN);
    }
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

static void cc_rewrite_sdp_audio_endpoint(pj_pool_t *pool,
                                          pjmedia_sdp_session *sdp,
                                          const cc_rtp_ep_t *ep,
                                          const char *tag)
{
    pj_size_t i;

    if (!pool || !sdp || !ep || !ep->valid)
        return;

    /* Rewrite session-level c= line if present */
    if (sdp->conn) {
        sdp->conn->addr = pj_strdup3(pool, ep->ip);
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
            continue;
        }

        /* Rewrite audio m= port */
        m->desc.port = (pj_uint16_t)ep->port;

        /* Rewrite media-level c= line */
        conn = m->conn ? m->conn : sdp->conn;
        if (conn) {
            conn->addr = pj_strdup3(pool, ep->ip);
        } else {
            m->conn = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_conn);
            m->conn->net_type  = pj_str("IN");
            m->conn->addr_type = pj_str("IP4");
            m->conn->addr      = pj_strdup3(pool, ep->ip);
        }

        /* Rewrite a=rtcp:<port+1> IN IP4 <ip> */
        snprintf(rtcp_value, sizeof(rtcp_value), "%d IN IP4 %s",
                 ep->port + 1, ep->ip);

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
                   ep->ip,
                   ep->port));
    }
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
    cc_rtp_ep_t target;
    const char *method = NULL;
    const char *tag = NULL;

    (void)rem_sdp;

    cc_sdp_set_session_name(sdp, pool, CC_SDP_SESSION_NAME);

    session = (cc_session_t *)pjsua_call_get_user_data(call_id);
    if (!session || !cc_session_acquire_reason(session, "callback-sdp"))
        return;

    {
        int leg = cc_resolve_leg(session, call_id);
        int has_pcma = cc_sdp_has_rtpmap(sdp, "PCMA/8000");
        int has_telephone_event =
            cc_sdp_has_rtpmap(sdp, "telephone-event/8000");

        if (has_pcma && has_telephone_event) {
            PJ_LOG(3, (THIS_FILE,
                       "[SDP] call_id=%d leg=%s PCMA/8000=yes telephone-event/8000=yes",
                       call_id,
                       leg == 1 ? "A" : (leg == 2 ? "B" : "UNKNOWN")));
        } else {
            PJ_LOG(1, (THIS_FILE,
                       "[SDP] call_id=%d leg=%s PCMA/8000=%s telephone-event/8000=%s",
                       call_id,
                       leg == 1 ? "A" : (leg == 2 ? "B" : "UNKNOWN"),
                       has_pcma ? "yes" : "no",
                       has_telephone_event ? "yes" : "no"));
        }
    }

    memset(&target, 0, sizeof(target));

    CC_SESSION_LOCK(session);

    if (call_id == session->call_a &&
        (session->update_a_pending || session->reinvite_a_pending) &&
        session->rtp_b.valid)
    {
        do_a = 1;
        target = session->rtp_b;
        was_update = session->update_a_pending;
        was_reinvite = session->reinvite_a_pending;
        session->update_a_pending = 0;
        session->reinvite_a_pending = 0;
    }
    else if (call_id == session->call_b &&
             (session->update_b_pending || session->reinvite_b_pending) &&
             session->rtp_a.valid)
    {
        do_b = 1;
        target = session->rtp_a;
        was_update = session->update_b_pending;
        was_reinvite = session->reinvite_b_pending;
        session->update_b_pending = 0;
        session->reinvite_b_pending = 0;
    }

    CC_SESSION_UNLOCK(session);

    if (was_update && was_reinvite)
        method = "UPDATE+REINVITE";
    else if (was_reinvite)
        method = "REINVITE";
    else
        method = "UPDATE";

    if (do_a) {
        tag = was_reinvite ? "A-REINVITE" : "A-UPDATE";
        cc_rewrite_sdp_audio_endpoint(pool, sdp, &target, tag);
        PJ_LOG(3, (THIS_FILE,
                   "[SDP-REWRITE] A-leg %s SDP rewritten to B RTP %s:%d",
                   method,
                   target.ip,
                   target.port));
    } else if (do_b) {
        tag = was_reinvite ? "B-REINVITE" : "B-UPDATE";
        cc_rewrite_sdp_audio_endpoint(pool, sdp, &target, tag);
        PJ_LOG(3, (THIS_FILE,
                   "[SDP-REWRITE] B-leg %s SDP rewritten to A RTP %s:%d",
                   method,
                   target.ip,
                   target.port));
    }

    cc_session_release_reason(session, "callback-sdp");
}
