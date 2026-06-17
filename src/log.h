/* log.h — leveled logging matching the Python logging output format. */
#ifndef LOG_H
#define LOG_H

enum { LOG_DEBUG = 0, LOG_INFO, LOG_WARNING, LOG_ERROR };

/* Initialise logging.  wire_mode mirrors --wire: only ipsc.wire / hbp.wire at
 * DEBUG with a bare timestamp format; everything else is silenced below WARNING. */
void log_init(int level, int wire_mode);

/* Parse "DEBUG"|"INFO"|"WARNING"|"ERROR" → level, or -1 if invalid. */
int log_level_from_str(const char *s);

/* Format bytes as lowercase hex into a static buffer (single-threaded use). */
const char *log_hex(const unsigned char *buf, int len);

/* Log a message under logger `name` at `level`. */
void log_msg(int level, const char *name, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Wire-format log (raw hex), only emitted in wire mode. `name` is ipsc.wire/hbp.wire. */
void log_wire(const char *name, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define LOGD(name, ...) log_msg(LOG_DEBUG,   name, __VA_ARGS__)
#define LOGI(name, ...) log_msg(LOG_INFO,    name, __VA_ARGS__)
#define LOGW(name, ...) log_msg(LOG_WARNING, name, __VA_ARGS__)
#define LOGE(name, ...) log_msg(LOG_ERROR,   name, __VA_ARGS__)

#endif
