/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#pragma once

#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>

enum log_level {
  LOG_LEVEL_DEBUG = 0,
  LOG_LEVEL_INFO,
  LOG_LEVEL_WARN,
  LOG_LEVEL_ERROR,
};

void logger_set_level(enum log_level level);
enum log_level logger_get_level(void);

/* Initialize log level from RXAPP_LOG_LEVEL if present. */
void logger_init_from_env(void);

#define LOGGER_MAX_BYTES    (20u * 1024u * 1024u)  /* rotate the log file at this size */
#define LOGGER_MAX_ARCHIVES 10                     /* keep <log>.1.gz .. <log>.10.gz */

/* Direct log output to a file.  When file_path is NULL or empty, logs go
 * to stderr (console).  When the file reaches max_bytes it is gzip-compressed
 * in the background to <file>.1.gz (older archives shift up, the oldest beyond
 * LOGGER_MAX_ARCHIVES is deleted) and a new file is started.  Unsafe paths
 * (see logger_path_is_safe) are refused and logging stays on the console. */
void logger_set_file(const char* file_path, size_t max_bytes);

/* True when an output file may be created at path: its directory and every
 * parent are not world-writable (rejects /tmp, /var/tmp, /dev/shm) and are
 * owned by root or the invoking user, and path is not a symlink or special
 * file.  On false, a reason is written to why. */
bool logger_path_is_safe(const char* path, char* why, size_t why_len);

/* Close the log file (if any) and revert to console output. */
void logger_close(void);

/* Parse string level (debug|info|warn|error). Returns true on success. */
bool logger_parse_level(const char* level_str, enum log_level* out_level);

void logger_vlog(enum log_level level, const char* tag, const char* fmt, va_list args);
void logger_log(enum log_level level, const char* tag, const char* fmt, ...);

#define log_debug(tag, fmt, ...) logger_log(LOG_LEVEL_DEBUG, (tag), (fmt), ##__VA_ARGS__)
#define log_info(tag, fmt, ...)  logger_log(LOG_LEVEL_INFO,  (tag), (fmt), ##__VA_ARGS__)
#define log_warn(tag, fmt, ...)  logger_log(LOG_LEVEL_WARN,  (tag), (fmt), ##__VA_ARGS__)
#define log_error(tag, fmt, ...) logger_log(LOG_LEVEL_ERROR, (tag), (fmt), ##__VA_ARGS__)
