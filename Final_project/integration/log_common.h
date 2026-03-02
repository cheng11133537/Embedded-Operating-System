#ifndef LOG_COMMON_H
#define LOG_COMMON_H

#include <stdio.h>
#include <time.h>

static inline void log_current_timestamp(char *buf, size_t buf_len)
{
    time_t now = time(NULL);
    struct tm tm_info;

#if defined(_WIN32) || defined(_WIN64)
    localtime_s(&tm_info, &now);
#else
    localtime_r(&now, &tm_info);
#endif

    strftime(buf, buf_len, "%Y-%m-%d %H:%M:%S", &tm_info);
}

#define LOG_STRUCTURED(level, component, fmt, ...) do {               \
    char _log_time[32];                                               \
    log_current_timestamp(_log_time, sizeof(_log_time));             \
    fprintf(stderr, "[%s][%s][%s] " fmt "\n", _log_time, level, component, ##__VA_ARGS__); \
    fflush(stderr);                                                   \
} while (0)

#define LOG_INFO(component, fmt, ...)  LOG_STRUCTURED("INFO", component, fmt, ##__VA_ARGS__)
#define LOG_WARN(component, fmt, ...)  LOG_STRUCTURED("WARN", component, fmt, ##__VA_ARGS__)
#define LOG_ERROR(component, fmt, ...) LOG_STRUCTURED("ERROR", component, fmt, ##__VA_ARGS__)

#endif // LOG_COMMON_H
