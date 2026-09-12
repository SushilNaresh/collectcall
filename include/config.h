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
#define CC_LOCAL_SIP_PORT           15060

#define CC_USER_AGENT               "CollectCall"

/* Initial B-leg INVITE identity and operator header policy. */
#define CC_BLEG_FROM_USE_FINAL_DIAL_NUMBER 1
#define CC_BLEG_STATIC_PANI_ENABLE         1
#define CC_BLEG_STATIC_PANI                "GSTN;gstn-location=\"03930803406806\";network-provided"
#define CC_BLEG_REPLACE_COPIED_PANI        1

/* Application-generated PJLIB/PJSIP log file. */
#define CC_APP_LOG_ENABLE           1
#define CC_APP_LOG_DIR              "logs"
#define CC_APP_LOG_TO_CONSOLE       0    /* disable at >50 CPS — console I/O is a bottleneck */
#define CC_APP_LOG_FLUSH_ALWAYS     0    /* disable at >50 CPS — let OS buffer; rotate handles loss */
#define CC_APP_LOG_FILE_MODE        0640
#define CC_APP_LOG_PREFIX           "collect_call"
#define CC_APP_LOG_MAX_SIZE_MB      100   /* rotate log file after this many MB; 0 = no limit */

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
#define CC_B_DTMF_TIMEOUT_SEC       10

/* ── UDP Validation Stub ─────────────────────────────────────────────── */
#define CC_VALIDATION_UDP_HOST       "127.0.0.1"
#define CC_VALIDATION_UDP_PORT       9090
#define CC_VALIDATION_TIMEOUT_MS     5000
#define CC_VALIDATION_UDP_BIND_LOCAL_PORT 0
#define CC_VALIDATION_UDP_LOCAL_PORT 9091
#define CC_INITIATE_SOURCE_NORMAL    "PREFIX_INITIATED"
#define CC_INITIATE_SOURCE_FUNDLESS  "LOW_BALANCE"

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
/*
 * RTP port range.
 * 4 ports per session (RTP+RTCP × A+B legs):
 *   100 CPS × 60s hold = 6,000 sessions × 4 = 24,000 ports needed
 *   200 CPS × 60s hold = 12,000 sessions × 4 = 48,000 ports needed
 * Stay below Linux ephemeral range (32768–60999).
 * Range 16000–31999 = 16,000 ports → only covers ~4,000 sessions (~67 CPS) — too low
 * Range 16000–65151 = 49,152 ports → covers 12,288 sessions (200 CPS) with headroom
 * Pin ephemeral range above 65152: sysctl -w net.ipv4.ip_local_port_range="65152 65535"
 */
#define CC_RTP_PORT_START            16000
#define CC_RTP_PORT_COUNT            49152  /* 16000–65151: covers 200 CPS x 60s hold */

/* Forward P-headers on INVITE. Keep UPDATE forwarding disabled unless required. */
#define CC_COPY_P_HEADERS_IN_UPDATE  0

/* Media change mode after B accepts with DTMF 1.
 * 0 = use SIP UPDATE flow (production default — exits RTP path)
 * 1 = use SIP re-INVITE flow
 */
#define CC_MEDIA_CHANGE_USE_REINVITE 0

/* RTPengine ng control (used when CC_MEDIA_MODE=rtpengine). */
#define CC_RTPENGINE_HOST            "127.0.0.1"
#define CC_RTPENGINE_PORT            22222
#define CC_RTPENGINE_TIMEOUT_MS      1500
/* Do not use trust-address: phones often put a private LAN IP in SDP while
 * RTP arrives from a public/NAT source; without trust-address RTPengine
 * learns the real source so play-media reaches the handset. */
/* port-latching: keep A-facing ports stable across dummy answer → real B
 * answer so we do not mid-dialog re-INVITE A when B’s remote RTP changes. */
/* detect-DTMF + force-transcoding (+ always-transcode): kernel forward relays
 * RFC2833 without userspace, so DTMF-log-dest never fires. force-transcoding
 * keeps media in userspace even when codecs match; detect-DTMF covers in-band.
 * Dummy answer SDP also advertises telephone-event PT 120 (MicroSIP) + 101.
 * codec-strip-*: single G.711 PCMA toward phones — SIP SDP is also restricted
 * to PCMA+TE in b2bua rewrite (PJSUA must not advertise PCMU/G722). */
#define CC_RTPENGINE_FLAGS           "replace-origin,replace-session-connection,ICE=remove,port-latching,detect-DTMF,force-transcoding,always-transcode,codec-strip-G722,codec-strip-opus,codec-strip-GSM,codec-strip-PCMU"
#define CC_RTPENGINE_DTMF_PORT       22223
#define CC_RTPENGINE_MEDIA_DIR       ""

