/* libcari - logging hook */
#include "cari/cari_log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

static pthread_mutex_t log_mtx = PTHREAD_MUTEX_INITIALIZER;
static cari_log_fn     log_fn;
static void           *log_user;
static cari_log_level_t log_min = CARI_LOG_WARN;

void cari_log_set_handler(cari_log_fn fn, void *user, cari_log_level_t min_level)
{
    pthread_mutex_lock(&log_mtx);
    log_fn = fn;
    log_user = user;
    log_min = min_level;
    pthread_mutex_unlock(&log_mtx);
}

void cari_log(cari_log_level_t lvl, const char *fmt, ...)
{
    char msg[512];
    va_list ap;

    pthread_mutex_lock(&log_mtx);
    cari_log_fn fn = log_fn;
    void *user = log_user;
    cari_log_level_t min = log_min;
    pthread_mutex_unlock(&log_mtx);
    if (lvl < min)
        return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (fn) {
        fn(user, lvl, msg);
    } else {
        static const char *names[] = { "debug", "info", "warn", "error" };
        fprintf(stderr, "libcari %s: %s\n", names[lvl & 3], msg);
    }
}
