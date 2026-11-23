/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Log levels for the logger
 */
typedef enum {
    LOG_LEVEL_ERROR = 0,  /**< Error messages */
    LOG_LEVEL_WARN  = 1,  /**< Warning messages */
    LOG_LEVEL_INFO  = 2,  /**< Info messages */
    LOG_LEVEL_DEBUG = 3,  /**< Debug messages */
} log_level_t;

/**
 * @brief Logger configuration structure
 */
typedef struct {
    log_level_t level;        /**< Current log level */
    bool enable_console;      /**< Enable console output */
    bool enable_file;         /**< Enable file output */
    bool enable_timestamp;    /**< Enable timestamp in logs */
    bool enable_colors;       /**< Enable colors in console output */
    const char *log_file;     /**< Log file path */
} logger_config_t;

/**
 * @brief Initialize the logger with configuration
 * 
 * @param config Logger configuration
 * @return 0 on success, -1 on failure
 */
int logger_init(const logger_config_t *config);

/**
 * @brief Initialize the logger with default configuration
 * 
 * @return 0 on success, -1 on failure
 */
int logger_init_default(void);

/**
 * @brief Cleanup and close the logger
 */
void logger_cleanup(void);

/**
 * @brief Set the logging level
 * 
 * @param level New log level
 */
void logger_set_level(log_level_t level);

/**
 * @brief Get the current logging level
 * 
 * @return Current log level
 */
log_level_t logger_get_level(void);

/**
 * @brief Log a message at specified level
 * 
 * @param level Log level
 * @param file Source file name
 * @param line Line number
 * @param func Function name
 * @param fmt Format string
 * @param ... Variable arguments
 */
void logger_log(log_level_t level, const char *file, int line, 
                const char *func, const char *fmt, ...);

/**
 * @brief Check if a log level is enabled
 * 
 * @param level Log level to check
 * @return true if enabled, false otherwise
 */
bool logger_is_level_enabled(log_level_t level);

/**
 * @brief Override the log rotation size threshold (default 20 MB).
 *        Intended for unit testing only.
 *
 * @param size_bytes New rotation threshold in bytes
 */
void logger_set_max_size(long size_bytes);

/**
 * @brief Tell the logger that stdout/stderr have been dup2'd to the log file.
 *        After log rotation, the logger will re-dup2 them to the new file.
 */
void logger_set_stdout_redirected(bool redirected);

/* Convenience macros for logging */
#define LOG_ERROR(fmt, ...) \
    logger_log(LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_WARN(fmt, ...) \
    logger_log(LOG_LEVEL_WARN, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_INFO(fmt, ...) \
    logger_log(LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_DEBUG(fmt, ...) \
    logger_log(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* LOGGER_H */
