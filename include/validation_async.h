#ifndef CC_VALIDATION_ASYNC_H
#define CC_VALIDATION_ASYNC_H

#include "validation.h"

/*
 * Async UDP validation dispatcher.
 *
 * Single non-blocking UDP socket + epoll thread.
 * Up to 512 concurrent in-flight validations.
 * Response matched by callId field in JSON.
 * Callback fired on a worker thread (not the epoll thread).
 */

typedef void (*cc_vasync_callback_t)(void *cb_arg,
                                     cc_validation_result_t *result);

/* Internal: payload for CC_EV_VASYNC_CB worker event */
typedef struct {
    cc_vasync_callback_t     cb;
    void                    *cb_arg;
    cc_validation_result_t   result;
} vasync_cb_event_t;

int  cc_vasync_init(void);
void cc_vasync_destroy(void);

/*
 * Send validation request asynchronously.
 * Returns 0 on success (request sent, callback will fire).
 * Returns -1 on error (callback will NOT fire — caller must handle).
 */
int cc_udp_validate_async(const char *caller_msisdn,
                           const char *sponsor_msisdn,
                           const char *call_id,
                           const char *source,
                           const char *timestamp,
                           cc_vasync_callback_t cb,
                           void *cb_arg);

#endif /* CC_VALIDATION_ASYNC_H */
