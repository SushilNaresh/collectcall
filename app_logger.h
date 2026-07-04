#ifndef CC_APP_LOGGER_H
#define CC_APP_LOGGER_H

int cc_app_logger_init(void);
void cc_app_logger_install_pj_writer(void);
void cc_app_logger_writer(int level, const char *data, int len);
void cc_app_logger_close(void);
const char *cc_app_logger_path(void);
const char *cc_app_logger_dir(void);

#endif /* CC_APP_LOGGER_H */
