/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "logger.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

static _Atomic enum log_level g_log_level = LOG_LEVEL_INFO;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE* g_log_file = NULL;          /* NULL → use stderr */
static char g_log_file_path[512] = {0};
static size_t g_log_max_bytes = 0;

/* Background gzip of a rotated log; g_gz_running is protected by g_log_lock. */
static pthread_t g_gz_thread;
static bool g_gz_running = false;
static atomic_bool g_gz_done = false;
static time_t g_gz_last_retry = 0;   /* protected by g_log_lock */

static const char *level_color[] = {
    [LOG_LEVEL_ERROR] = "\033[31m",  /* red    */
    [LOG_LEVEL_WARN]  = "\033[33m",  /* yellow */
    [LOG_LEVEL_INFO]  = "\033[32m",  /* green  */
    [LOG_LEVEL_DEBUG] = "\033[36m",  /* cyan   */
};
static const char *color_reset = "\033[0m";

static const char* level_to_text(enum log_level level) {
  switch (level) {
    case LOG_LEVEL_DEBUG:
      return "DEBUG";
    case LOG_LEVEL_INFO:
      return "INFO";
    case LOG_LEVEL_WARN:
      return "WARN";
    case LOG_LEVEL_ERROR:
      return "ERROR";
    default:
      return "INFO";
  }
}

static void to_lower_copy(const char* src, char* dst, size_t dst_size) {
  if (!src || !dst || dst_size == 0) return;

  size_t i = 0;
  for (; src[i] != '\0' && i < dst_size - 1; i++) {
    dst[i] = (char)tolower((unsigned char)src[i]);
  }
  dst[i] = '\0';
}

bool logger_parse_level(const char* level_str, enum log_level* out_level) {
  if (!level_str || !out_level) return false;

  char norm[16];
  to_lower_copy(level_str, norm, sizeof(norm));

  if (strcmp(norm, "debug") == 0) {
    *out_level = LOG_LEVEL_DEBUG;
    return true;
  }
  if (strcmp(norm, "info") == 0) {
    *out_level = LOG_LEVEL_INFO;
    return true;
  }
  if (strcmp(norm, "warn") == 0 || strcmp(norm, "warning") == 0) {
    *out_level = LOG_LEVEL_WARN;
    return true;
  }
  if (strcmp(norm, "error") == 0) {
    *out_level = LOG_LEVEL_ERROR;
    return true;
  }

  return false;
}

void logger_set_level(enum log_level level) { atomic_store(&g_log_level, level); }

enum log_level logger_get_level(void) { return atomic_load(&g_log_level); }

bool logger_path_is_safe(const char* path, char* why, size_t why_len) {
  char dir[PATH_MAX], real[PATH_MAX];

  if (!path || path[0] == '\0') {
    snprintf(why, why_len, "empty path");
    return false;
  }
  const char* slash = strrchr(path, '/');
  if (slash && slash[1] == '\0') {
    snprintf(why, why_len, "'%s' is a directory", path);
    return false;
  }
  if (!slash)
    snprintf(dir, sizeof(dir), ".");
  else if (slash == path)
    snprintf(dir, sizeof(dir), "/");
  else
    snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);

  if (!realpath(dir, real)) {
    snprintf(why, why_len, "directory '%s' does not exist", dir);
    return false;
  }

  const char* sudo_uid_env = getenv("SUDO_UID");
  uid_t sudo_uid = sudo_uid_env ? (uid_t)strtoul(sudo_uid_env, NULL, 10) : (uid_t)-1;
  for (;;) {
    struct stat st;
    if (stat(real, &st) != 0) {
      snprintf(why, why_len, "cannot stat '%s'", real);
      return false;
    }
    if (st.st_mode & S_IWOTH) {
      snprintf(why, why_len, "'%s' is world-writable (e.g. /tmp); use a private directory", real);
      return false;
    }
    if (st.st_uid != 0 && st.st_uid != geteuid() && st.st_uid != sudo_uid) {
      snprintf(why, why_len, "'%s' is owned by another user (uid %u)", real, (unsigned)st.st_uid);
      return false;
    }
    if (strcmp(real, "/") == 0) break;
    char* s = strrchr(real, '/');
    if (s == real) s[1] = '\0';
    else *s = '\0';
  }

  struct stat lst;
  if (lstat(path, &lst) == 0 && !S_ISREG(lst.st_mode)) {
    snprintf(why, why_len, "'%s' is not a regular file (symlink or device)", path);
    return false;
  }
  return true;
}

