/* log.c — leveled logging with optional tee-file for build logs. */
#include "common.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static enum gf_log_level g_level = GF_LOG_INFO;
static FILE *g_tee = NULL;
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;

void gf_log_set_level(enum gf_log_level level)
{
    g_level = level;
}

enum gf_log_level gf_log_level(void)
{
    return g_level;
}

void gf_log_set_tee(const char *path)
{
    gf_log_close_tee();
    if (path) {
        g_tee = fopen(path, "a");
        /* A failed tee open is non-fatal: log to stderr only. */
    }
}

void gf_log_close_tee(void)
{
    if (g_tee) {
        fclose(g_tee);
        g_tee = NULL;
    }
}

static const char *level_tag(enum gf_log_level lvl)
{
    switch (lvl) {
    case GF_LOG_DEBUG: return "debug";
    case GF_LOG_INFO:  return "info";
    case GF_LOG_WARN:  return "warn";
    case GF_LOG_ERROR: return "error";
    default:           return "error";
    }
}

void gf_log(enum gf_log_level lvl, const char *fmt, ...)
{
    if (lvl < g_level)
        return;

    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(msg))
        n = (int)sizeof(msg) - 1;

    char stamp[21] = {0};
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        struct tm tm;
        time_t sec = ts.tv_sec;
        if (gmtime_r(&sec, &tm))
            strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm);
    }

    pthread_mutex_lock(&g_log_mu);
    fprintf(stderr, "%s gitfull: %s: %s\n", stamp, level_tag(lvl), msg);
    fflush(stderr);
    if (g_tee) {
        fprintf(g_tee, "%sZ %s: %s\n", stamp, level_tag(lvl), msg);
        fflush(g_tee);
    }
    pthread_mutex_unlock(&g_log_mu);
}

void gf_log_errno(enum gf_log_level lvl, const char *what)
{
    int e = errno;
    gf_log(lvl, "%s: %s", what, strerror(e));
}
