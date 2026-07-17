#ifndef CC_PROMPT_MAPPING_H
#define CC_PROMPT_MAPPING_H

typedef enum {
    CC_PROMPT_WAITING = 0,
    CC_PROMPT_COLLECT_PROMPT,
    CC_PROMPT_REJECTED,
    CC_PROMPT_UNAVAILABLE,
    CC_PROMPT_LOW_BALANCE,
    CC_PROMPT_BUSY,
    CC_PROMPT_NOT_AVAILABLE_TO_PAY,
    CC_PROMPT_MCA_SENT,
    CC_PROMPT_MCA_NOT_SENT,
    CC_PROMPT_FUNDLESS,
    CC_PROMPT_COUNT
} cc_prompt_tag_t;

int cc_prompt_mapping_load(const char *path);
const char *cc_prompt_get_path(cc_prompt_tag_t tag);
const char *cc_prompt_tag_name(cc_prompt_tag_t tag);

#endif /* CC_PROMPT_MAPPING_H */
