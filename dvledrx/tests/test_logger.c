/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Unit tests for logger: level parsing/filtering, safe log paths, file output
 * and size-based rotation into gzip archives.
 */

#include "test_common.h"

#include <fcntl.h>
#include <zlib.h>

#include "logger.h"

/* Read a whole (plain or gzip) file into buf; returns bytes read or -1. */
static long read_gz(const char* path, char* buf, size_t len) {
  gzFile gz = gzopen(path, "rb");
  if (!gz) return -1;
  int n = gzread(gz, buf, (unsigned)len - 1);
  gzclose(gz);
  if (n < 0) return -1;
  buf[n] = '\0';
  return n;
}

static void test_parse_level(void) {
  enum log_level l = LOG_LEVEL_ERROR;
  CHECK(logger_parse_level("debug", &l) && l == LOG_LEVEL_DEBUG);
  CHECK(logger_parse_level("INFO", &l) && l == LOG_LEVEL_INFO);
  CHECK(logger_parse_level("Warn", &l) && l == LOG_LEVEL_WARN);
  CHECK(logger_parse_level("warning", &l) && l == LOG_LEVEL_WARN);
  CHECK(logger_parse_level("error", &l) && l == LOG_LEVEL_ERROR);
  l = LOG_LEVEL_INFO;
  CHECK(!logger_parse_level("verbose", &l));
  CHECK(!logger_parse_level("", &l));
  CHECK(!logger_parse_level("debugdebugdebugdebug", &l)); /* longer than the buffer */
  CHECK(l == LOG_LEVEL_INFO);                              /* untouched on failure */
  CHECK(!logger_parse_level(NULL, &l));
  CHECK(!logger_parse_level("info", NULL));
}

static void test_set_get_level_and_env(void) {
  logger_set_level(LOG_LEVEL_WARN);
  CHECK_EQ_INT(logger_get_level(), LOG_LEVEL_WARN);

  setenv("RXAPP_LOG_LEVEL", "debug", 1);
  logger_init_from_env();
  CHECK_EQ_INT(logger_get_level(), LOG_LEVEL_DEBUG);

  setenv("RXAPP_LOG_LEVEL", "bogus", 1);
  logger_init_from_env();
  CHECK_EQ_INT(logger_get_level(), LOG_LEVEL_DEBUG); /* invalid value ignored */

  unsetenv("RXAPP_LOG_LEVEL");
  logger_set_level(LOG_LEVEL_INFO);
  logger_init_from_env();
  CHECK_EQ_INT(logger_get_level(), LOG_LEVEL_INFO);
}

static void test_path_is_safe(void) {
  char why[512];
  CHECK(logger_path_is_safe("app.log", why, sizeof(why)));
  CHECK(logger_path_is_safe("./app.log", why, sizeof(why)));

  REQUIRE(mkdir("logs", 0750) == 0);
  CHECK(logger_path_is_safe("logs/app.log", why, sizeof(why)));

  why[0] = '\0';
  CHECK(!logger_path_is_safe("", why, sizeof(why)));
  CHECK(why[0] != '\0');
  CHECK(!logger_path_is_safe(NULL, why, sizeof(why)));
  CHECK(!logger_path_is_safe("logs/", why, sizeof(why)));
  CHECK(!logger_path_is_safe("nope/app.log", why, sizeof(why)));
  CHECK(!logger_path_is_safe("/tmp/app.log", why, sizeof(why)));
  CHECK(strstr(why, "world-writable") != NULL);
  CHECK(!logger_path_is_safe("logs", why, sizeof(why))); /* existing directory */
}

static void test_path_is_safe_world_writable_parent(void) {
  char why[512];
  REQUIRE(mkdir("open", 0750) == 0);
  REQUIRE(chmod("open", 0777) == 0);
  REQUIRE(mkdir("open/inner", 0750) == 0);
  CHECK(!logger_path_is_safe("open/app.log", why, sizeof(why)));
  CHECK(!logger_path_is_safe("open/inner/app.log", why, sizeof(why))); /* any ancestor */
  chmod("open", 0750);
  CHECK(logger_path_is_safe("open/inner/app.log", why, sizeof(why)));
}

