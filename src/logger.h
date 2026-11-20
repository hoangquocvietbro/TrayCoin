#ifndef TRAYCOIN_LOGGER_H
#define TRAYCOIN_LOGGER_H

#include "types.h"

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} LogLevel;

void log_init(void);
void log_shutdown(void);
void log_set_enabled(int enabled);
int  log_is_enabled(void);
void log_get_path(wchar_t *outPath, int cap);
void log_clear(void);

/* Thread-safe formatted log entry (wide string). */
void log_write(LogLevel lvl, const wchar_t *tag, const wchar_t *fmt, ...);

/* Install global unhandled exception filter that always writes crash diagnostics. */
void log_install_crash_handler(void);

#define LOGD(tag, fmt, ...) log_write(LOG_DEBUG, tag, fmt, ##__VA_ARGS__)
#define LOGI(tag, fmt, ...) log_write(LOG_INFO,  tag, fmt, ##__VA_ARGS__)
#define LOGW(tag, fmt, ...) log_write(LOG_WARN,  tag, fmt, ##__VA_ARGS__)
#define LOGE(tag, fmt, ...) log_write(LOG_ERROR, tag, fmt, ##__VA_ARGS__)

#endif /* TRAYCOIN_LOGGER_H */
