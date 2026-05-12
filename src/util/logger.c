/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "util/logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

static struct {
    logger_config_t config;
    FILE           *file_fp;
    bool            initialized;
    pthread_mutex_t lock;
} g_logger = {
    .initialized = false,
    .lock        = PTHREAD_MUTEX_INITIALIZER,
};

static const char *level_str[] = {
    [LOG_LEVEL_ERROR] = "ERROR",
    [LOG_LEVEL_WARN]  = "WARN ",
    [LOG_LEVEL_INFO]  = "INFO ",
    [LOG_LEVEL_DEBUG]  = "DEBUG",
};

static const char *level_color[] = {
    [LOG_LEVEL_ERROR] = "\033[31m",   /* red */
    [LOG_LEVEL_WARN]  = "\033[33m",   /* yellow */
    [LOG_LEVEL_INFO]  = "\033[32m",   /* green */
    [LOG_LEVEL_DEBUG]  = "\033[36m",  /* cyan */
};

#define COLOR_RESET "\033[0m"

int logger_init(const logger_config_t *config)
{
    if (!config)
        return -1;

    pthread_mutex_lock(&g_logger.lock);

    if (g_logger.initialized && g_logger.file_fp) {
        fclose(g_logger.file_fp);
        g_logger.file_fp = NULL;
    }

    g_logger.config = *config;
    g_logger.file_fp = NULL;

    if (config->enable_file && config->log_file) {
        g_logger.file_fp = fopen(config->log_file, "a");
        if (!g_logger.file_fp) {
            pthread_mutex_unlock(&g_logger.lock);
            return -1;
        }
    }

    g_logger.initialized = true;
    pthread_mutex_unlock(&g_logger.lock);
    return 0;
}

int logger_init_default(void)
{
    logger_config_t cfg = {
        .level             = LOG_LEVEL_INFO,
        .enable_console    = true,
        .enable_file       = false,
        .enable_timestamp  = true,
        .enable_colors     = true,
        .log_file          = NULL,
    };
    return logger_init(&cfg);
}

void logger_cleanup(void)
{
    pthread_mutex_lock(&g_logger.lock);
    if (g_logger.file_fp) {
        fclose(g_logger.file_fp);
        g_logger.file_fp = NULL;
    }
    g_logger.initialized = false;
    pthread_mutex_unlock(&g_logger.lock);
}

void logger_set_level(log_level_t level)
{
    pthread_mutex_lock(&g_logger.lock);
    g_logger.config.level = level;
    pthread_mutex_unlock(&g_logger.lock);
}

log_level_t logger_get_level(void)
{
    pthread_mutex_lock(&g_logger.lock);
    log_level_t level = g_logger.config.level;
    pthread_mutex_unlock(&g_logger.lock);
    return level;
}

bool logger_is_level_enabled(log_level_t level)
{
    pthread_mutex_lock(&g_logger.lock);
    bool enabled = g_logger.initialized && (level <= g_logger.config.level);
    pthread_mutex_unlock(&g_logger.lock);
    return enabled;
}

void logger_log(log_level_t level, const char *file, int line,
                const char *func, const char *fmt, ...)
{
    if (!logger_is_level_enabled(level))
        return;

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_logger.lock);

    char ts[32] = "";
    if (g_logger.config.enable_timestamp) {
        struct timespec tp;
        clock_gettime(CLOCK_REALTIME, &tp);
        struct tm tm_info;
        localtime_r(&tp.tv_sec, &tm_info);
        snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d.%03ld ",
                 tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
                 tp.tv_nsec / 1000000L);
    }

    if (g_logger.config.enable_console) {
        FILE *out = (level == LOG_LEVEL_ERROR) ? stderr : stdout;
        if (level == LOG_LEVEL_ERROR) {
            if (g_logger.config.enable_colors) {
                fprintf(out, "%s%s[%s:%d %s] %s%s\n",
                        level_color[level], ts, file, line, func, msg, COLOR_RESET);
            } else {
                fprintf(out, "%s[%s:%d %s] %s\n",
                        ts, file, line, func, msg);
            }
        } else {
            if (g_logger.config.enable_colors) {
                fprintf(out, "%s%s[%s] %s%s\n",
                        level_color[level], ts, level_str[level], msg, COLOR_RESET);
            } else {
                fprintf(out, "%s[%s] %s\n", ts, level_str[level], msg);
            }
        }
        fflush(out);
    }

    if (g_logger.config.enable_file && g_logger.file_fp) {
        /* If the file has reached 20 MB, truncate and start fresh */
        long pos = ftell(g_logger.file_fp);
        if (pos >= 0 && pos >= 20 * 1024 * 1024) {
            FILE *new_fp = freopen(g_logger.config.log_file, "w", g_logger.file_fp);
            if (new_fp == NULL) {
                /* freopen closed the old stream; reopen from scratch */
                g_logger.file_fp = fopen(g_logger.config.log_file, "a");
            } else {
                g_logger.file_fp = new_fp;
            }
        }
        if (g_logger.file_fp) {
            if (level == LOG_LEVEL_ERROR) {
                fprintf(g_logger.file_fp, "%s[%s:%d %s] %s\n",
                        ts, file, line, func, msg);
            } else {
                fprintf(g_logger.file_fp, "%s[%s] %s\n", ts, level_str[level], msg);
            }
            fflush(g_logger.file_fp);
        }
    }

    pthread_mutex_unlock(&g_logger.lock);
}
