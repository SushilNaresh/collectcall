/*
 * main.c — PJSUA C-API bootstrap for the Collect Call B2BUA
 *
 * Initialises PJSUA with operator-grade settings:
 *   - No VAD, G.711 narrowband, RFC 2833 DTMF
 *   - UDP + TCP SIP transports
 *   - No SIP REGISTER (inline B2BUA, routed by IMS core)
 *   - pjsua_callback wired to global cc_* handlers
 */
#include "b2bua.h"
#include "app_logger.h"
#include "config.h"
#include "env_loader.h"
#include "options.h"
#include "prompt_mapping.h"
#include "runtime_config.h"
#include "utils.h"

#include <pjsua-lib/pjsua.h>
#include <pjsip/sip_endpoint.h>
#include <pjsip/sip_config.h>
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
    if (g_wake_wfd >= 0)
        (void)write(g_wake_wfd, &byte, 1);
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
    PJ_LOG(3, (THIS_FILE,
               "[APP] start pid=%ld log_dir=%s log_file=%s",
               (long)getpid(),
               cc_app_logger_dir(),
               cc_app_logger_path()));

    /* ── 2. Configure ────────────────────────────────────────────── */
    pjsua_config_default(&ua_cfg);
    ua_cfg.max_calls = CC_MAX_CALLS;
    ua_cfg.user_agent = pj_str((char *)cc_cfg_user_agent());

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

    pjsua_logging_config_default(&log_cfg);
    log_cfg.level         = CC_LOG_LEVEL;
    log_cfg.console_level = 0;
    log_cfg.cb            = &cc_app_logger_writer;

    	pjsua_media_config_default(&med_cfg);
	/* Bypass test: allow silence suppression so PJSUA does not keep forcing RTP to VM */
        med_cfg.no_vad      = PJ_FALSE;

	med_cfg.clock_rate  = CC_CLOCK_RATE;    /* 8 kHz G.711             */
	med_cfg.snd_clock_rate = CC_CLOCK_RATE;

	/* RTP start port must be configurable for production firewall/operator setup. */
	/*med_cfg.port = CC_RTP_PORT_START; // not available in this installed PJSUA verison */

    status = pjsua_init(&ua_cfg, &log_cfg, &med_cfg);
    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE, "pjsua_init() failed: %d", status));
        pjsua_destroy();
        cc_app_logger_close();
        return 1;
    }

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

    cc_app_logger_install_pj_writer();
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
               CC_RTP_PORT_START,
               CC_RTP_PORT_START + CC_RTP_PORT_COUNT - 1,
               CC_RTP_PORT_COUNT));
    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] validation=%s:%d timeout_ms=%d",
               cc_cfg_validation_host(),
               cc_cfg_validation_port(),
               CC_VALIDATION_TIMEOUT_MS));
    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] initiate_source=%s service_key_mode=%s",
               CC_INITIATE_SOURCE,
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
               "[CONFIG] max_call_legs=%d",
               CC_MAX_CALLS));
    PJ_LOG(3, (THIS_FILE,
               "[CONFIG] media_mode=%s",
               cc_cfg_media_mode_name()));

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

/* Codec policy: keep SDP small and operator-friendly.
 * Disable all codecs, then enable only PCMA/8000 and telephone-event/8000.
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

    codec_id = pj_str("PCMU/8000");
    pjsua_codec_set_priority(&codec_id, 253);

    codec_id = pj_str("telephone-event/8000");
    pjsua_codec_set_priority(&codec_id, 254);

    PJ_LOG(3, (THIS_FILE, "Codec policy applied: PCMA/8000 + PCMU/8000 + telephone-event/8000"));
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

    /* Configured RTP port range */
    acc_cfg.rtp_cfg.port = CC_RTP_PORT_START;
    acc_cfg.rtp_cfg.port_range = CC_RTP_PORT_COUNT;
    acc_cfg.rtp_cfg.randomize_port = PJ_FALSE;


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
            pj_time_val earliest;
            pj_time_val pj_timeout;
            unsigned event_count = 0;
            long wait_ms = CC_EVENT_LOOP_MAX_MS;
            int nfds;

            /* Process pending SIP events first (non-blocking) */
            pj_timeout.sec  = 0;
            pj_timeout.msec = 0;
            pjsip_endpt_handle_events2(endpt, &pj_timeout, &event_count);

            /* If events were processed, loop immediately — no sleep */
            if (event_count > 0)
                continue;

            /* No events: determine sleep time from next PJSIP timer */
            if (pj_timer_heap_earliest_time(timer_heap,
                                            &earliest) == PJ_SUCCESS) {
                pj_gettimeofday(&now);
                PJ_TIME_VAL_SUB(earliest, now);

                if (earliest.sec < 0 ||
                    (earliest.sec == 0 && earliest.msec <= 0))
                {
                    wait_ms = 1; /* timer due — brief yield then re-process */
                } else {
                    long timer_ms = earliest.sec * 1000 + earliest.msec;
                    if (timer_ms < wait_ms)
                        wait_ms = timer_ms;
                }
            }

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
                (void)read(g_wake_rfd, drain, sizeof(drain));
            }
        }

        if (g_wake_rfd >= 0) close(g_wake_rfd);
        if (g_wake_wfd >= 0) close(g_wake_wfd);
    }

    PJ_LOG(3, (THIS_FILE, "Shutting down..."));
#if CC_OPTIONS_ENABLE
    cc_options_stop_periodic();
#endif
    pjsua_destroy();
    cc_app_logger_close();
    return 0;
}
