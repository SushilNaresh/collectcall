#ifndef CC_API_MAPPING_H
#define CC_API_MAPPING_H

#include "session.h"

const char *cc_api_eligibility_status_from_udp(int udp_status);
const char *cc_api_status_code_from_udp(int udp_status);
const char *cc_api_reason_description_from_udp(int udp_status);
const char *cc_api_details_from_udp(int udp_status);

void cc_log_initiate_api_mapping(const char *caller_msisdn,
                                 const char *sponsor_msisdn,
                                 const char *call_id,
                                 const char *source,
                                 const char *timestamp,
                                 int udp_status);

void cc_map_end_call_result(const char *internal_status,
                            const char *internal_reason,
                            const char **api_status,
                            const char **api_reason);

void cc_send_end_call_udp(cc_session_t *session);

#endif /* CC_API_MAPPING_H */