/* Append-open without following symlinks; only regular files, mode 0640. */
static FILE* open_log_file(const char* path) {
  int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0640);
  if (fd < 0) return NULL;
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    return NULL;
  }
  FILE* f = fdopen(fd, "a");
  if (!f) close(fd);
  return f;
}

static int gzip_file(const char* src, const char* dst) {
  char tmp[PATH_MAX + 8];
  snprintf(tmp, sizeof(tmp), "%s.tmp", dst);

  int in = open(src, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (in < 0) return -1;
  unlink(tmp);
  int out = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0640);
  if (out < 0) {
    close(in);
    return -1;
  }
  gzFile gz = gzdopen(out, "wb6");
  if (!gz) {
    close(out);
    close(in);
    unlink(tmp);
    return -1;
  }

  int rc = 0;
  char buf[64 * 1024];
  ssize_t n;
  while ((n = read(in, buf, sizeof(buf))) > 0) {
    if (gzwrite(gz, buf, (unsigned)n) != (int)n) {
      rc = -1;
      break;
    }
  }
  if (n < 0) rc = -1;
  if (gzclose(gz) != Z_OK) rc = -1; /* also closes out */
  close(in);
  if (rc == 0 && rename(tmp, dst) != 0) rc = -1;
  if (rc != 0) unlink(tmp);
  return rc;
}

/* gzip <base>.rotating to a temp archive first; only on success shift <base>.N.gz up by
 * one (dropping the oldest) and install it as <base>.1.gz.  On failure nothing is
 * shifted or deleted and <base>.rotating stays for a later retry. */
static void* gz_worker(void* arg) {
  char* base = arg;
  char a[PATH_MAX], b[PATH_MAX], pending[PATH_MAX], fresh[PATH_MAX];

  snprintf(pending, sizeof(pending), "%s.rotating", base);
  snprintf(fresh, sizeof(fresh), "%s.new.gz", base);
  if (gzip_file(pending, fresh) != 0) {
    fprintf(stderr, "logger: cannot compress %s, will retry\n", pending);
    unlink(fresh);
    free(base);
    atomic_store(&g_gz_done, true);
    return NULL;
  }

  snprintf(a, sizeof(a), "%s.%d.gz", base, LOGGER_MAX_ARCHIVES);
  unlink(a);
  for (int i = LOGGER_MAX_ARCHIVES - 1; i >= 1; i--) {
    snprintf(a, sizeof(a), "%s.%d.gz", base, i);
    snprintf(b, sizeof(b), "%s.%d.gz", base, i + 1);
    if (rename(a, b) != 0 && errno != ENOENT)
      fprintf(stderr, "logger: cannot rename %s: %s\n", a, strerror(errno));
  }
  snprintf(a, sizeof(a), "%s.1.gz", base);
  if (rename(fresh, a) == 0)
    unlink(pending);
  else
    fprintf(stderr, "logger: cannot install %s: %s\n", a, strerror(errno));

  free(base);
  atomic_store(&g_gz_done, true);
  return NULL;
}

/* Must be called with g_log_lock held. */
static void start_archiver(void) {
  char* base = strdup(g_log_file_path);
  if (!base) return;
  atomic_store(&g_gz_done, false);
  if (pthread_create(&g_gz_thread, NULL, gz_worker, base) == 0) {
    g_gz_running = true;
  } else {
    gz_worker(base); /* no thread: compress inline */
  }
}

/* Must be called with g_log_lock held. */
static void join_archiver(void) {
  if (g_gz_running) {
    pthread_join(g_gz_thread, NULL);
    g_gz_running = false;
  }
}

