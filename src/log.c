/* log.c — leveled logging. Format mirrors Python:
 *   normal:  '%(asctime)s %(levelname)s [%(name)s] %(message)s'
 *   wire:    '%(asctime)s %(message)s'
 * asctime is 'YYYY-MM-DD HH:MM:SS,mmm' (comma + milliseconds), as Python does. */
#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

static int g_level = LOG_INFO;
static int g_wire  = 0;

static const char *level_name(int lvl)
{
    switch (lvl) {
        case LOG_DEBUG:   return "DEBUG";
        case LOG_INFO:    return "INFO";
        case LOG_WARNING: return "WARNING";
        default:          return "ERROR";
    }
}

int log_level_from_str(const char *s)
{
    if (!s) return -1;
    if (!strcasecmp(s, "DEBUG"))   return LOG_DEBUG;
    if (!strcasecmp(s, "INFO"))    return LOG_INFO;
    if (!strcasecmp(s, "WARNING")) return LOG_WARNING;
    if (!strcasecmp(s, "ERROR"))   return LOG_ERROR;
    return -1;
}

void log_init(int level, int wire_mode)
{
    g_level = level;
    g_wire  = wire_mode;
}

const char *log_hex(const unsigned char *buf, int len)
{
    static char hexbuf[2050];
    static const char *d = "0123456789abcdef";
    int n = len;
    if (n > 1024) n = 1024;             /* cap to buffer */
    for (int i = 0; i < n; i++) {
        hexbuf[i*2]   = d[buf[i] >> 4];
        hexbuf[i*2+1] = d[buf[i] & 0xF];
    }
    hexbuf[n*2] = 0;
    return hexbuf;
}

static void timestamp(char *buf, size_t n)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    char base[32];
    strftime(base, sizeof base, "%Y-%m-%d %H:%M:%S", &tm);
    snprintf(buf, n, "%s,%03d", base, (int)(tv.tv_usec / 1000));
}

void log_msg(int level, const char *name, const char *fmt, ...)
{
    /* In wire mode the root logger sits at WARNING. */
    int threshold = g_wire ? LOG_WARNING : g_level;
    if (level < threshold)
        return;

    char ts[40];
    timestamp(ts, sizeof ts);
    fprintf(stderr, "%s %s [%s] ", ts, level_name(level), name);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void log_wire(const char *name, const char *fmt, ...)
{
    (void)name;
    if (!g_wire)
        return;
    char ts[40];
    timestamp(ts, sizeof ts);
    fprintf(stderr, "%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
