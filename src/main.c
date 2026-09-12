/*
 * main.c — PJSUA C-API bootstrap for the Collect Call B2BUA
 *
 * Initialises PJSUA with operator-grade settings:
 *   - No VAD, G.711 narrowband, RFC 2833 DTMF
 *   - UDP + TCP SIP transports
 *   - No SIP REGISTER (inline B2BUA, routed by IMS core)
 *   - pjsua_callback wired to global cc_* handlers
 *   - Worker pool started after pjsua_start(), stopped before pjsua_destroy()
 */
#include "api_mapping.h"
#include "b2bua.h"
#include "app_logger.h"
#include "config.h"
#include "env_loader.h"
#include "options.h"
#include "prompt_mapping.h"
#include "runtime_config.h"
#include "rtpengine.h"
#include "utils.h"
#include "worker.h"
#include "validation_async.h"

#include <pjsua-lib/pjsua.h>
#include <pjsua-lib/pjsua_internal.h>
#include <pjsip/sip_endpoint.h>
#include <pjsip/sip_config.h>
#include <pjmedia/echo.h>
#include <pjmedia/jbuf.h>
#include <pj/timer.h>
#include <pj/log.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <stdio.h>
#include <string.h>

#define THIS_FILE "main.c"

static volatile int g_running = 1;

/* Wakeup pipe: sig_handler writes 1 byte to g_wake_wfd to unblock
 * the select() inside pjsip_endpt_handle_events2(). */
static int g_wake_rfd = -1;
static int g_wake_wfd = -1;

static void sig_handler(int sig)
{
    char byte = 1;
    (void)sig;
    g_running = 0;
        if (g_wake_wfd >= 0) {
            ssize_t _ign = write(g_wake_wfd, &byte, 1);
            (void)_ign;
        }
}

/*
 * acc_id is needed by cc_originate_b_thread.
 * We store it at startup so the B-leg origination thread can use it.
 * (PJSUA has a global account pool; in production you may have multiple
 *  accounts, in which case pass acc_id through the session.)
 */
pjsua_acc_id g_acc_id = PJSUA_INVALID_ID;

