/*
 * options.c - Optional out-of-dialog SIP OPTIONS health check.
 *
 * Disabled by default through config.h so normal collect-call flow is unchanged
 * unless an operator explicitly enables it.
 */
#include "options.h"
#include "config.h"
#include "runtime_config.h"

#include <pjsua-lib/pjsua.h>
#include <pj/log.h>
#include <pj/os.h>

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#define THIS_FILE "options.c"

static volatile int g_options_running = 0;
static int g_options_thread_started = 0;
static pthread_t g_options_thread;

static void options_response_cb(void *token, pjsip_event *e)
{
    (void)token;

    if (e && e->type == PJSIP_EVENT_TSX_STATE &&
        e->body.tsx_state.tsx &&
        e->body.tsx_state.tsx->status_code > 0)
    {
        PJ_LOG(3, (THIS_FILE, "[OPTIONS] Response status=%d",
                   e->body.tsx_state.tsx->status_code));
    }
}

int cc_options_send_once(const char *target_uri)
{
    pjsip_endpoint *endpt;
    pjsip_tx_data *tdata = NULL;
    pj_str_t target;
    pj_str_t from;
    pj_str_t to;
    pj_str_t contact;
    char from_buf[128];
    char contact_buf[128];
    pj_status_t status;

    if (!target_uri || !target_uri[0]) {
        PJ_LOG(1, (THIS_FILE, "[OPTIONS] SIP OPTIONS send failed: empty target"));
        return PJ_EINVAL;
    }

    endpt = pjsua_get_pjsip_endpt();
    if (!endpt) {
        PJ_LOG(1, (THIS_FILE, "[OPTIONS] SIP OPTIONS send failed: no endpoint"));
        return PJ_EINVAL;
    }

    snprintf(from_buf,
             sizeof(from_buf),
             "sip:collectcall@%s:%d",
             cc_cfg_local_host(),
             cc_cfg_local_sip_port());
    snprintf(contact_buf, sizeof(contact_buf), "sip:collectcall@%s:%d",
             cc_cfg_local_host(), cc_cfg_local_sip_port());

    target = pj_str((char *)target_uri);
    to = target;
    from = pj_str(from_buf);
    contact = pj_str(contact_buf);

    PJ_LOG(3, (THIS_FILE, "[OPTIONS] Sending SIP OPTIONS to %s", target_uri));

    status = pjsip_endpt_create_request(endpt,
                                        &pjsip_options_method,
                                        &target,
                                        &from,
                                        &to,
                                        &contact,
                                        NULL,
                                        -1,
                                        NULL,
                                        &tdata);
    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE, "[OPTIONS] SIP OPTIONS send failed: %d",
                   status));
        return status;
    }

    status = pjsip_endpt_send_request(endpt, tdata, -1, NULL,
                                      &options_response_cb);
    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE, "[OPTIONS] SIP OPTIONS send failed: %d",
                   status));
        return status;
    }

    PJ_LOG(3, (THIS_FILE, "[OPTIONS] SIP OPTIONS sent to %s", target_uri));
    return PJ_SUCCESS;
}

static void *options_periodic_thread(void *arg)
{
    pj_thread_desc desc;
    pj_thread_t *this_thread = NULL;
    int interval = CC_OPTIONS_INTERVAL_SEC;

    (void)arg;

    pj_bzero(desc, sizeof(desc));
    pj_thread_register("cc_options", desc, &this_thread);

    if (interval <= 0) {
        interval = 30;
    }

    while (g_options_running) {
        int i;

        cc_options_send_once(CC_OPTIONS_TARGET_URI);

        for (i = 0; i < interval && g_options_running; ++i) {
            sleep(1);
        }
    }

    return NULL;
}

int cc_options_start_periodic(void)
{
    int rc;

    if (g_options_thread_started) {
        return 0;
    }

    g_options_running = 1;
    rc = pthread_create(&g_options_thread, NULL, options_periodic_thread, NULL);
    if (rc != 0) {
        g_options_running = 0;
        PJ_LOG(1, (THIS_FILE, "[OPTIONS] Periodic OPTIONS start failed: %d",
                   rc));
        return rc;
    }

    g_options_thread_started = 1;
    PJ_LOG(3, (THIS_FILE,
               "[OPTIONS] Periodic OPTIONS started interval=%d target=%s",
               CC_OPTIONS_INTERVAL_SEC, CC_OPTIONS_TARGET_URI));
    return 0;
}

void cc_options_stop_periodic(void)
{
    if (!g_options_thread_started) {
        return;
    }

    g_options_running = 0;
    pthread_join(g_options_thread, NULL);
    g_options_thread_started = 0;

    PJ_LOG(3, (THIS_FILE, "[OPTIONS] Periodic OPTIONS stopped"));
}