static void test_path_is_safe_rejects_symlink(void) {
  char why[512];
  REQUIRE(write_file("target.log", "x\n") == 0);
  REQUIRE(symlink("target.log", "link.log") == 0);
  CHECK(!logger_path_is_safe("link.log", why, sizeof(why)));
  CHECK(strstr(why, "not a regular file") != NULL);
  REQUIRE(symlink("/tmp", "tmplink") == 0);
  CHECK(!logger_path_is_safe("tmplink/app.log", why, sizeof(why))); /* resolved dir checked */
}

static void test_file_output_and_level_filter(void) {
  char buf[4096];
  logger_set_level(LOG_LEVEL_INFO);
  logger_set_file("out.log", 0);
  log_debug("t", "hidden %d", 1);
  log_info("t", "visible %d", 2);
  log_warn(NULL, "warned");
  log_error("t", "failed %s", "here");
  logger_close();

  REQUIRE(read_gz("out.log", buf, sizeof(buf)) > 0); /* gzread also reads plain files */
  CHECK(strstr(buf, "hidden") == NULL);
  CHECK(strstr(buf, "[INFO] [t] visible 2\n") != NULL);
  CHECK(strstr(buf, "[WARN] [app] warned\n") != NULL);
  CHECK(strstr(buf, "[ERROR] [t] failed here\n") != NULL);
  CHECK(strchr(buf, '\033') == NULL); /* no colour codes in files */

  struct stat st;
  REQUIRE(stat("out.log", &st) == 0);
  CHECK_EQ_INT(st.st_mode & 0777, 0640);
}

static void test_file_appends(void) {
  char buf[4096];
  REQUIRE(write_file("append.log", "previous run\n") == 0);
  logger_set_file("append.log", 0);
  log_info("t", "new run");
  logger_close();
  REQUIRE(read_gz("append.log", buf, sizeof(buf)) > 0);
  CHECK(strncmp(buf, "previous run\n", 13) == 0);
  CHECK(strstr(buf, "new run") != NULL);
}

static void test_unsafe_file_refused(void) {
  REQUIRE(write_file("real.log", "") == 0);
  REQUIRE(symlink("real.log", "evil.log") == 0);
  logger_set_file("evil.log", 0);
  log_info("t", "must not reach the symlink target");
  logger_close();
  CHECK_EQ_INT(file_size("real.log"), 0);

  logger_set_file("no-such-dir/x.log", 0);
  log_info("t", "console");
  logger_close();
  CHECK(!file_exists("no-such-dir"));
}

static void test_rotation_creates_gzip_archive(void) {
  char buf[8192];
  logger_set_file("rot.log", 1024);
  /* ~63-byte lines: the file passes 1024 bytes after 17 lines, so exactly one rotation */
  for (int i = 0; i < 20; i++) log_info("t", "line %02d padding padding padding", i);
  logger_close(); /* joins the archiver thread */

  CHECK(!file_exists("rot.log.rotating"));
  CHECK(!file_exists("rot.log.new.gz"));
  CHECK(!file_exists("rot.log.2.gz"));
  REQUIRE(file_exists("rot.log.1.gz"));
  REQUIRE(read_gz("rot.log.1.gz", buf, sizeof(buf)) > 0);
  CHECK(strstr(buf, "line 00") != NULL);
  CHECK(strstr(buf, "line 15") != NULL);
  CHECK(strstr(buf, "line 19") == NULL);
  REQUIRE(read_gz("rot.log", buf, sizeof(buf)) > 0);
  CHECK(strstr(buf, "--- log rotated") != NULL);
  CHECK(strstr(buf, "line 00") == NULL);
  CHECK(strstr(buf, "line 19") != NULL);
}