int main(void)
{
    pjsua_config         ua_cfg;
    pjsua_logging_config log_cfg;
    pjsua_media_config   med_cfg;
    pjsua_transport_config tp_cfg;
    pjsua_acc_config     acc_cfg;
    pj_status_t          status;
    const char          *local_host;
    int                  local_sip_port;
    char                 acc_id_buf[160];

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    /* Load config file before anything reads env vars.
     * Priority: real env > file values. File path is itself configurable
     * via CC_ENV_FILE env var; defaults to /etc/collect_call.env */
    {
        const char *env_path = getenv("CC_ENV_FILE");
        if (!env_path || env_path[0] == '\0')
            env_path = "/etc/collect_call.env";
        cc_load_env_file(env_path);
    }

    (void)cc_app_logger_init();
    local_host = cc_cfg_local_host();
    local_sip_port = cc_cfg_local_sip_port();

    /* ── 1. Create PJSUA ──────────────────────────────────────────── */
    status = pjsua_create();
    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE, "pjsua_create() failed: %d", status));
        cc_app_logger_close();
        return 1;
    }

    cc_app_logger_install_pj_writer();

    /*
     * Cap PJSUA's internal caching pool (pjsua_var.cp).
     * Default max_capacity=0 means unlimited: every freed per-call block
     * (dialog, inv_session, SDP, transactions, RTP state) accumulates in
     * the free list and is never returned to the OS.  RSS grows with peak
     * call count and never shrinks.
     * Cap = CC_MAX_CALLS * 64KB covers the full concurrent working set
     * (~40KB/leg * 8192 legs = 320MB) with headroom for block reuse.
     * Blocks beyond the cap are freed directly to the OS via free().
     */
    pjsua_var.cp.max_capacity = (pj_size_t)cc_cfg_max_calls() * 64 * 1024;
    PJ_LOG(3, (THIS_FILE, "[CONFIG] pjsua internal pool cap=%zu MB",
               (size_t)(pjsua_var.cp.max_capacity / 1024 / 1024)));

    cc_session_pool_init();
    PJ_LOG(3, (THIS_FILE,
               "[APP] start pid=%ld log_dir=%s log_file=%s",
               (long)getpid(),
               cc_app_logger_dir(),
               cc_app_logger_path()));
    PJ_LOG(3, (THIS_FILE, "[APP] version=%s", CC_BUILD_VERSION));

    /* ── 2. Configure ────────────────────────────────────────────── */
    pjsua_config_default(&ua_cfg);
    ua_cfg.max_calls    = cc_cfg_max_calls();
    ua_cfg.thread_cnt   = 6;   /* SIP I/O: A answer is off-thread; more threads help INVITE/UPDATE */
    ua_cfg.user_agent   = pj_str((char *)cc_cfg_user_agent());

    /* Wire global callbacks */
    ua_cfg.cb.on_incoming_call    = cc_on_incoming_call;
    ua_cfg.cb.on_call_state       = cc_on_call_state;
    ua_cfg.cb.on_call_media_state = cc_on_call_media_state;
    ua_cfg.cb.on_call_sdp_created = cc_on_call_sdp_created;

    /*
     * DTMF handling:
     * - on_dtmf_digit2 is primary for RFC2833 and SIP INFO with method info.
     * - on_dtmf_digit remains wired for legacy compatibility.
     */
    ua_cfg.cb.on_dtmf_digit       = cc_on_dtmf_digit;
    ua_cfg.cb.on_dtmf_digit2      = cc_on_dtmf_digit2;
    ua_cfg.cb.on_stream_precreate = cc_on_stream_precreate;
    /* Always wire stream_created2: rtpengine mode pauses TX+RX at create;
     * line-echo wrap also runs from this callback when enabled. */
    ua_cfg.cb.on_stream_created2  = cc_on_stream_created2;
    if (cc_line_echo_enabled())
        ua_cfg.cb.on_stream_destroyed = cc_on_stream_destroyed;

    pjsua_logging_config_default(&log_cfg);
    log_cfg.level         = cc_cfg_log_level();
    log_cfg.console_level = 0;
    log_cfg.cb            = &cc_app_logger_writer;

    pjsua_media_config_default(&med_cfg);
    med_cfg.no_vad         = PJ_TRUE;  /* disable VAD — B2BUA must always forward RTP */
    med_cfg.clock_rate     = CC_CLOCK_RATE;
    med_cfg.snd_clock_rate = CC_CLOCK_RATE;
    med_cfg.channel_count  = 1;
    med_cfg.ptime          = CC_AUDIO_PTIME_MS;
    med_cfg.quality        = 8;

    /*
     * Jitter buffer and line-echo processing apply whenever RTP is in the
     * B2BUA: the whole call in local_bridge, and prompts/hold/fallback in
     * UPDATE/re-INVITE. After hairpin bypass, endpoints handle their own QoS.
     *
     * Jitter buffer (G.711 ~20ms ptime over carrier):
     *   jb_init    = 40ms  — initial depth; absorbs burst at call start
     *   jb_min_pre = 20ms  — keep 1-2 packets; avoids underrun
     *   jb_max_pre = 80ms  — adaptive ceiling under congestion
     *   jb_max     = 200ms — discard later packets
     *
     * Per-leg AEC is off unless CC_LINE_ECHO=1 (null snd has no speaker/mic).
     */
    med_cfg.jb_init         = CC_JB_INIT_MS;
    med_cfg.jb_min_pre      = CC_JB_MIN_PRE_MS;
    med_cfg.jb_max_pre      = CC_JB_MAX_PRE_MS;
    med_cfg.jb_max          = CC_JB_MAX_MS;
    med_cfg.jb_discard_algo = PJMEDIA_JB_DISCARD_PROGRESSIVE;
    med_cfg.ec_tail_len     = cc_line_echo_enabled() ? CC_EC_TAIL_MS : 0;
    med_cfg.ec_options      = PJMEDIA_ECHO_SIMPLE | PJMEDIA_ECHO_USE_SW_ECHO;
    med_cfg.thread_cnt      = 4;

    {
        unsigned extra_players = 256;
        unsigned ports = (unsigned)cc_cfg_max_calls() * 2u + extra_players;
#ifdef PJSUA_MAX_CONF_PORTS
        if (ports > (unsigned)PJSUA_MAX_CONF_PORTS)
            ports = (unsigned)PJSUA_MAX_CONF_PORTS;
#endif
        med_cfg.max_media_ports = ports;
    }

    status = pjsua_init(&ua_cfg, &log_cfg, &med_cfg);
    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE, "pjsua_init() failed: %d", status));
        pjsua_destroy();
        cc_app_logger_close();
        return 1;
    }
    cc_media_qos_init(cc_cfg_max_calls());
    cc_tune_audio_codecs();

    /* Disable auto UDP->TCP switch for messages >1300 bytes (RFC 3261 18.1.1).
     * The SBC expects UDP; our B-leg INVITE with all headers exceeds 1300. */
    pjsip_cfg()->endpt.disable_tcp_switch = PJ_TRUE;

    /* Add REGISTER and PUBLISH to Allow header */
    {
        pjsip_endpoint *endpt = pjsua_get_pjsip_endpt();
        const pj_str_t allow_methods[] = {
            { "REGISTER", 8 },
            { "PUBLISH", 7 }
        };
        const pj_str_t supported_ext[] = {
            { "histinfo", 8 }
        };

        pjsip_endpt_add_capability(endpt, NULL, PJSIP_H_ALLOW,
                                   NULL, 2, allow_methods);
        pjsip_endpt_add_capability(endpt, NULL, PJSIP_H_SUPPORTED,
                                   NULL, 1, supported_ext);
    }

    status = cc_sip_contact_fix_install();
    if (status != PJ_SUCCESS)
        PJ_LOG(2, (THIS_FILE,
                   "[CONFIG] contact fixup module not registered: %d "
                   "(bracketed-GRUU ACKs may fail)", status));

    cc_app_logger_install_pj_writer();
    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] PJSUA_MAX_CALLS compile_limit=%d ua_cfg.max_calls=%d",
               PJSUA_MAX_CALLS, ua_cfg.max_calls));
    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] SIP local=%s:%d sbc_next_hop=%s",
               local_host,
               local_sip_port,
               cc_cfg_sbc_next_hop()));
    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] collect_prefixes=%s source=%s prefix_mode=%s default_country_code=%s",
               cc_cfg_collect_prefixes(),
               cc_collect_prefix_is_env_override() ?
                   "environment" : "compile-time-default",
               cc_cfg_prefix_mode_name(),
               cc_cfg_default_country_code()));
    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] RTP range=%d-%d count=%d",
               cc_cfg_rtp_port_start(),
               cc_cfg_rtp_port_start() + cc_cfg_rtp_port_count() - 1,
               cc_cfg_rtp_port_count()));
    if (cc_cfg_rtp_port_start() + cc_cfg_rtp_port_count() > 65535)
        PJ_LOG(1, (THIS_FILE,
                   "[CONFIG] WARNING: RTP range %d-%d exceeds port limit 65535 — "
                   "reduce CC_RTP_PORT_START or CC_RTP_PORT_COUNT",
                   cc_cfg_rtp_port_start(),
                   cc_cfg_rtp_port_start() + cc_cfg_rtp_port_count() - 1));
    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] validation=%s:%d timeout_ms=%d",
               cc_cfg_validation_host(),
               cc_cfg_validation_port(),
               CC_VALIDATION_TIMEOUT_MS));
    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] initiate_source=PREFIX_INITIATED|LOW_BALANCE(fundless) service_key_mode=%s",
               cc_cfg_service_key_mode_name()));
    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] end_udp_enable=%d target=%s:%d",
               CC_CALL_END_UDP_ENABLE,
               cc_cfg_endcall_host(),
               cc_cfg_endcall_port()));
    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] pani_value=%s",
               cc_cfg_pani_value()));
    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] free_period_ms=%d",
               cc_cfg_free_period_ms()));
    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] fundless_prefixes=%s",
               cc_cfg_fundless_prefixes()));
    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] validation_timeout_ms=%d",
               cc_cfg_validation_timeout_ms()));
    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] b_dtmf_timeout_sec=%d",
               cc_cfg_b_dtmf_timeout_sec()));
    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] max_call_legs=%d max_sessions=%d",
               cc_cfg_max_calls(), cc_cfg_max_calls() / 2));
    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] admission_max_calls=%d admission_timer_heap_max=%d "
               "(0=disabled)",
               cc_cfg_admission_max_calls(),
               cc_cfg_admission_timer_heap_max()));
    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] media_mode=%s",
               cc_cfg_media_mode_name()));
    if (cc_rtpengine_enabled()) {
        PJ_LOG(3, (THIS_FILE,
                   "[CONFIG] rtpengine ng=%s:%d timeout_ms=%d flags=%s dtmf_dest=%s media_dir=%s",
                   cc_cfg_rtpengine_host(),
                   cc_cfg_rtpengine_port(),
                   cc_cfg_rtpengine_timeout_ms(),
                   cc_cfg_rtpengine_flags(),
                   cc_cfg_rtpengine_dtmf_dest(),
                   cc_cfg_rtpengine_media_dir()[0] ? cc_cfg_rtpengine_media_dir() : "(local realpath)"));
        PJ_LOG(3, (THIS_FILE,
                   "[CONFIG] rtpengine loop_med_tp=on enable_loopback=off "
                   "(no local UDP RTP bind; SDP rewritten to RTPengine)"));
        if (cc_rtpengine_init() != PJ_SUCCESS) {
            PJ_LOG(1, (THIS_FILE,
                       "[RTPENGINE] ng client init failed — UPDATEs will fall back"));
        }
    }
    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] media thread_cnt=%d line_echo=%s ec_tail_len=%d "
               "jb_init=%d jb_min_pre=%d jb_max_pre=%d jb_max=%d",
               med_cfg.thread_cnt,
               cc_line_echo_enabled() ? "on" : "off",
               med_cfg.ec_tail_len,
               med_cfg.jb_init, med_cfg.jb_min_pre,
               med_cfg.jb_max_pre, med_cfg.jb_max));
    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] max_media_ports=%u",
               med_cfg.max_media_ports));

    /* ── 3. UDP transport ────────────────────────────────────────── */
    cc_prompt_mapping_load("wav/wav_mapping.conf");

    pjsua_transport_config_default(&tp_cfg);
    tp_cfg.port         = local_sip_port;
    tp_cfg.bound_addr   = pj_str((char *)local_host);
    tp_cfg.public_addr  = pj_str((char *)local_host);

    status = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &tp_cfg, NULL);
    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE, "UDP transport create failed: %d", status));
        pjsua_destroy();
        cc_app_logger_close();
        return 1;
    }
    PJ_LOG(3, (THIS_FILE, "UDP transport: %s:%d",
               local_host, local_sip_port));

    /* ── 4. TCP transport (IMS prefers TCP for large messages) ────── */
    pjsua_transport_config_default(&tp_cfg);
    tp_cfg.port        = local_sip_port;
    tp_cfg.bound_addr  = pj_str((char *)local_host);
    tp_cfg.public_addr = pj_str((char *)local_host);

    status = pjsua_transport_create(PJSIP_TRANSPORT_TCP, &tp_cfg, NULL);
    if (status != PJ_SUCCESS) {
        PJ_LOG(2, (THIS_FILE, "TCP transport create failed: %d "
                              "(continuing without TCP)", status));
    } else {
        PJ_LOG(3, (THIS_FILE, "TCP transport: %s:%d",
                   local_host, local_sip_port));
    }

    /* ── 5. Start PJSUA ──────────────────────────────────────────── */
    status = pjsua_start();
    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE, "pjsua_start() failed: %d", status));
        pjsua_destroy();
        cc_app_logger_close();
        return 1;
    }

    /*
     * Keep PJSUA's conference bridge and RTP media active on a headless
     * server without opening a physical/default audio device.
     */
    status = pjsua_set_null_snd_dev();
    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] [AUDIO] pjsua_set_null_snd_dev status=%d",
                   status));
    } else {
        PJ_LOG(3, (THIS_FILE,
                   "[AUDIO] pjsua_set_null_snd_dev status=%d",
                   status));
        PJ_LOG(3, (THIS_FILE, "[CONFIG] null sound device enabled"));
    }

