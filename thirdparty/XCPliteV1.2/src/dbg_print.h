#pragma once
#define __DBG_PRINT_H__

/*----------------------------------------------------------------------------
| File:
|   dbg_print.h
|
| Description:
|   Debug logging
|
| Code released into public domain, no attribution required
|
 ----------------------------------------------------------------------------*/

#include "main_cfg.h" // for OPTION_ENABLE_DBG_PRINTS, OPTION_DEFAULT_DBG_LEVEL, OPTION_FIXED_DBG_LEVEL
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
void XcpLogWriteAt(uint8_t level, const char *file, int line, const char *function, const char *message);
void XcpLogWritefAt(uint8_t level, const char *file, int line, const char *function, const char *fmt, ...);
void XcpLogWriteWithPrefixAt(uint8_t level, const char *file, int line, const char *function, const char *prefix, const char *message);
void XcpLogWritefWithPrefixAt(uint8_t level, const char *file, int line, const char *function, const char *prefix, const char *fmt, ...);
void XcpLogLineAt(uint8_t level, const char *file, int line, const char *function, const char *message);
void XcpLogLinefAt(uint8_t level, const char *file, int line, const char *function, const char *fmt, ...);
#ifdef __cplusplus
}
#endif

#if defined(OPTION_ENABLE_DBG_PRINTS) && !defined(OPTION_FIXED_DBG_LEVEL) && !defined(OPTION_DEFAULT_DBG_LEVEL)
#error "Please define OPTION_DEFAULT_DBG_LEVEL or OPTION_FIXED_DBG_LEVEL"
#endif

#ifdef OPTION_ENABLE_DBG_METRICS
#ifdef __cplusplus
extern "C" {
#endif
extern uint32_t gXcpWritePendingCount;
extern uint32_t gXcpCalSegPublishAllCount;
extern uint32_t gXcpDaqEventCount;
extern uint32_t gXcpTxPacketCount;
extern uint32_t gXcpRxPacketCount;
#ifdef __cplusplus
}
#endif
#endif

#ifdef OPTION_ENABLE_DBG_PRINTS

/*
1 - Error
2 - Warn
3 - Info
4 - Trace
5 - Debug
*/

// ANSI color codes
#define ANSI_COLOR_RED "\x1b[31m"
#define ANSI_COLOR_YELLOW "\x1b[33m"
#define ANSI_COLOR_GREEN "\x1b[32m"
#define ANSI_COLOR_BLUE "\x1b[34m"
#define ANSI_COLOR_RESET "\x1b[0m"

#ifdef OPTION_FIXED_DBG_LEVEL
#define DBG_LEVEL OPTION_FIXED_DBG_LEVEL
#else
#ifdef __cplusplus
extern "C" {
#endif
extern uint8_t gXcpDebugLevel;
#ifdef __cplusplus
}
#endif
#define DBG_LEVEL gXcpDebugLevel
#endif

#define DBG_PRINTF(format, ...)                                                                                                                                                   \
    do {                                                                                                                                                                           \
        if (DBG_LEVEL >= 3)                                                                                                                                                        \
            XcpLogWritefAt(3, __FILE__, __LINE__, __func__, format, __VA_ARGS__);                                                                                                  \
    } while (0)

#ifdef OPTION_ENABLE_DBG_STDERR
#define DBG_PRINTF_ERROR(format, ...)                                                                                                                                              \
    do {                                                                                                                                                                           \
        if (DBG_LEVEL >= 1)                                                                                                                                                        \
            XcpLogWritefWithPrefixAt(1, __FILE__, __LINE__, __func__, ANSI_COLOR_RED "[XCP  ] ERROR: " ANSI_COLOR_RESET, format, __VA_ARGS__);                                    \
    } while (0)
#define DBG_PRINTF_WARNING(format, ...)                                                                                                                                            \
    do {                                                                                                                                                                           \
        if (DBG_LEVEL >= 2)                                                                                                                                                        \
            XcpLogWritefWithPrefixAt(2, __FILE__, __LINE__, __func__, ANSI_COLOR_YELLOW "[XCP  ] WARNING: " ANSI_COLOR_RESET, format, __VA_ARGS__);                              \
    } while (0)
#else
#define DBG_PRINTF_ERROR(format, ...)                                                                                                                                              \
    do {                                                                                                                                                                           \
        if (DBG_LEVEL >= 1)                                                                                                                                                        \
            XcpLogWritefWithPrefixAt(1, __FILE__, __LINE__, __func__, ANSI_COLOR_RED "[XCP  ] ERROR: " ANSI_COLOR_RESET, format, __VA_ARGS__);                                    \
    } while (0)
#define DBG_PRINTF_WARNING(format, ...)                                                                                                                                            \
    do {                                                                                                                                                                           \
        if (DBG_LEVEL >= 2)                                                                                                                                                        \
            XcpLogWritefWithPrefixAt(2, __FILE__, __LINE__, __func__, ANSI_COLOR_YELLOW "[XCP  ] WARNING: " ANSI_COLOR_RESET, format, __VA_ARGS__);                              \
    } while (0)