void logger_set_file(const char* file_path, size_t max_bytes) {
  pthread_mutex_lock(&g_log_lock);

  /* Close any previously opened log file */
  if (g_log_file) {
    fclose(g_log_file);
    g_log_file = NULL;
  }
  g_log_file_path[0] = '\0';
  g_log_max_bytes = 0;

  if (file_path && file_path[0] != '\0') {
    char why[PATH_MAX + 96];
    if (!logger_path_is_safe(file_path, why, sizeof(why))) {
      fprintf(stderr, "logger: refusing log file '%s': %s; using console\n", file_path, why);
    } else if (strlen(file_path) >= sizeof(g_log_file_path)) {
      fprintf(stderr, "logger: log file path too long, using console\n");
    } else if ((g_log_file = open_log_file(file_path)) != NULL) {
      snprintf(g_log_file_path, sizeof(g_log_file_path), "%s", file_path);
      g_log_max_bytes = max_bytes;
      /* Archive a rotation interrupted by a previous crash. */
      char pending[PATH_MAX];
      struct stat st;
      snprintf(pending, sizeof(pending), "%s.rotating", file_path);
      if (!g_gz_running && lstat(pending, &st) == 0 && S_ISREG(st.st_mode)) start_archiver();
    } else {
      /* Cannot open — fall through to stderr; print a notice. */
      fprintf(stderr, "logger: cannot open log file '%s': %s; using console\n",
              file_path, strerror(errno));
    }
  }

  pthread_mutex_unlock(&g_log_lock);
}

void logger_close(void) {
  pthread_mutex_lock(&g_log_lock);
  if (g_log_file) {
    fclose(g_log_file);
    g_log_file = NULL;
  }
  g_log_file_path[0] = '\0';
  join_archiver();
  pthread_mutex_unlock(&g_log_lock);
}

void logger_init_from_env(void) {
  const char* level = getenv("RXAPP_LOG_LEVEL");
  enum log_level parsed;

  if (!level) return;

  if (logger_parse_level(level, &parsed)) {
    logger_set_level(parsed);
  }
}

/* Rotate when the file exceeds the limit: rename it aside, reopen a fresh file
 * immediately, and gzip the old one on a background thread so callers (RX and
 * display threads) never wait for compression.  Must be called with g_log_lock held. */
static void check_and_rotate(void) {
  if (!g_log_file || g_log_max_bytes == 0) return;

  long pos = ftell(g_log_file);
  if (pos < 0) {
    /* ftell failed – try stat as fallback */
    struct stat st;
    if (stat(g_log_file_path, &st) == 0)
      pos = (long)st.st_size;
    else
      return;
  }
  if ((size_t)pos < g_log_max_bytes) return;

  if (g_gz_running) {
    if (!atomic_load(&g_gz_done)) return; /* previous archive still compressing: keep appending */
    join_archiver();
  }

  char pending[PATH_MAX];
  snprintf(pending, sizeof(pending), "%s.rotating", g_log_file_path);
  struct stat pst;
  if (lstat(pending, &pst) == 0) {
    /* An earlier archive failed to compress: never overwrite it.  Keep appending
     * and retry compressing it at most every 10 s. */
    time_t now = time(NULL);
    if (now - g_gz_last_retry >= 10) {
      g_gz_last_retry = now;
      start_archiver();
    }
    return;
  }
  fclose(g_log_file);
  g_log_file = NULL;
  bool renamed = rename(g_log_file_path, pending) == 0;
  g_log_file = open_log_file(g_log_file_path);
  if (!renamed) {
    if (g_log_file)
      fprintf(g_log_file, "--- log rotation failed: %s ---\n", strerror(errno));
    return;
  }
  if (g_log_file) {
    fprintf(g_log_file, "--- log rotated: previous %ld bytes -> %s.1.gz (keeping %d archives) ---\n",
            pos, g_log_file_path, LOGGER_MAX_ARCHIVES);
    fflush(g_log_file);
  }
  start_archiver();
}

void logger_vlog(enum log_level level, const char* tag, const char* fmt, va_list args) {
  if (level < atomic_load(&g_log_level)) return;

  time_t now = time(NULL);
  struct tm tm_now;
  char time_buf[20];

  localtime_r(&now, &tm_now);
  strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_now);

  pthread_mutex_lock(&g_log_lock);

  FILE* out = g_log_file ? g_log_file : stderr;

  if (g_log_file) check_and_rotate();

  /* Re-read out in case check_and_rotate changed g_log_file */
  out = g_log_file ? g_log_file : stderr;

  bool use_color = (!g_log_file && level <= LOG_LEVEL_ERROR);

  if (use_color) fputs(level_color[level], out);
  fprintf(out, "%s [%s] [%s] ", time_buf, level_to_text(level), tag ? tag : "app");
  vfprintf(out, fmt, args);
  if (use_color) fputs(color_reset, out);
  fputc('\n', out);
  fflush(out);
  pthread_mutex_unlock(&g_log_lock);
}

void logger_log(enum log_level level, const char* tag, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  logger_vlog(level, tag, fmt, args);
  va_end(args);
}
