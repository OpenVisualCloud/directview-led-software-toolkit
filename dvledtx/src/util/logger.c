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
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define LOG_MAX_SIZE_DEFAULT (20L *1024 * 1024)  /* 20 MB */

static long g_log_max_size = LOG_MAX_SIZE_DEFAULT;
static bool g_stdout_redirected = false;

static struct {
    logger_config_t config;
    char            log_path[512];  /* owned copy of log file path */
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

/* =========================================================================
 * logger_rotate_file — archive the current log and open a fresh one.
 *
 * 1. Close the current log file.
 * 2. Rename it to <log_path>.<timestamp>  (e.g. dvledtx.log.2026-05-13_083000)
 * 3. Compress the renamed file with gzip  (produces .gz archive).
 * 4. Remove the uncompressed renamed copy.
 * 5. Open a new empty log file at the original path.
 *
 * Must be called with g_logger.lock held.
 * ========================================================================= */
static void logger_rotate_file(void)
{
    if (!g_logger.file_fp || g_logger.log_path[0] == '\0')
        return;

    fclose(g_logger.file_fp);
    g_logger.file_fp = NULL;

    /* Build timestamped archive name */
    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);
    struct tm tm_info;
    localtime_r(&tp.tv_sec, &tm_info);

    char ts_suffix[64];
    snprintf(ts_suffix, sizeof(ts_suffix), ".%04d-%02d-%02d_%02d%02d%02d",
             tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);

    char archived_path[600];
    snprintf(archived_path, sizeof(archived_path), "%s%s",
             g_logger.log_path, ts_suffix);

    /* Rename current log → timestamped name */
    if (rename(g_logger.log_path, archived_path) == 0) {
        /* Compress with gzip */
        pid_t pid = fork();
        if (pid == 0) {
            /* Child: exec gzip; on failure just _exit */
            execlp("gzip", "gzip", "-f", archived_path, (char *)NULL);
            _exit(1);
        } else if (pid > 0) {
            /* Parent: wait for gzip to finish so the .gz is ready
             * before any subsequent rotation attempts. */
            int status;
            waitpid(pid, &status, 0);
        }
    }
    /* If rename failed, the old file stays as-is; we still open fresh. */

    g_logger.file_fp = fopen(g_logger.log_path, "w");

    /* Re-redirect stdout/stderr to the new log file so that MTL library
     * output (which writes directly to stdout/stderr via dup2 in main.c)
     * continues to land in the active log file after rotation.
     * Only do this if main.c explicitly flagged that stdout was redirected. */
    if (g_logger.file_fp && g_stdout_redirected) {
        int fd = fileno(g_logger.file_fp);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
    }
}

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
    g_logger.log_path[0] = '\0';

    if (config->enable_file && config->log_file) {
        snprintf(g_logger.log_path, sizeof(g_logger.log_path), "%s",
                 config->log_file);
        g_logger.file_fp = fopen(g_logger.log_path, "a");
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

void logger_set_max_size(long size_bytes)
{
    g_log_max_size = (size_bytes > 0) ? size_bytes : LOG_MAX_SIZE_DEFAULT;
}

void logger_set_stdout_redirected(bool redirected)
{
    g_stdout_redirected = redirected;
}

void logger_log(log_level_t level, const char *file, int line,
                const char *func, const char *fmt, ...)
{
    (void)file;
    (void)line;
    (void)func;

    if (!logger_is_level_enabled(level))
        return;

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_logger.lock);

    char ts[64] = "";
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
        if (g_logger.config.enable_colors) {
            fprintf(out, "%s%s[%s] %s%s\n",
                    level_color[level], ts, level_str[level], msg, COLOR_RESET);
        } else {
            fprintf(out, "%s[%s] %s\n", ts, level_str[level], msg);
        }
        fflush(out);
    }

    if (g_logger.config.enable_file && g_logger.file_fp) {
        /* If the file has reached 20 MB, archive and start fresh */
        long pos = ftell(g_logger.file_fp);
        if (pos >= 0 && pos >= g_log_max_size) {
            logger_rotate_file();
        }
        if (g_logger.file_fp) {
            fprintf(g_logger.file_fp, "%s[%s] %s\n", ts, level_str[level], msg);
            fflush(g_logger.file_fp);
        }
    }

    pthread_mutex_unlock(&g_logger.lock);
}
