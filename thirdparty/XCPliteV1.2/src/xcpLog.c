#include "xcplib.h"
#include "main_cfg.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L) && !defined(__STDC_NO_ATOMICS__) && !defined(_MSC_VER)
#define XCP_LOG_USE_ATOMICS 1
#include <stdatomic.h>
#endif

#define XCP_LOG_BUFFER_SIZE 1024u
#define XCP_LOG_CALLBACK_BUFFER_SIZE 1152u

#ifdef XCP_LOG_USE_ATOMICS
static atomic_uintptr_t gXcpLogCallbackPtr;
#else
static tXcpLogCallback gXcpLogCallback;
#endif

static bool xcp_log_ends_with_newline(const char *message) {
    size_t len;

    if (message == NULL) return false;
    len = strlen(message);
    return len > 0u && message[len - 1u] == '\n';
}

static tXcpLogCallback xcp_log_get_callback(void) {
#ifdef XCP_LOG_USE_ATOMICS
    return (tXcpLogCallback)(uintptr_t)atomic_load_explicit(&gXcpLogCallbackPtr, memory_order_relaxed);
#else
    return gXcpLogCallback;
#endif
}

static void xcp_log_default_write(uint8_t level, const char *prefix, const char *message, bool line) {
    FILE *stream = stdout;

#ifdef OPTION_ENABLE_DBG_STDERR
    if (level <= 2u) {
        stream = stderr;
    }
#endif

    fputs(prefix != NULL ? prefix : "", stream);
    fputs(message != NULL ? message : "", stream);
    if (line && !xcp_log_ends_with_newline(message)) {
        fputc('\n', stream);
    }
}

static void xcp_log_callback_write(tXcpLogCallback cb, uint8_t level, const char *file, int line, const char *function, const char *prefix, const char *message) {
    char buffer[XCP_LOG_CALLBACK_BUFFER_SIZE];
    const char *p = prefix != NULL ? prefix : "";
    const char *m = message != NULL ? message : "";

    snprintf(buffer, sizeof(buffer), "%s%s", p, m);
    cb(level, file, line, function, buffer);
}

static void xcp_log_emit(uint8_t level, const char *file, int line, const char *function, const char *prefix, const char *message, bool append_newline) {
    tXcpLogCallback cb = xcp_log_get_callback();

    if (cb != NULL) {
        xcp_log_callback_write(cb, level, file, line, function, prefix, message);
        return;
    }

    xcp_log_default_write(level, prefix, message, append_newline);
}

static void xcp_log_vwrite(uint8_t level, const char *file, int line, const char *function, const char *prefix, bool append_newline, const char *fmt, va_list ap) {
    char buffer[XCP_LOG_BUFFER_SIZE];
    int n;

    if (fmt == NULL) {
        xcp_log_emit(level, file, line, function, prefix, "", append_newline);
        return;
    }

    n = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    if (n < 0) {
        xcp_log_emit(level, file, line, function, prefix, fmt, append_newline);
        return;
    }
    if ((size_t)n >= sizeof(buffer)) {
        buffer[sizeof(buffer) - 1u] = '\0';
    }

    xcp_log_emit(level, file, line, function, prefix, buffer, append_newline);
}

void XcpSetLogCallback(tXcpLogCallback cb) {
#ifdef XCP_LOG_USE_ATOMICS
    atomic_store_explicit(&gXcpLogCallbackPtr, (uintptr_t)cb, memory_order_release);
#else
    gXcpLogCallback = cb;
#endif
}

void XcpLogWriteAt(uint8_t level, const char *file, int line, const char *function, const char *message) {
    xcp_log_emit(level, file, line, function, NULL, message, false);
}

void XcpLogWritefAt(uint8_t level, const char *file, int line, const char *function, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    xcp_log_vwrite(level, file, line, function, NULL, false, fmt, ap);
    va_end(ap);
}

void XcpLogWriteWithPrefixAt(uint8_t level, const char *file, int line, const char *function, const char *prefix, const char *message) {
    xcp_log_emit(level, file, line, function, prefix, message, false);
}

void XcpLogWritefWithPrefixAt(uint8_t level, const char *file, int line, const char *function, const char *prefix, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    xcp_log_vwrite(level, file, line, function, prefix, false, fmt, ap);
    va_end(ap);
}

void XcpLogLineAt(uint8_t level, const char *file, int line, const char *function, const char *message) {
    xcp_log_emit(level, file, line, function, NULL, message, true);
}

void XcpLogLinefAt(uint8_t level, const char *file, int line, const char *function, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    xcp_log_vwrite(level, file, line, function, NULL, true, fmt, ap);
    va_end(ap);
}