static void test_rotation_keeps_max_archives(void) {
  char name[64], buf[8192];
  for (int r = 0; r < LOGGER_MAX_ARCHIVES + 3; r++) {
    logger_set_file("keep.log", 256);
    for (int i = 0; i < 6; i++) log_info("t", "round %02d line %d ........................", r, i);
    logger_close();
  }
  for (int i = 1; i <= LOGGER_MAX_ARCHIVES; i++) {
    snprintf(name, sizeof(name), "keep.log.%d.gz", i);
    CHECK(file_exists(name));
  }
  snprintf(name, sizeof(name), "keep.log.%d.gz", LOGGER_MAX_ARCHIVES + 1);
  CHECK(!file_exists(name));
  CHECK(!file_exists("keep.log.rotating"));

  /* newest archive is .1.gz: it holds a later round than .10.gz */
  REQUIRE(read_gz("keep.log.1.gz", buf, sizeof(buf)) > 0);
  char* p1 = strstr(buf, "round ");
  REQUIRE(p1 != NULL);
  int newest = atoi(p1 + 6);
  snprintf(name, sizeof(name), "keep.log.%d.gz", LOGGER_MAX_ARCHIVES);
  REQUIRE(read_gz(name, buf, sizeof(buf)) > 0);
  char* p10 = strstr(buf, "round ");
  REQUIRE(p10 != NULL);
  CHECK(atoi(p10 + 6) < newest);
}

static void test_debug_level_and_reopen(void) {
  char buf[4096];
  logger_set_level(LOG_LEVEL_DEBUG);
  logger_set_file("first.log", 0);
  log_debug("t", "to first");
  logger_set_file("second.log", 0); /* switching files closes the first one */
  log_debug("t", "to second");
  logger_close();
  logger_set_level(LOG_LEVEL_INFO);
  REQUIRE(read_gz("first.log", buf, sizeof(buf)) > 0);
  CHECK(strstr(buf, "[DEBUG] [t] to first") != NULL);
  CHECK(strstr(buf, "to second") == NULL);
  REQUIRE(read_gz("second.log", buf, sizeof(buf)) > 0);
  CHECK(strstr(buf, "[DEBUG] [t] to second") != NULL);
}

static void test_path_in_root_dir_and_too_long(void) {
  char why[512], path[600] = "";
  CHECK(logger_path_is_safe("/rx-unit-test-not-created.log", why, sizeof(why)));

  while (strlen(path) < 520) strcat(path, "./");
  strcat(path, "long.log");
  CHECK(logger_path_is_safe(path, why, sizeof(why)));
  logger_set_file(path, 0); /* safe but longer than the logger keeps: console */
  log_info("t", "console");
  logger_close();
  CHECK(!file_exists("long.log"));
}

static void test_unwritable_dir_uses_console(void) {
  if (geteuid() == 0) {
    fprintf(stderr, "  (skipped as root)\n");
    return;
  }
  REQUIRE(mkdir("ro", 0550) == 0);
  logger_set_file("ro/app.log", 0);
  log_info("t", "console");
  logger_close();
  CHECK(!file_exists("ro/app.log"));
  chmod("ro", 0750);
}

/* A leftover that cannot be compressed is kept (never overwritten) and the live
 * log keeps growing past the limit instead of rotating. */
static void test_failed_compression_keeps_pending(void) {
  char buf[256];
  if (geteuid() == 0) {
    fprintf(stderr, "  (skipped as root)\n");
    return;
  }
  REQUIRE(write_file("stuck.log.rotating", "precious\n") == 0);
  REQUIRE(chmod("stuck.log.rotating", 0) == 0);
  logger_set_file("stuck.log", 256);
  for (int i = 0; i < 20; i++) log_info("t", "line %02d ..............................", i);
  logger_close();
  CHECK(!file_exists("stuck.log.1.gz"));
  CHECK(!file_exists("stuck.log.new.gz"));
  CHECK(file_size("stuck.log") > 256 * 3);
  REQUIRE(chmod("stuck.log.rotating", 0640) == 0);
  REQUIRE(read_gz("stuck.log.rotating", buf, sizeof(buf)) > 0);
  CHECK_EQ_STR(buf, "precious\n");

  logger_set_file("stuck.log", 0); /* readable again: archived on the next start */
  logger_close();
  CHECK(!file_exists("stuck.log.rotating"));
  REQUIRE(read_gz("stuck.log.1.gz", buf, sizeof(buf)) > 0);
  CHECK_EQ_STR(buf, "precious\n");
}

