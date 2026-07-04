#ifndef CC_OPTIONS_H
#define CC_OPTIONS_H

int cc_options_send_once(const char *target_uri);
int cc_options_start_periodic(void);
void cc_options_stop_periodic(void);

#endif /* CC_OPTIONS_H */