/* Codec policy: single G.711 toward both legs (matches RE dummy answer +
 * codec-strip-PCMU/G722). Advertising both PCMA and PCMU caused RE to
 * egress interleaved PT 8+0 to softphones (noise/silence). Softphones
 * often use telephone-event PT 120; RE dummy advertises 120 (+101).
 */
{
    pjsua_codec_info codecs[32];
    unsigned count = PJ_ARRAY_SIZE(codecs);
    unsigned i;
    pj_str_t codec_id;
    pj_status_t cstatus;

    cstatus = pjsua_enum_codecs(codecs, &count);
    if (cstatus == PJ_SUCCESS) {
        for (i = 0; i < count; i++) {
            pjsua_codec_set_priority(&codecs[i].codec_id, 0);
        }
    }

    codec_id = pj_str("PCMA/8000");
    pjsua_codec_set_priority(&codec_id, 255);

    codec_id = pj_str("telephone-event/8000");
    pjsua_codec_set_priority(&codec_id, 254);

    PJ_LOG(3, (THIS_FILE,
               "Codec policy applied: PCMA/8000 + telephone-event/8000 only "
               "(PCMU/G722/Opus disabled; RE dummy DTMF PT 120+101)"));
}


    /* ── 6. Create account (no REGISTER — inline B2BUA) ─────────── */
    pjsua_acc_config_default(&acc_cfg);
    snprintf(acc_id_buf,
             sizeof(acc_id_buf),
             "sip:%s@%s:%d",
             cc_cfg_user_agent(),
             local_host,
             local_sip_port);
    acc_cfg.id           = pj_str(acc_id_buf);
    acc_cfg.reg_uri      = pj_str("");     /* no REGISTER */
    acc_cfg.register_on_acc_add = PJ_FALSE;
    acc_cfg.use_rfc5626  = 0;              /* disable ;ob in Contact */

    /* Configured RTP port range (used for update/local_bridge UDP media;
     * rtpengine mode uses loop_med_tp and does not bind these ports). */
    acc_cfg.rtp_cfg.port       = cc_cfg_rtp_port_start();
    acc_cfg.rtp_cfg.port_range = cc_cfg_rtp_port_count();
    /* Randomize to reduce EADDRINUSE clustering when scanning from a fixed start. */
    acc_cfg.rtp_cfg.randomize_port = cc_rtpengine_enabled() ? PJ_FALSE : PJ_TRUE;

    /*
     * rtpengine / third-party media (variant C1):
     * use_loop_med_tp — PJSUA creates pjmedia_transport_loop instead of UDP,
     * so no OS bind on :RTP_PORT_*. Placeholder addresses appear in SDP and
     * are rewritten to RTPengine in on_call_sdp_created.
     * enable_loopback=0 — do not echo packets into the local stream.
     */
    if (cc_rtpengine_enabled()) {
        acc_cfg.use_loop_med_tp = PJ_TRUE;
        acc_cfg.enable_loopback = PJ_FALSE;
    }

    /*
     * Session Timer (RFC 4028).
     * Without this the SBC's Session-Expires:16 kills every call at ~16s
     * because PJSUA never sends a refresh and the SBC tears down the dialog.
     * PJSUA_SIP_TIMER_ALWAYS: always include Session-Expires in our responses
     * and send refreshes.  If the SBC offers Session-Expires:16 we reply with
     * 422 Session Interval Too Small (min_se=90) forcing renegotiation to
     * sess_expires=1800.  PJSUA then owns the refresh and sends re-INVITEs
     * every 1800s, keeping the dialog alive indefinitely.
     */
    acc_cfg.use_timer                  = PJSUA_SIP_TIMER_ALWAYS;
    acc_cfg.timer_setting.sess_expires = 1800;
    acc_cfg.timer_setting.min_se       = 90;

    /*
     * Disable lock_codec. Default (1) sends a post-answer re-INVITE/UPDATE
     * that shrinks the codec list to a single payload. In rtpengine mode that
     * extra B re-INVITE is unnecessary (RE endpoint unchanged) and adds
     * fragile mid-dialog traffic; in update/local_bridge it also fights SDP
     * rewrite. Codec preference is already set via cc_tune_audio_codecs().
     */
    acc_cfg.lock_codec = 0;

    /* Outbound proxy — all B-leg INVITEs and in-dialog requests route
     * through Kamailio first.  PJSUA adds this as a Route header
     * automatically; we must NOT also add Route1 manually in b2bua.c. */
    {
        static char proxy_uri_buf[192];
        snprintf(proxy_uri_buf, sizeof(proxy_uri_buf),
                 "<sip:%s:%d;transport=udp;lr>",
                 cc_cfg_sbc_host(), cc_cfg_sbc_port());
        acc_cfg.proxy[0]  = pj_str(proxy_uri_buf);
        acc_cfg.proxy_cnt = 1;
    }

    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] session_timer=ALWAYS sess_expires=%u min_se=%u lock_codec=0",
               acc_cfg.timer_setting.sess_expires,
               acc_cfg.timer_setting.min_se));
    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] DTMF receive=RFC2833+SIP_INFO callback=on_dtmf_digit2 legacy_callback=enabled"));

    status = pjsua_acc_add(&acc_cfg, PJ_TRUE, &g_acc_id);
    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE, "pjsua_acc_add() failed: %d", status));
        pjsua_destroy();
        cc_app_logger_close();
        return 1;
    }

    PJ_LOG(3, (THIS_FILE,
               "Collect Call B2BUA ready (acc_id=%d) — waiting for calls...",
               g_acc_id));

    if (cc_worker_start() != 0) {
        PJ_LOG(1, (THIS_FILE, "[ERROR] worker pool start failed"));
        pjsua_destroy();
        cc_app_logger_close();
        return 1;
    }
    PJ_LOG(3, (THIS_FILE,
               "[WORKER] pools started (general=%d answer=%d)",
               CC_WORKER_POOL_SIZE, CC_ANSWER_WORKER_POOL_SIZE));

    if (cc_vasync_init() != 0) {
        PJ_LOG(1, (THIS_FILE, "[ERROR] async validation init failed"));
        cc_worker_stop();
        pjsua_destroy();
        cc_app_logger_close();
        return 1;
    }

    if (cc_endcall_udp_init() != 0) {
        PJ_LOG(1, (THIS_FILE, "[ERROR] end-call UDP socket init failed"));
        cc_vasync_destroy();
        cc_worker_stop();
        pjsua_destroy();
        cc_app_logger_close();
        return 1;
    }

