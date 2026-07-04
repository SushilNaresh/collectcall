#ifndef CC_CONFIG_H
#define CC_CONFIG_H

/*
 * config.h — Operator configuration for the Collect Call B2BUA
 * All deployment-specific values live here.
 */

/* ── SIP / Network ────────────────────────────────────────────────────── */
/*
 * Compile-time fallback used when the CC_COLLECT_PREFIX environment
 * variable is unset or invalid. Customer short codes are 49013 and 49014.
 * Example runtime override: export CC_COLLECT_PREFIX=49013
 */
#define CC_COLLECT_PREFIX           "1800"
#define CC_SIP_DOMAIN               "10.20.10.119:5070"
#define CC_LOCAL_HOST               "10.20.10.119"
#define CC_LOCAL_SIP_PORT           6060

#define CC_USER_AGENT               "CollectCall"

/* Initial B-leg INVITE identity and operator header policy. */
#define CC_BLEG_FROM_USE_FINAL_DIAL_NUMBER 1
#define CC_BLEG_STATIC_PANI_ENABLE         1
#define CC_BLEG_STATIC_PANI                "GSTN;gstn-location=\"03930803406806\";network-provided"
#define CC_BLEG_REPLACE_COPIED_PANI        1

/* Application-generated PJLIB/PJSIP log file. */
#define CC_APP_LOG_ENABLE           1
#define CC_APP_LOG_DIR              "logs"
#define CC_APP_LOG_TO_CONSOLE       1
#define CC_APP_LOG_FLUSH_ALWAYS     1
#define CC_APP_LOG_FILE_MODE        0640
#define CC_APP_LOG_PREFIX           "collect_call"

/* ── SIP OPTIONS health check ────────────────────────────────────────── */
#define CC_OPTIONS_ENABLE           0
#define CC_OPTIONS_PERIODIC_ENABLE  0
#define CC_OPTIONS_INTERVAL_SEC     30
#define CC_OPTIONS_TARGET_URI       "sip:192.168.29.173:5060"

/* ── Number normalization ──────────────────────────────────────────────── */
/* Country code prepended when a number starts with '0' (local format). */
#define CC_DEFAULT_COUNTRY_CODE     "234"

/* ── DTMF ─────────────────────────────────────────────────────────────── */
#define CC_DTMF_ACCEPT              '1'
#define CC_DTMF_REJECT              '2'

/* ── Timers (seconds) ─────────────────────────────────────────────────── */
#define CC_B_RING_TIMEOUT_SEC       60
#define CC_B_DTMF_TIMEOUT_SEC       30

/* ── UDP Validation Stub ─────────────────────────────────────────────── */
#define CC_VALIDATION_UDP_HOST       "127.0.0.1"
#define CC_VALIDATION_UDP_PORT       9090
#define CC_VALIDATION_TIMEOUT_MS     1000
#define CC_VALIDATION_UDP_BIND_LOCAL_PORT 0
#define CC_VALIDATION_UDP_LOCAL_PORT 9091
#define CC_INITIATE_SOURCE           "PREFIX_INITIATED"

/*
 * ServiceKey handling.
 * Runtime override: CC_SERVICE_KEY_PLACEHOLDER=8024
 */
#define CC_SERVICE_KEY_PREPEND_ENABLE 0
#define CC_SERVICE_KEY_PLACEHOLDER     "8024"

/* Fire-and-forget end_call reporting through local udp2http. */
#define CC_CALL_END_UDP_ENABLE        1
#define CC_CALL_END_UDP_HOST          "127.0.0.1"
#define CC_CALL_END_UDP_PORT          9092
#define CC_CALL_END_UDP_BIND_LOCAL_PORT 0
#define CC_CALL_END_UDP_LOCAL_PORT    9093

/*
 * The supplied accepted-values list does not currently show the successful
 * call reason. Keep it isolated here until the API owner confirms it.
 */
#define CC_END_API_COMPLETED_REASON           "NORMAL_CLEARING"
#define CC_END_API_COMPLETED_REASON_CONFIRMED 0

/* ── RTP port range ────────────────────────────────────────────────────── */
#define CC_RTP_PORT_START            6000
#define CC_RTP_PORT_COUNT            400

/* Forward P-headers on INVITE. Keep UPDATE forwarding disabled unless required. */
#define CC_COPY_P_HEADERS_IN_UPDATE  0

/* Media change mode after B accepts with DTMF 1.
 * 0 = use existing SIP UPDATE flow
 * 1 = use SIP re-INVITE flow
 */
#define CC_MEDIA_CHANGE_USE_REINVITE 1

/* RTP bypass experiment only.
 * Keep disabled by default because local VM/MicroSIP testing still needs VM media path for audio.
 * Set to 1 only when testing whether direct RTP works in real network/server setup.
 */
#define CC_REINVITE_UNBRIDGE_AFTER_SEND 0

/* ── WAV announcements ────────────────────────────────────────────────── */
#define CC_WAV_WAITING              "wav/waiting.wav"
#define CC_WAV_COLLECT_PROMPT       "wav/collect_prompt.wav"
#define CC_WAV_REJECTED             "wav/rejected.wav"
#define CC_WAV_UNAVAILABLE          "wav/unavailable.wav"

/* ── RTP bypass — MGW pool subnets ───────────────────────────────────── */



/* Bypass test mode:
 * 0 = keep the local PJSUA conference bridge
 * 1 = send UPDATEs without starting the local bridge
 */
#define CC_BYPASS_TEST_MODE 0



/* IPs matching these prefixes → MGW bypass (X-MGW-Directive)            */
/* All other IPs               → DIRECT VoLTE hairpin (SDP rewrite)      */
#define CC_MGW_SUBNET_COUNT         2
static const char *CC_MGW_SUBNETS[] = { "10.200.", "10.201." };

/* ── Operator headers to forward A → B ───────────────────────────────── */
#define CC_FWD_HDR_COUNT            6
static const char *CC_FWD_HEADERS[] = {
    "P-Asserted-Identity",
    "P-Preferred-Identity",
    "Privacy",
    "P-Access-Network-Info",
    "P-Charging-Vector",
    "P-Charging-Function-Addresses"
};

/* ── PJSUA engine ─────────────────────────────────────────────────────── */
/*
 * PJSUA counts call legs, not complete collect-call sessions.
 * A connected collect call normally consumes two legs (A + B), so
 * CC_MAX_CALLS=200 is theoretically about 100 paired sessions. The RTP
 * port range must also be sized for both media legs plus operating margin.
 */
#define CC_MAX_CALLS                200
#define CC_LOG_LEVEL                4
#define CC_CLOCK_RATE               8000   /* G.711 narrowband */
#define CC_POOL_INIT_SIZE           4000
#define CC_POOL_INC_SIZE            4000

/*
 * Adaptive event loop cap (milliseconds).
 * The loop sleeps until the next PJSIP timer fires, but no longer than
 * this value. Keeps signal/shutdown latency bounded while avoiding the
 * fixed 10 ms busy-poll when the system is idle.
 */
#define CC_EVENT_LOOP_MAX_MS        500

#endif /* CC_CONFIG_H */
