#ifndef CC_VALIDATION_H
#define CC_VALIDATION_H

/*
 * UDP validation client.
 *
 * Sends the Signalling_CC_APIs_V0.1 initiate payload to local udp2http.
 */
#define CC_VALIDATION_ALLOW                  0
#define CC_VALIDATION_CALLER_BLACKLISTED     1
#define CC_VALIDATION_SPONSOR_BALANCE_FAIL   2
#define CC_VALIDATION_API_FAILURE            3
#define CC_VALIDATION_SPONSOR_DND_ACTIVE     4
#define CC_VALIDATION_SPONSOR_ROAMING        5

typedef struct {
    int  status;          /* CC_VALIDATION_* or -1 for transport error */
    char reason[128];
    char details[256];
    char reason_description[256];
    char service_key[64]; /* optional; empty when omitted */
} cc_validation_result_t;

int cc_udp_validate_call(const char *caller_msisdn,
                         const char *sponsor_msisdn,
                         const char *call_id,
                         const char *source,
                         const char *timestamp,
                         cc_validation_result_t *result);

#endif /* CC_VALIDATION_H */