#if CC_OPTIONS_ENABLE
#if CC_OPTIONS_PERIODIC_ENABLE
    cc_options_start_periodic();
#else
    cc_options_send_once(CC_OPTIONS_TARGET_URI);
#endif
#endif

    /* ── 7. Main event loop ──────────────────────────────────────── */
    /*
     * Blocking event loop using select() on the wakeup pipe +
     * pjsip_endpt_handle_events2() for SIP I/O and timers.
     *
     * select() blocks on the pipe read-fd with a timeout derived from
     * the earliest PJSIP timer. When a signal arrives, sig_handler writes
     * to the pipe and select() returns immediately.
     *
     * After select(), pjsip_endpt_handle_events2() is called with timeout=0
     * to process any pending SIP I/O and fire due timers without blocking.
     */
    {
        pjsip_endpoint *endpt = pjsua_get_pjsip_endpt();
        pj_timer_heap_t *timer_heap = pjsip_endpt_get_timer_heap(endpt);
        int pipe_fds[2];

        /* Port-drain diagnostics: track idle stretches and timer-heap depth */
        unsigned long  loop_total       = 0;  /* total handle_events2 calls */
        unsigned long  loop_idle        = 0;  /* calls that returned event_count=0 */
        unsigned long  loop_busy        = 0;  /* calls that returned event_count>0 */
        unsigned long  idle_streak      = 0;  /* consecutive idle iterations */
        unsigned long  idle_streak_max  = 0;  /* longest idle streak observed */
        pj_time_val    last_busy_time;
        pj_gettimeofday(&last_busy_time);

        if (pipe(pipe_fds) == 0) {
            fcntl(pipe_fds[1], F_SETFL,
                  fcntl(pipe_fds[1], F_GETFL) | O_NONBLOCK);
            g_wake_rfd = pipe_fds[0];
            g_wake_wfd = pipe_fds[1];
        }

        PJ_LOG(3, (THIS_FILE, "[LOOP] select-based event loop started, max_ms=%d",
                   CC_EVENT_LOOP_MAX_MS));

        while (g_running) {
            fd_set rfds;
            struct timeval tv;
            pj_time_val now;
            pj_time_val pj_timeout;
            unsigned event_count = 0;
            long wait_ms = CC_EVENT_LOOP_MAX_MS;
            int nfds;
            pj_size_t heap_size;

            /* Process pending SIP events first (non-blocking) */
            pj_timeout.sec  = 0;
            pj_timeout.msec = 0;
            pjsip_endpt_handle_events2(endpt, &pj_timeout, &event_count);
            loop_total++;

            if (event_count > 0) {
                /* Busy: reset idle streak, record last-busy timestamp */
                loop_busy++;
                if (idle_streak > idle_streak_max)
                    idle_streak_max = idle_streak;
                if (idle_streak >= 100) {
                    /* Transitioning from long idle back to busy — port drain
                     * backlog is now being processed again. Log the gap so
                     * we can correlate with netstat port counts. */
                    pj_gettimeofday(&now);
                    PJ_LOG(3, (THIS_FILE,
                               "[LOOP-DRAIN] busy resumed after %lu idle iters "
                               "idle_streak_max=%lu total=%lu busy=%lu idle=%lu",
                               idle_streak, idle_streak_max,
                               loop_total, loop_busy, loop_idle));
                }
                idle_streak = 0;
                pj_gettimeofday(&last_busy_time);
                continue;
            }

            /* Idle iteration */
            loop_idle++;
            idle_streak++;

            /* Log when we first go idle after a busy period — this is the
             * moment the port-drain backlog may start accumulating. */
            if (idle_streak == 1) {
                heap_size = pj_timer_heap_count(timer_heap);
                pj_gettimeofday(&now);
                PJ_LOG(3, (THIS_FILE,
                           "[LOOP-DRAIN] went idle: timer_heap_pending=%lu "
                           "total=%lu busy=%lu idle=%lu",
                           (unsigned long)heap_size,
                           loop_total, loop_busy, loop_idle));
            }

            /* Periodic idle log every 500 iterations (~5s at max_ms=10):
             * shows if timer heap is draining or stuck. */
            if (idle_streak % 500 == 0) {
                heap_size = pj_timer_heap_count(timer_heap);
                pj_gettimeofday(&now);
                PJ_LOG(3, (THIS_FILE,
                           "[LOOP-DRAIN] still idle: streak=%lu timer_heap_pending=%lu "
                           "total=%lu busy=%lu idle=%lu",
                           idle_streak, (unsigned long)heap_size,
                           loop_total, loop_busy, loop_idle));
            }

            /*
             * Do NOT call pj_timer_heap_earliest_time() here.
             *
             * pjlib does pj_assert(ht->cur_size != 0) *before* returning
             * PJ_ENOTFOUND, and the assert runs unlocked. With
             * ua_cfg.thread_cnt > 0 another PJSIP thread can cancel the
             * last timer between our observation and the call → SIGABRT.
             *
             * Evidence (instance_3, collect_call_20260911_154704_616946.log):
             * zero calls for ~5 min, LOOP-DRAIN always reported
             * timer_heap_pending=1, then abort at streak≈279k with
             * ht->cur_size==0. So the heap was not stably empty — it raced
             * to empty. Polling every CC_EVENT_LOOP_MAX_MS (10 ms) is enough
             * for SIP timer granularity and removes the crash.
             */

            /* Block in select() on the wakeup pipe */
            FD_ZERO(&rfds);
            if (g_wake_rfd >= 0)
                FD_SET(g_wake_rfd, &rfds);

            tv.tv_sec  = wait_ms / 1000;
            tv.tv_usec = (wait_ms % 1000) * 1000;

            nfds = (g_wake_rfd >= 0) ? g_wake_rfd + 1 : 0;
            select(nfds, &rfds, NULL, NULL, &tv);

            /* Drain wakeup pipe if signaled */
            if (g_wake_rfd >= 0 && FD_ISSET(g_wake_rfd, &rfds)) {
                char drain[16];
                ssize_t _ign = read(g_wake_rfd, drain, sizeof(drain));
                (void)_ign;
            }
        }

        /* Final loop stats on shutdown */
        PJ_LOG(3, (THIS_FILE,
                   "[LOOP-DRAIN] shutdown stats: total=%lu busy=%lu idle=%lu "
                   "idle_streak_max=%lu",
                   loop_total, loop_busy, loop_idle, idle_streak_max));

        if (g_wake_rfd >= 0) close(g_wake_rfd);
        if (g_wake_wfd >= 0) close(g_wake_wfd);
    }

    PJ_LOG(3, (THIS_FILE, "Shutting down..."));
#if CC_OPTIONS_ENABLE
    cc_options_stop_periodic();
#endif
    cc_vasync_destroy();
    cc_endcall_udp_destroy();
    cc_worker_stop();
    cc_rtpengine_shutdown();
    cc_session_pool_destroy();
    pjsua_destroy();
    cc_app_logger_close();
    return 0;
}
