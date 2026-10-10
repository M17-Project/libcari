/*
 * libcari - logging hook
 *
 * The library never prints on its own; it reports through a single
 * process-wide handler (default: messages at WARN or above go to stderr).
 */
#ifndef CARI_LOG_H
#define CARI_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CARI_LOG_DEBUG = 0,
    CARI_LOG_INFO,
    CARI_LOG_WARN,
    CARI_LOG_ERROR
} cari_log_level_t;

typedef void (*cari_log_fn)(void *user, cari_log_level_t lvl, const char *msg);

/* fn == NULL restores the default handler. min_level filters messages. */
void cari_log_set_handler(cari_log_fn fn, void *user, cari_log_level_t min_level);
void cari_log(cari_log_level_t lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif
#endif