/* Unbridge local PJSUA conference after sending re-INVITE.
 * Only relevant when CC_MEDIA_CHANGE_USE_REINVITE=1.
 */
#define CC_REINVITE_UNBRIDGE_AFTER_SEND 0

/* ── WAV announcements ────────────────────────────────────────────────── */
#define CC_WAV_WAITING              "wav/waiting.wav"
#define CC_WAV_COLLECT_PROMPT       "wav/collect_prompt.wav"
#define CC_WAV_REJECTED             "wav/rejected.wav"
#define CC_WAV_UNAVAILABLE          "wav/unavailable.wav"

/* ── RTP bypass — MGW pool subnets ───────────────────────────────────── */



/* Bypass test mode — unused, kept for reference only */
#define CC_BYPASS_TEST_MODE 0



/* IPs matching these prefixes → MGW bypass (X-MGW-Directive)            */
/* All other IPs               → DIRECT VoLTE hairpin (SDP rewrite)      */
#define CC_MGW_SUBNET_COUNT         2
/* IPs matching these prefixes → MGW bypass (X-MGW-Directive)            */
/* All other IPs               → DIRECT VoLTE hairpin (SDP rewrite)      */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
static const char *CC_MGW_SUBNETS[] = { "10.200.", "10.201." };

/* ── Operator headers to forward A → B ───────────────────────────────── */
#define CC_FWD_HDR_COUNT            (sizeof(CC_FWD_HEADERS) / sizeof(CC_FWD_HEADERS[0]))
static const char *CC_FWD_HEADERS[] = {
    "P-Asserted-Identity",
    "P-Preferred-Identity",
    "Privacy",
    "P-Access-Network-Info",
    "P-Charging-Vector",
    "P-Charging-Function-Addresses",
    "X-Orig-SBC"
};
#pragma GCC diagnostic pop

/* ── PJSUA engine ─────────────────────────────────────────────────────── */
/*
 * PJSUA counts call legs, not complete collect-call sessions.
 * A connected collect call normally consumes two legs (A + B), so
 * CC_MAX_CALLS=8192 supports ~4,096 paired sessions per process/IP.
 * True ceiling is RTP port exhaustion: 16000–32767 = 16,768 ports
 * ÷ 4 ports/session (RTP+RTCP × 2 legs) = 4,192 sessions max per IP.
 * CC_RTP_PORT_COUNT=16384 covers this range.
 * Requires PJSUA recompile with PJSUA_MAX_CALLS=8192 (set in Makefile).
 */
#define CC_MAX_CALLS                32768  /* matches PJSUA_MAX_CALLS in config_site.h; 200 CPS x 60s = 12000 sessions x 2 legs = 24000 */
#define CC_LOG_LEVEL                2    /* 2=errors+warnings at >100 CPS; level 3 adds per-call INFO lines */
#define CC_CLOCK_RATE               8000   /* G.711 narrowband */
#define CC_AUDIO_PTIME_MS           20     /* conference / mem-player frame size */
#define CC_JB_INIT_MS               40     /* initial jitter prefetch */
#define CC_JB_MIN_PRE_MS            20     /* minimum prefetch; avoids underrun */
#define CC_JB_MAX_PRE_MS            80     /* adaptive prefetch ceiling */
#define CC_JB_MAX_MS                200    /* hard discard; later packets are useless */
#define CC_EC_TAIL_MS               200    /* used only if line echo is enabled */
/*
 * Per-stream AEC on a null-snd B2BUA. Default off: endpoints own echo,
 * and wrapping every leg caused delaybuf asserts and SIP-thread hangs.
 * Override at runtime: CC_LINE_ECHO=1
 */
#ifndef CC_LINE_ECHO_ENABLE
#define CC_LINE_ECHO_ENABLE         0
#endif
#define CC_POOL_INIT_SIZE           8192   /* fits cc_session_t(~3940B) + fwd_hdr values in one block; was 4000 */
#define CC_POOL_INC_SIZE            4096   /* was 4000 */

/*
 * Adaptive event loop cap (milliseconds).
 * The loop sleeps until the next PJSIP timer fires, but no longer than
 * this value. Keeps signal/shutdown latency bounded while avoiding the
 * fixed 10 ms busy-poll when the system is idle.
 */
#define CC_EVENT_LOOP_MAX_MS        10

/* Build version — overridden at compile time by Makefile via -DCC_BUILD_VERSION */
#ifndef CC_BUILD_VERSION
#define CC_BUILD_VERSION "dev"
#endif

/* Free period: max prompt duration for A-party, and minimum time before
 * charging starts (B-accept is delayed if it arrives within this window).
 * Override: export CC_FREE_PERIOD_MS=10000
 */
#define CC_FREE_PERIOD_MS           10000

#endif /* CC_CONFIG_H */