/* Directory made read-only while logging: rotation (rename) and archiving (temp
 * file) fail, the live log keeps working and nothing is lost. */
static void test_readonly_dir_rotation_fails_safely(void) {
  char buf[8192];
  if (geteuid() == 0) {
    fprintf(stderr, "  (skipped as root)\n");
    return;
  }
  REQUIRE(mkdir("rodir", 0750) == 0);
  logger_set_file("rodir/app.log", 256);
  REQUIRE(chmod("rodir", 0550) == 0);
  for (int i = 0; i < 10; i++) log_info("t", "line %02d ..............................", i);
  logger_close();
  REQUIRE(read_gz("rodir/app.log", buf, sizeof(buf)) > 0);
  CHECK(strstr(buf, "--- log rotation failed") != NULL);
  CHECK(strstr(buf, "line 00") != NULL && strstr(buf, "line 09") != NULL);
  CHECK(!file_exists("rodir/app.log.rotating"));

  REQUIRE(chmod("rodir", 0750) == 0);
  REQUIRE(write_file("rodir/app.log.rotating", "pending\n") == 0);
  REQUIRE(chmod("rodir", 0550) == 0);
  logger_set_file("rodir/app.log", 0); /* cannot create the temp archive */
  logger_close();
  CHECK(file_exists("rodir/app.log.rotating"));
  CHECK(!file_exists("rodir/app.log.1.gz"));
  chmod("rodir", 0750);
}

static void test_path_owned_by_other_user(void) {
  /* system directories owned by service accounts, when present */
  static const char* dirs[] = {"/var/cache/man", "/var/lib/colord", "/var/spool/cups",
                               "/var/lib/gdm3", "/var/log/cups"};
  char why[512], path[PATH_MAX];
  for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
    struct stat st;
    if (stat(dirs[i], &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid == 0 ||
        st.st_uid == geteuid() || (st.st_mode & S_IWOTH))
      continue;
    snprintf(path, sizeof(path), "%s/app.log", dirs[i]);
    CHECK(!logger_path_is_safe(path, why, sizeof(why)));
    CHECK(strstr(why, "owned by another user") != NULL);
    return;
  }
  fprintf(stderr, "  (skipped: no directory owned by another user found)\n");
}

static void test_interrupted_rotation_recovered(void) {
  char buf[1024];
  REQUIRE(write_file("crash.log.rotating", "left over from a crash\n") == 0);
  logger_set_file("crash.log", 0);
  logger_close();
  CHECK(!file_exists("crash.log.rotating"));
  REQUIRE(read_gz("crash.log.1.gz", buf, sizeof(buf)) > 0);
  CHECK(strstr(buf, "left over from a crash") != NULL);
}

int main(void) {
  scratch_enter("logger");

  RUN_TEST(test_parse_level);
  RUN_TEST(test_set_get_level_and_env);
  RUN_TEST(test_path_is_safe);
  RUN_TEST(test_path_is_safe_world_writable_parent);
  RUN_TEST(test_path_is_safe_rejects_symlink);
  RUN_TEST(test_file_output_and_level_filter);
  RUN_TEST(test_file_appends);
  RUN_TEST(test_unsafe_file_refused);
  RUN_TEST(test_rotation_creates_gzip_archive);
  RUN_TEST(test_rotation_keeps_max_archives);
  RUN_TEST(test_interrupted_rotation_recovered);
  RUN_TEST(test_debug_level_and_reopen);
  RUN_TEST(test_path_in_root_dir_and_too_long);
  RUN_TEST(test_unwritable_dir_uses_console);
  RUN_TEST(test_failed_compression_keeps_pending);
  RUN_TEST(test_readonly_dir_rotation_fails_safely);
  RUN_TEST(test_path_owned_by_other_user);

  scratch_leave();
  return test_summary();
}
