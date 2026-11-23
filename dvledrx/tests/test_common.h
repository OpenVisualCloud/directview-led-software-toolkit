/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Minimal unit-test helpers (no external framework).  Each test is a void
 * function; CHECK* record a failure and continue, REQUIRE* stop the test.
 */

#pragma once

#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_checks, g_failures, g_test_failed;
static const char* g_test_name;

#define CHECK(cond)                                                              \
  do {                                                                           \
    g_checks++;                                                                  \
    if (!(cond)) {                                                               \
      g_failures++;                                                              \
      g_test_failed = 1;                                                         \
      fprintf(stderr, "  FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__, g_test_name, \
              #cond);                                                            \
    }                                                                            \
  } while (0)

#define CHECK_EQ_INT(a, b)                                                          \
  do {                                                                              \
    long long a_ = (long long)(a), b_ = (long long)(b);                             \
    g_checks++;                                                                     \
    if (a_ != b_) {                                                                 \
      g_failures++;                                                                 \
      g_test_failed = 1;                                                            \
      fprintf(stderr, "  FAIL %s:%d [%s]: %s == %s (%lld != %lld)\n", __FILE__,     \
              __LINE__, g_test_name, #a, #b, a_, b_);                               \
    }                                                                               \
  } while (0)

#define CHECK_EQ_STR(a, b)                                                             \
  do {                                                                                 \
    const char *a_ = (a), *b_ = (b);                                                   \
    g_checks++;                                                                        \
    if (!a_ || !b_ || strcmp(a_, b_) != 0) {                                           \
      g_failures++;                                                                    \
      g_test_failed = 1;                                                               \
      fprintf(stderr, "  FAIL %s:%d [%s]: %s == %s (\"%s\" != \"%s\")\n", __FILE__,    \
              __LINE__, g_test_name, #a, #b, a_ ? a_ : "(null)", b_ ? b_ : "(null)"); \
    }                                                                                  \
  } while (0)

#define REQUIRE(cond)             \
  do {                            \
    int before_ = g_failures;     \
    CHECK(cond);                  \
    if (g_failures != before_) return; \
  } while (0)

#define RUN_TEST(fn)                                              \
  do {                                                            \
    g_test_name = #fn;                                            \
    g_test_failed = 0;                                            \
    fn();                                                         \
    fprintf(stderr, "%s %s\n", g_test_failed ? "FAIL" : "ok  ", #fn); \
  } while (0)

static int test_summary(void) {
  fprintf(stderr, "%d checks, %d failure(s)\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}

/* Private (0700) scratch directory under the current directory; the process
 * chdir()s into it so relative paths in tests stay inside it.  The current
 * directory must not be world-writable (the logger refuses e.g. /tmp). */
static char g_scratch[PATH_MAX];

static void scratch_enter(const char* tag) {
  char tmpl[64];
  snprintf(tmpl, sizeof(tmpl), "ut_%s_XXXXXX", tag);
  if (!mkdtemp(tmpl) || !realpath(tmpl, g_scratch) || chdir(g_scratch) != 0) {
    fprintf(stderr, "cannot create scratch dir: %s\n", strerror(errno));
    exit(2);
  }
}

static int rm_entry(const char* path, const struct stat* st, int flag, struct FTW* ftw) {
  (void)st; (void)flag; (void)ftw;
  return remove(path);
}

static void scratch_leave(void) {
  if (chdir("..") != 0) return;
  if (nftw(g_scratch, rm_entry, 16, FTW_DEPTH | FTW_PHYS) != 0)
    fprintf(stderr, "warning: could not remove %s\n", g_scratch);
}

static inline int write_file(const char* path, const char* text) {
  FILE* f = fopen(path, "w");
  if (!f) return -1;
  fputs(text, f);
  return fclose(f);
}

static inline long file_size(const char* path) {
  struct stat st;
  return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static inline int file_exists(const char* path) {
  struct stat st;
  return lstat(path, &st) == 0;
}
