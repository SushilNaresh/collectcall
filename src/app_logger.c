#include "app_logger.h"
#include "config.h"

#include <pj/log.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define CC_LOG_PATH_MAX 1024

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_log_file = NULL;
static char g_log_dir[CC_LOG_PATH_MAX] = "";
static char g_log_path[CC_LOG_PATH_MAX] = "console-only";

static int ensure_one_directory(const char *path)
{
    struct stat st;

    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode))
            return 0;
        errno = ENOTDIR;
        return -1;
    }

    if (errno != ENOENT)
        return -1;

    if (mkdir(path, 0750) == 0)
        return 0;

    if (errno == EEXIST &&
        stat(path, &st) == 0 &&
        S_ISDIR(st.st_mode))
    {
        return 0;
    }

    return -1;
}

static int ensure_directory_tree(const char *directory)
{
    char path[CC_LOG_PATH_MAX];
    char *p;
    size_t len;

    if (!directory || directory[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    len = strlen(directory);
    if (len >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(path, directory, len + 1);
    while (len > 1 && path[len - 1] == '/')
        path[--len] = '\0';

    for (p = path + 1; *p != '\0'; ++p) {
        if (*p != '/' || p[-1] == '/')
            continue;

        *p = '\0';
        if (ensure_one_directory(path) != 0)
            return -1;
        *p = '/';
    }

    return ensure_one_directory(path);
}

static int set_effective_directory(void)
{
    const char *configured = CC_APP_LOG_DIR;
    size_t len;

    if (!configured || configured[0] == '\0')
        configured = "logs";

    len = strlen(configured);
    if (len >= sizeof(g_log_dir)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(g_log_dir, configured, len + 1);
    while (len > 1 && g_log_dir[len - 1] == '/')
        g_log_dir[--len] = '\0';

    return 0;
}

int cc_app_logger_init(void)
{
#if CC_APP_LOG_ENABLE
    char timestamp[32];
    struct tm local_tm;
    time_t now;
    int path_len;
    int fd;
    FILE *file;

    if (set_effective_directory() != 0) {
        fprintf(stderr, "[LOGGER] invalid log directory: %s\n",
                strerror(errno));
        return -1;
    }

    if (ensure_directory_tree(g_log_dir) != 0) {
        fprintf(stderr,
                "[LOGGER] cannot create/use log directory '%s': %s; "
                "continuing with console logging\n",
                g_log_dir, strerror(errno));
        return -1;
    }

    now = time(NULL);
    if (localtime_r(&now, &local_tm) == NULL ||
        strftime(timestamp, sizeof(timestamp),
                 "%Y%m%d_%H%M%S", &local_tm) == 0)
    {
        fprintf(stderr,
                "[LOGGER] cannot generate timestamp; "
                "continuing with console logging\n");
        return -1;
    }

    path_len = snprintf(g_log_path, sizeof(g_log_path),
                        "%s/%s_%s_%ld.log",
                        g_log_dir, CC_APP_LOG_PREFIX,
                        timestamp, (long)getpid());
    if (path_len < 0 || (size_t)path_len >= sizeof(g_log_path)) {
        snprintf(g_log_path, sizeof(g_log_path), "%s", "console-only");
        fprintf(stderr,
                "[LOGGER] log path is too long; "
                "continuing with console logging\n");
        return -1;
    }

    fd = open(g_log_path, O_WRONLY | O_CREAT | O_APPEND,
              CC_APP_LOG_FILE_MODE);
    if (fd < 0) {
        fprintf(stderr,
                "[LOGGER] cannot open log file '%s': %s; "
                "continuing with console logging\n",
                g_log_path, strerror(errno));
        snprintf(g_log_path, sizeof(g_log_path), "%s", "console-only");
        return -1;
    }

    file = fdopen(fd, "a");
    if (!file) {
        int saved_errno = errno;
        close(fd);
        fprintf(stderr,
                "[LOGGER] cannot create log stream: %s; "
                "continuing with console logging\n",
                strerror(saved_errno));
        snprintf(g_log_path, sizeof(g_log_path), "%s", "console-only");
        return -1;
    }

    setvbuf(file, NULL, _IOLBF, 0);

    pthread_mutex_lock(&g_log_lock);
    g_log_file = file;
    pthread_mutex_unlock(&g_log_lock);
#else
    if (set_effective_directory() != 0)
        snprintf(g_log_dir, sizeof(g_log_dir), "%s", "logs");
#endif

    return 0;
}

void cc_app_logger_writer(int level, const char *data, int len)
{
    (void)level;

    if (!data || len <= 0)
        return;

    pthread_mutex_lock(&g_log_lock);

#if CC_APP_LOG_ENABLE
    if (g_log_file) {
        fwrite(data, 1, (size_t)len, g_log_file);
#if CC_APP_LOG_FLUSH_ALWAYS
        fflush(g_log_file);
#endif
    }
#endif

#if CC_APP_LOG_TO_CONSOLE
    fwrite(data, 1, (size_t)len, stdout);
#if CC_APP_LOG_FLUSH_ALWAYS
    fflush(stdout);
#endif
#endif

    pthread_mutex_unlock(&g_log_lock);
}

void cc_app_logger_install_pj_writer(void)
{
    pj_log_set_log_func(&cc_app_logger_writer);
}

void cc_app_logger_close(void)
{
    pthread_mutex_lock(&g_log_lock);
    if (g_log_file) {
        fflush(g_log_file);
        fclose(g_log_file);
        g_log_file = NULL;
    }
    pthread_mutex_unlock(&g_log_lock);
}

const char *cc_app_logger_path(void)
{
    return g_log_path;
}

const char *cc_app_logger_dir(void)
{
    return g_log_dir[0] ? g_log_dir : "logs";
}