#endif
#define DBG_PRINTF3(format, ...)                                                                                                                                                   \
    do {                                                                                                                                                                           \
        if (DBG_LEVEL >= 3)                                                                                                                                                        \
            XcpLogWritefWithPrefixAt(3, __FILE__, __LINE__, __func__, "[XCP  ] ", format, __VA_ARGS__);                                                                           \
    } while (0)
#define DBG_PRINTF4(format, ...)                                                                                                                                                   \
    do {                                                                                                                                                                           \
        if (DBG_LEVEL >= 4)                                                                                                                                                        \
            XcpLogWritefWithPrefixAt(4, __FILE__, __LINE__, __func__, "[XCP  ] ", format, __VA_ARGS__);                                                                           \
    } while (0)
#define DBG_PRINTF5(format, ...)                                                                                                                                                   \
    do {                                                                                                                                                                           \
        if (DBG_LEVEL >= 5)                                                                                                                                                        \
            XcpLogWritefWithPrefixAt(5, __FILE__, __LINE__, __func__, "[XCP  ] ", format, __VA_ARGS__);                                                                           \
    } while (0)

// Convenience macros for format-only strings (no additional arguments)
#define DBG_PRINT(format)                                                                                                                                                         \
    do {                                                                                                                                                                           \
        if (DBG_LEVEL >= 3)                                                                                                                                                        \
            XcpLogWriteAt(3, __FILE__, __LINE__, __func__, format);                                                                                                                \
    } while (0)

#ifdef OPTION_ENABLE_DBG_STDERR
#define DBG_PRINT_ERROR(format)                                                                                                                                                    \
    do {                                                                                                                                                                           \
        if (DBG_LEVEL >= 1)                                                                                                                                                        \
            XcpLogWriteWithPrefixAt(1, __FILE__, __LINE__, __func__, ANSI_COLOR_RED "[XCP  ] ERROR: " ANSI_COLOR_RESET, format);                                                  \
    } while (0)
#define DBG_PRINT_WARNING(format)                                                                                                                                                  \
    do {                                                                                                                                                                           \
        if (DBG_LEVEL >= 2)                                                                                                                                                        \
            XcpLogWriteWithPrefixAt(2, __FILE__, __LINE__, __func__, ANSI_COLOR_YELLOW "[XCP  ] WARNING: " ANSI_COLOR_RESET, format);                                             \
    } while (0)
#else
#define DBG_PRINT_ERROR(format)                                                                                                                                                    \
    do {                                                                                                                                                                           \
        if (DBG_LEVEL >= 1)                                                                                                                                                        \
            XcpLogWriteWithPrefixAt(1, __FILE__, __LINE__, __func__, ANSI_COLOR_RED "[XCP  ] ERROR: " ANSI_COLOR_RESET, format);                                                  \
    } while (0)
#define DBG_PRINT_WARNING(format)                                                                                                                                                  \
    do {                                                                                                                                                                           \
        if (DBG_LEVEL >= 2)                                                                                                                                                        \
            XcpLogWriteWithPrefixAt(2, __FILE__, __LINE__, __func__, ANSI_COLOR_YELLOW "[XCP  ] WARNING: " ANSI_COLOR_RESET, format);                                             \
    } while (0)
#endif
#define DBG_PRINT3(format)                                                                                                                                                         \
    do {                                                                                                                                                                           \
        if (DBG_LEVEL >= 3)                                                                                                                                                        \
            XcpLogWriteWithPrefixAt(3, __FILE__, __LINE__, __func__, "[XCP  ] ", format);                                                                                         \
    } while (0)
#define DBG_PRINT4(format)                                                                                                                                                         \
    do {                                                                                                                                                                           \
        if (DBG_LEVEL >= 4)                                                                                                                                                        \
            XcpLogWriteWithPrefixAt(4, __FILE__, __LINE__, __func__, "[XCP  ] ", format);                                                                                         \
    } while (0)
#define DBG_PRINT5(format)                                                                                                                                                         \
    do {                                                                                                                                                                           \
        if (DBG_LEVEL >= 5)                                                                                                                                                        \
            XcpLogWriteWithPrefixAt(5, __FILE__, __LINE__, __func__, "[XCP  ] ", format);                                                                                         \
    } while (0)

#else

#undef DBG_LEVEL

#define DBG_PRINTF(s, ...)
#define DBG_PRINTF_ERROR(s, ...) // printf(s,__VA_ARGS__)
#define DBG_PRINTF_WARNING(s, ...)
#define DBG_PRINTF3(s, ...)
#define DBG_PRINTF4(s, ...)
#define DBG_PRINTF5(s, ...)

#define DBG_PRINT(s)
#define DBG_PRINT_ERROR(s) // printf(s,__VA_ARGS__)
#define DBG_PRINT_WARNING(s)
#define DBG_PRINT3(s)
#define DBG_PRINT4(s)
#define DBG_PRINT5(s)

#endif
