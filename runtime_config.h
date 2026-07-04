#ifndef CC_RUNTIME_CONFIG_H
#define CC_RUNTIME_CONFIG_H

typedef enum {
    CC_PREFIX_MODE_STRIP_REQUIRED = 0,
    CC_PREFIX_MODE_ALLOW_ALREADY_STRIPPED
} cc_prefix_mode_t;

typedef enum {
    CC_SERVICE_KEY_MODE_DISABLED = 0,
    CC_SERVICE_KEY_MODE_FROM_ONLY,
    CC_SERVICE_KEY_MODE_REQUEST_URI,
    CC_SERVICE_KEY_MODE_REQUEST_URI_AND_FROM
} cc_service_key_mode_t;

typedef enum {
    CC_MEDIA_MODE_UPDATE = 0,
    CC_MEDIA_MODE_REINVITE,
    CC_MEDIA_MODE_LOCAL_BRIDGE
} cc_media_mode_t;

const char *cc_cfg_local_host(void);
int         cc_cfg_local_sip_port(void);

const char *cc_cfg_sbc_host(void);
int         cc_cfg_sbc_port(void);
const char *cc_cfg_sbc_next_hop(void);

const char *cc_cfg_collect_prefixes(void);
const char *cc_cfg_default_country_code(void);

cc_prefix_mode_t cc_cfg_prefix_mode(void);
const char       *cc_cfg_prefix_mode_name(void);

cc_service_key_mode_t cc_cfg_service_key_mode(void);
const char            *cc_cfg_service_key_mode_name(void);
const char            *cc_cfg_service_key_placeholder(void);

const char *cc_cfg_validation_host(void);
int         cc_cfg_validation_port(void);

const char *cc_cfg_endcall_host(void);
int         cc_cfg_endcall_port(void);

const char *cc_cfg_pani_value(void);

cc_media_mode_t cc_cfg_media_mode(void);
const char     *cc_cfg_media_mode_name(void);

#endif /* CC_RUNTIME_CONFIG_H */
