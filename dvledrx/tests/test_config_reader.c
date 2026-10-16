/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Unit tests for config_reader: JSON parsing, defaults, "displays" placement
 * and every validate_rx_config() rule.
 * Usage: test_config_reader [<repo config dir>]
 */

#include "test_common.h"

#include <dirent.h>

#include "config_reader.h"
#include "logger.h"

static const char* g_config_dir;

/* Parse JSON text via a scratch file. */
static int parse_text(const char* json, struct rx_config* cfg) {
  if (write_file("cfg.json", json) != 0) return -99;
  return parse_rx_config("cfg.json", cfg);
}

/* A valid 2-NIC, 2-session config built in C; tests break one field each. */
static void make_valid(struct rx_config* c) {
  memset(c, 0, sizeof(*c));
  c->num_interfaces = 2;
  for (int i = 0; i < 2; i++) {
    c->interfaces[i].nic_index = i;
    snprintf(c->interfaces[i].name, sizeof(c->interfaces[i].name), "0000:03:00.%d", i);
    snprintf(c->interfaces[i].sip, sizeof(c->interfaces[i].sip), "192.168.50.%d", 65 + i);
    snprintf(c->interfaces[i].dip, sizeof(c->interfaces[i].dip), "239.168.85.%d", 20 + i);
  }
  c->video.width = 1920;
  c->video.height = 1080;
  c->video.fps = 30;
  snprintf(c->video.fmt, sizeof(c->video.fmt), "yuv422p10le");
  c->num_rx_sessions = 2;
  for (int i = 0; i < 2; i++) {
    struct rx_session_config* s = &c->rx_sessions[i];
    s->nic_index = i;
    s->udp_port = 20000 + 2 * i;
    s->payload_type = 96;
    s->crop = (struct crop_config){i * 960, 0, 960, 1080};
  }
}

#define EXPECT_INVALID(mutation)          \
  do {                                    \
    struct rx_config c_;                  \
    make_valid(&c_);                      \
    { struct rx_config* c = &c_; mutation; } \
    CHECK_EQ_INT(validate_rx_config(&c_), -1); \
  } while (0)

#define EXPECT_VALID(mutation)            \
  do {                                    \
    struct rx_config c_;                  \
    make_valid(&c_);                      \
    { struct rx_config* c = &c_; mutation; } \
    CHECK_EQ_INT(validate_rx_config(&c_), 0); \
  } while (0)

/* ------------------------------------------------------------------ parsing */

static void test_parse_full_config(void) {
  struct rx_config c;
  REQUIRE(parse_text(
      "{ \"log_file\": \"rx.log\", \"log_level\": \"debug\", \"tasklet_sleep\": true,\n"
      "  \"interfaces\": [\n"
      "    { \"nic_index\": 3, \"name\": \"0000:03:00.0\", \"sip\": \"192.168.50.65\", \"dip\": \"239.1.1.1\" },\n"
      "    { \"nic_index\": 7, \"name\": \"0000:03:00.1\", \"SIP\": \"192.168.50.66\", \"DIP\": \"239.1.1.2\" } ],\n"
      "  \"video\": { \"width\": 3840, \"height\": 2160, \"fps\": 60, \"fmt\": \"gbrp12le\" },\n"
      "  \"rx_sessions\": [\n"
      "    { \"nic_index\": 3, \"udp_port\": 30000, \"payload_type\": 112,\n"
      "      \"crop\": { \"x\": 0, \"y\": 0, \"w\": 1920, \"h\": 2160 } } ] }", &c) == 0);
  CHECK_EQ_INT(c.num_interfaces, 2);
  CHECK_EQ_INT(c.interfaces[0].nic_index, 3);
  CHECK_EQ_INT(c.interfaces[1].nic_index, 7);
  CHECK_EQ_STR(c.interfaces[0].name, "0000:03:00.0");
  CHECK_EQ_STR(c.interfaces[0].sip, "192.168.50.65");
  CHECK_EQ_STR(c.interfaces[1].sip, "192.168.50.66"); /* upper-case SIP/DIP accepted */
  CHECK_EQ_STR(c.interfaces[1].dip, "239.1.1.2");
  CHECK_EQ_INT(c.video.width, 3840);
  CHECK_EQ_INT(c.video.height, 2160);
  CHECK_EQ_INT(c.video.fps, 60);
  CHECK_EQ_STR(c.video.fmt, "gbrp12le");
  CHECK_EQ_STR(c.log_file, "rx.log");
  CHECK_EQ_STR(c.log_level, "debug");
  CHECK(c.tasklet_sleep);
  CHECK_EQ_INT(c.num_rx_sessions, 1);
  CHECK_EQ_INT(c.rx_sessions[0].nic_index, 3);
  CHECK_EQ_INT(c.rx_sessions[0].udp_port, 30000);
  CHECK_EQ_INT(c.rx_sessions[0].payload_type, 112);
  CHECK_EQ_INT(c.rx_sessions[0].crop.w, 1920);
  CHECK_EQ_INT(c.rx_sessions[0].crop.h, 2160);
  CHECK(!c.rx_sessions[0].has_window);
  CHECK_EQ_STR(c.display_mode, "");
  CHECK_EQ_INT(validate_rx_config(&c), 0);
}

static void test_parse_defaults(void) {
  struct rx_config c;
  REQUIRE(parse_text(
      "{ \"interfaces\": [ { \"name\": \"a\" }, { \"name\": \"b\" } ],\n"
      "  \"video\": { \"width\": 1920, \"height\": 1080 },\n"
      "  \"rx_sessions\": [ { \"crop\": { \"w\": 16, \"h\": 16 } } ] }", &c) == 0);
  CHECK_EQ_INT(c.interfaces[0].nic_index, 0); /* nic_index defaults to position */
  CHECK_EQ_INT(c.interfaces[1].nic_index, 1);
  CHECK_EQ_INT(c.video.fps, 30);
  CHECK_EQ_STR(c.video.fmt, "");
  CHECK(!c.tasklet_sleep);
  CHECK_EQ_INT(c.rx_sessions[0].nic_index, 0);
  CHECK_EQ_INT(c.rx_sessions[0].udp_port, 20000);
  CHECK_EQ_INT(c.rx_sessions[0].payload_type, 96);
  CHECK_EQ_INT(c.rx_sessions[0].crop.x, 0);
}

static void test_parse_video_size_from_crops(void) {
  struct rx_config c;
  REQUIRE(parse_text(
      "{ \"video\": { \"fps\": 30 },\n"
      "  \"rx_sessions\": [ { \"crop\": { \"x\": 0,    \"y\": 0, \"w\": 1920, \"h\": 2160 } },\n"
      "                     { \"crop\": { \"x\": 1920, \"y\": 0, \"w\": 1920, \"h\": 2160 } } ] }",
      &c) == 0);
  CHECK_EQ_INT(c.video.width, 3840);
  CHECK_EQ_INT(c.video.height, 2160);

  REQUIRE(parse_text("{ \"video\": { \"fps\": 30 } }", &c) == 0);
  CHECK_EQ_INT(c.video.width, 1920); /* no sessions: full HD fallback */
  CHECK_EQ_INT(c.video.height, 1080);
}

static void test_parse_displays_and_mirrors(void) {
  struct rx_config c;
  REQUIRE(parse_text(
      "{ \"rx_sessions\": [ { \"udp_port\": 20000, \"crop\": { \"w\": 960, \"h\": 1080 } },\n"
      "                     { \"udp_port\": 20002, \"crop\": { \"x\": 960, \"w\": 960, \"h\": 1080 } } ],\n"
      "  \"displays\": [\n"
      "    { \"output\": \"DP-1\", \"sessions\": [\n"
      "      { \"session\": 0, \"x\": 0,   \"y\": 0, \"scale_w\": 540, \"scale_h\": 1920 },\n"
      "      { \"session\": 1, \"x\": 540, \"y\": 0, \"scale_w\": 540, \"scale_h\": 1920 } ] },\n"
      "    { \"output\": \"HDMI-2\", \"sessions\": [\n"
      "      { \"session\": 1, \"x\": 10, \"y\": 20, \"w\": 300, \"h\": 400, \"scale_w\": 1080, \"scale_h\": 1920 } ] } ] }",
      &c) == 0);
  CHECK_EQ_STR(c.display_mode, "per_output"); /* implied by "displays" */
  const struct rx_session_config* s0 = &c.rx_sessions[0];
  const struct rx_session_config* s1 = &c.rx_sessions[1];
  CHECK(s0->display_assigned && s0->has_window);
  CHECK_EQ_STR(s0->output, "DP-1");
  CHECK_EQ_INT(s0->scale_w, 540);
  CHECK_EQ_INT(s0->num_mirrors, 0);
  CHECK_EQ_STR(s1->output, "DP-1");
  CHECK_EQ_INT(s1->window.x, 540);
  CHECK_EQ_INT(s1->num_mirrors, 1); /* second placement becomes a mirror */
  CHECK_EQ_STR(s1->mirrors[0].output, "HDMI-2");
  CHECK_EQ_INT(s1->mirrors[0].window.x, 10);
  CHECK_EQ_INT(s1->mirrors[0].window.y, 20);
  CHECK_EQ_INT(s1->mirrors[0].window.w, 300);
  CHECK_EQ_INT(s1->mirrors[0].window.h, 400);
  CHECK_EQ_INT(s1->mirrors[0].scale_w, 1080);
  CHECK_EQ_INT(s1->mirrors[0].scale_h, 1920);
}

static void test_parse_display_mode_not_overridden(void) {
  struct rx_config c;
  REQUIRE(parse_text("{ \"display_mode\": \"composite\", \"rx_sessions\": [ {} ],\n"
                     "  \"displays\": [ { \"output\": \"DP-1\", \"sessions\": [ { \"session\": 0 } ] } ] }",
                     &c) == 0);
  CHECK_EQ_STR(c.display_mode, "composite");
}

static void test_parse_display_unknown_session(void) {
  struct rx_config c;
  CHECK_EQ_INT(parse_text("{ \"rx_sessions\": [ {} ],\n"
                          "  \"displays\": [ { \"output\": \"DP-1\", \"sessions\": [ { \"session\": 1 } ] } ] }",
                          &c), -1);
  CHECK_EQ_INT(parse_text("{ \"rx_sessions\": [ {} ],\n"
                          "  \"displays\": [ { \"output\": \"DP-1\", \"sessions\": [ { \"x\": 0 } ] } ] }",
                          &c), -1);
}

static void test_parse_too_many_mirrors(void) {
  char json[2048];
  struct rx_config c;
  int n = snprintf(json, sizeof(json), "{ \"rx_sessions\": [ {} ], \"displays\": [");
  for (int d = 0; d < MAX_MIRRORS + 2; d++)
    n += snprintf(json + n, sizeof(json) - n,
                  "%s { \"output\": \"OUT-%d\", \"sessions\": [ { \"session\": 0 } ] }",
                  d ? "," : "", d);
  snprintf(json + n, sizeof(json) - n, " ] }");
  CHECK_EQ_INT(parse_text(json, &c), -1);

  /* exactly 1 primary + MAX_MIRRORS mirrors is fine */
  n = snprintf(json, sizeof(json), "{ \"rx_sessions\": [ {} ], \"displays\": [");
  for (int d = 0; d < MAX_MIRRORS + 1; d++)
    n += snprintf(json + n, sizeof(json) - n,
                  "%s { \"output\": \"OUT-%d\", \"sessions\": [ { \"session\": 0 } ] }",
                  d ? "," : "", d);
  snprintf(json + n, sizeof(json) - n, " ] }");
  REQUIRE(parse_text(json, &c) == 0);
  CHECK_EQ_INT(c.rx_sessions[0].num_mirrors, MAX_MIRRORS);
}

static void test_parse_session_window_and_scale(void) {
  struct rx_config c;
  REQUIRE(parse_text(
      "{ \"rx_sessions\": [ { \"output\": \"HDMI-1\", \"crop\": { \"w\": 960, \"h\": 540 },\n"
      "    \"window\": { \"x\": 5, \"y\": 6, \"w\": 7, \"h\": 8 }, \"scale\": { \"w\": 1920, \"h\": 1080 } },\n"
      "  { \"window\": 1, \"scale\": \"2x\" } ] }", &c) == 0);
  const struct rx_session_config* s = &c.rx_sessions[0];
  CHECK_EQ_STR(s->output, "HDMI-1");
  CHECK(s->has_window && !s->display_assigned);
  CHECK(s->window.x == 5 && s->window.y == 6 && s->window.w == 7 && s->window.h == 8);
  CHECK_EQ_INT(s->scale_w, 1920);
  CHECK_EQ_INT(s->scale_h, 1080);
  CHECK(!c.rx_sessions[1].has_window); /* non-object window/scale ignored */
  CHECK_EQ_INT(c.rx_sessions[1].scale_w, 0);
}

static void test_parse_display_without_sessions_ignored(void) {
  struct rx_config c;
  REQUIRE(parse_text("{ \"rx_sessions\": [ {} ],\n"
                     "  \"displays\": [ { \"output\": \"DP-1\" },\n"
                     "                 { \"output\": \"DP-2\", \"sessions\": {} },\n"
                     "                 { \"output\": \"HDMI-2\", \"sessions\": [ { \"session\": 0 } ] } ] }",
                     &c) == 0);
  CHECK_EQ_STR(c.rx_sessions[0].output, "HDMI-2");
  CHECK_EQ_INT(c.rx_sessions[0].num_mirrors, 0);
}

static void test_parse_truncates_arrays(void) {
  char json[8192];
  struct rx_config c;
  int n = snprintf(json, sizeof(json), "{ \"interfaces\": [");
  for (int i = 0; i < MAX_INTERFACES + 2; i++)
    n += snprintf(json + n, sizeof(json) - n, "%s{ \"name\": \"n%d\" }", i ? "," : "", i);
  n += snprintf(json + n, sizeof(json) - n, "], \"rx_sessions\": [");
  for (int i = 0; i < MAX_RX_SESSIONS + 3; i++)
    n += snprintf(json + n, sizeof(json) - n, "%s{ \"udp_port\": %d }", i ? "," : "", 20000 + i);
  snprintf(json + n, sizeof(json) - n, "] }");
  REQUIRE(parse_text(json, &c) == 0);
  CHECK_EQ_INT(c.num_interfaces, MAX_INTERFACES);
  CHECK_EQ_INT(c.num_rx_sessions, MAX_RX_SESSIONS);
}

static void test_parse_long_strings_truncated(void) {
  char json[512], name[200];
  struct rx_config c;
  memset(name, 'x', sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  snprintf(json, sizeof(json), "{ \"interfaces\": [ { \"name\": \"%s\" } ] }", name);
  REQUIRE(parse_text(json, &c) == 0);
  CHECK_EQ_INT(strlen(c.interfaces[0].name), sizeof(c.interfaces[0].name) - 1);
}

static void test_parse_wrong_types_ignored(void) {
  struct rx_config c;
  REQUIRE(parse_text("{ \"video\": { \"width\": \"1920\", \"fps\": \"60\", \"fmt\": 5 },\n"
                     "  \"rx_sessions\": [ { \"udp_port\": \"x\", \"crop\": [] } ] }", &c) == 0);
  CHECK_EQ_INT(c.video.fps, 30);
  CHECK_EQ_STR(c.video.fmt, "");
  CHECK_EQ_INT(c.rx_sessions[0].udp_port, 20000);
  CHECK_EQ_INT(c.rx_sessions[0].crop.w, 0);
}

static void test_parse_errors(void) {
  struct rx_config c;
  CHECK_EQ_INT(parse_text("{ \"interfaces\": [ ", &c), -1);
  CHECK_EQ_INT(parse_rx_config("does-not-exist.json", &c), -1);
  CHECK_EQ_INT(parse_rx_config(NULL, &c), -1);
  CHECK_EQ_INT(parse_rx_config("cfg.json", NULL), -1);
  CHECK_EQ_INT(load_and_validate_rx_config("", &c), -1);
  CHECK_EQ_INT(load_and_validate_rx_config(NULL, &c), -1);
  CHECK_EQ_INT(validate_rx_config(NULL), -1);
}

static void test_load_and_validate(void) {
  struct rx_config c;
  REQUIRE(write_file("ok.json",
      "{ \"interfaces\": [ { \"name\": \"0000:03:00.0\", \"sip\": \"192.168.50.65\", \"dip\": \"239.168.85.20\" } ],\n"
      "  \"video\": { \"fps\": 30, \"fmt\": \"yuv422p10le\" },\n"
      "  \"rx_sessions\": [ { \"crop\": { \"w\": 1920, \"h\": 1080 } } ] }") == 0);
  CHECK_EQ_INT(load_and_validate_rx_config("ok.json", &c), 0);
  REQUIRE(write_file("bad.json",
      "{ \"interfaces\": [ { \"name\": \"0000:03:00.0\", \"sip\": \"192.168.50.65\" } ],\n"
      "  \"video\": { \"fps\": 61 }, \"rx_sessions\": [ { \"crop\": { \"w\": 8, \"h\": 8 } } ] }") == 0);
  CHECK_EQ_INT(load_and_validate_rx_config("bad.json", &c), -1);
}

/* --------------------------------------------------------------- validation */

static void test_validate_baseline(void) {
  EXPECT_VALID((void)c);
  EXPECT_VALID(c->video.fmt[0] = '\0');             /* empty fmt = yuv422p10le */
  EXPECT_VALID(c->rx_sessions[1].udp_port = 20000); /* same port on another NIC */
}

static void test_validate_interfaces(void) {
  EXPECT_INVALID(c->num_interfaces = 0);
  EXPECT_INVALID(c->interfaces[1].name[0] = '\0');
  EXPECT_INVALID(snprintf(c->interfaces[0].sip, sizeof(c->interfaces[0].sip), "192.168.50"));
  EXPECT_INVALID(c->interfaces[0].sip[0] = '\0');
  EXPECT_INVALID(c->interfaces[0].dip[0] = '\0');
  EXPECT_INVALID(snprintf(c->interfaces[0].dip, sizeof(c->interfaces[0].dip), "239.1.1.256"));
  EXPECT_INVALID(c->interfaces[1].nic_index = 0);
  EXPECT_INVALID(snprintf(c->interfaces[1].name, sizeof(c->interfaces[1].name), "0000:03:00.0"));
}

static void test_validate_video_size(void) {
  EXPECT_VALID((c->video.width = 3840, c->video.height = 2160,
                c->rx_sessions[1].crop.x = 2880));
  EXPECT_VALID((c->video.width = 2160, c->video.height = 3840)); /* portrait 4K */
  EXPECT_INVALID(c->video.width = 0);
  EXPECT_INVALID(c->video.height = -1);
  EXPECT_INVALID(c->video.width = 3841);
  EXPECT_INVALID((c->video.width = 3840, c->video.height = 3840)); /* > 4K pixels */
  EXPECT_INVALID((c->video.width = 4096, c->video.height = 2160));
}

static void test_validate_fps(void) {
  static const int ok[] = {24, 25, 29, 30, 50, 59, 60};
  static const int bad[] = {0, -30, 1, 23, 31, 45, 61, 120};
  for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) EXPECT_VALID(c->video.fps = ok[i]);
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) EXPECT_INVALID(c->video.fps = bad[i]);
}

static void test_validate_fmt(void) {
  static const char* ok[] = {"yuv422p10le", "yuv422p12le", "yuv444p10le", "yuv444p12le",
                             "gbrp10le",    "gbrp12le",    "yuv420"};
  static const char* bad[] = {"nv12", "YUV422P10LE", "yuv422p10", "rgb24", " "};
  for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
    EXPECT_VALID(snprintf(c->video.fmt, sizeof(c->video.fmt), "%s", ok[i]));
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    EXPECT_INVALID(snprintf(c->video.fmt, sizeof(c->video.fmt), "%s", bad[i]));
}

static void test_validate_display_mode(void) {
  EXPECT_VALID(snprintf(c->display_mode, sizeof(c->display_mode), "composite"));
  EXPECT_VALID(snprintf(c->display_mode, sizeof(c->display_mode), "per_output"));
  EXPECT_INVALID(snprintf(c->display_mode, sizeof(c->display_mode), "tiled"));
}

static void test_validate_sessions(void) {
  EXPECT_INVALID(c->num_rx_sessions = 0);
  EXPECT_INVALID(c->rx_sessions[0].crop.w = 0);
  EXPECT_INVALID(c->rx_sessions[0].crop.h = -4);
  EXPECT_INVALID(c->rx_sessions[0].crop.x = -1);
  EXPECT_INVALID(c->rx_sessions[0].crop.y = -1);
  EXPECT_INVALID(c->rx_sessions[1].crop.x = 961);   /* x + w > width */
  EXPECT_INVALID(c->rx_sessions[1].crop.y = 1);     /* y + h > height */
  EXPECT_VALID(c->rx_sessions[1].udp_port = 65535);
  EXPECT_VALID(c->rx_sessions[1].udp_port = 1);
  EXPECT_INVALID(c->rx_sessions[1].udp_port = 0);
  EXPECT_INVALID(c->rx_sessions[1].udp_port = 65536);
  EXPECT_VALID(c->rx_sessions[1].payload_type = 127);
  EXPECT_INVALID(c->rx_sessions[1].payload_type = 95);
  EXPECT_INVALID(c->rx_sessions[1].payload_type = 128);
  EXPECT_INVALID((c->rx_sessions[1].nic_index = 0, c->rx_sessions[1].udp_port = 20000));
  EXPECT_INVALID(c->rx_sessions[1].nic_index = 5); /* no interface with nic_index 5 */
}

static void test_validate_windows(void) {
  EXPECT_VALID((c->rx_sessions[0].has_window = true,
                c->rx_sessions[0].window = (struct crop_config){0, 0, 540, 1920}));
  EXPECT_INVALID((c->rx_sessions[0].has_window = true, c->rx_sessions[0].window.x = -1));
  EXPECT_INVALID((c->rx_sessions[0].has_window = true, c->rx_sessions[0].window.w = 3841));
  EXPECT_VALID((c->rx_sessions[0].scale_w = 3840, c->rx_sessions[0].scale_h = 2160));
  EXPECT_INVALID(c->rx_sessions[0].scale_w = 540);  /* scale_h missing */
  EXPECT_INVALID(c->rx_sessions[0].scale_h = 540);
  EXPECT_INVALID((c->rx_sessions[0].scale_w = 3841, c->rx_sessions[0].scale_h = 100));
  EXPECT_INVALID((c->rx_sessions[0].scale_w = -1, c->rx_sessions[0].scale_h = -1));
  /* placed by "displays" but with no output name */
  EXPECT_INVALID(c->rx_sessions[0].display_assigned = true);
  EXPECT_VALID((c->rx_sessions[0].display_assigned = true,
                snprintf(c->rx_sessions[0].output, sizeof(c->rx_sessions[0].output), "DP-1")));
}

static void test_validate_mirrors(void) {
#define ADD_MIRROR(out, sw, sh)                                                   \
  do {                                                                            \
    struct display_placement* m_ = &c->rx_sessions[0].mirrors[c->rx_sessions[0].num_mirrors++]; \
    snprintf(m_->output, sizeof(m_->output), "%s", out);                          \
    m_->scale_w = sw;                                                             \
    m_->scale_h = sh;                                                             \
  } while (0)
  EXPECT_VALID(ADD_MIRROR("HDMI-2", 1080, 1920));
  EXPECT_INVALID(ADD_MIRROR("", 1080, 1920));
  EXPECT_INVALID(ADD_MIRROR("HDMI-2", 1080, 0));
  EXPECT_INVALID(ADD_MIRROR("HDMI-2", 4000, 1920));
  EXPECT_INVALID(ADD_MIRROR("HDMI-2", 0, 0); c->rx_sessions[0].mirrors[0].window.y = -5);
#undef ADD_MIRROR
}

static void test_validate_bandwidth_is_warning_only(void) {
  /* 4K60 12-bit 4:4:4 on one NIC is ~18.8 Gb/s: warned about, not rejected */
  EXPECT_VALID((c->video.width = 3840, c->video.height = 2160, c->video.fps = 60,
                snprintf(c->video.fmt, sizeof(c->video.fmt), "gbrp12le"),
                c->num_rx_sessions = 1,
                c->rx_sessions[0].crop = (struct crop_config){0, 0, 3840, 2160}));
}

static void test_validate_log_file(void) {
  EXPECT_VALID(snprintf(c->log_file, sizeof(c->log_file), "rx.log"));
  EXPECT_INVALID(snprintf(c->log_file, sizeof(c->log_file), "/tmp/rx.log"));
  EXPECT_INVALID(snprintf(c->log_file, sizeof(c->log_file), "missing-dir/rx.log"));
  EXPECT_INVALID(snprintf(c->log_file, sizeof(c->log_file), "logs/"));
}

static void test_port_for_nic(void) {
  struct rx_config c;
  make_valid(&c);
  c.interfaces[0].nic_index = 4;
  c.interfaces[1].nic_index = 2;
  CHECK_EQ_INT(rx_config_port_for_nic(&c, 4), 0);
  CHECK_EQ_INT(rx_config_port_for_nic(&c, 2), 1);
  CHECK_EQ_INT(rx_config_port_for_nic(&c, 0), -1);
  CHECK_EQ_INT(rx_config_port_for_nic(&c, -1), -1);
}

/* Every shipped config under config/ must load and validate. */
static void test_repo_configs_valid(void) {
  if (!g_config_dir) {
    fprintf(stderr, "  (skipped: no config dir argument)\n");
    return;
  }
  DIR* d = opendir(g_config_dir);
  REQUIRE(d != NULL);
  int n = 0;
  struct dirent* e;
  while ((e = readdir(d)) != NULL) {
    size_t len = strlen(e->d_name);
    if (len < 5 || strcmp(e->d_name + len - 5, ".json") != 0) continue;
    char path[PATH_MAX];
    struct rx_config c;
    snprintf(path, sizeof(path), "%s/%s", g_config_dir, e->d_name);
    int rc = load_and_validate_rx_config(path, &c);
    if (rc != 0) fprintf(stderr, "  %s is invalid\n", e->d_name);
    CHECK_EQ_INT(rc, 0);
    n++;
  }
  closedir(d);
  CHECK(n > 0);
}

int main(int argc, char** argv) {
  char dir[PATH_MAX];
  if (argc > 1 && realpath(argv[1], dir)) g_config_dir = dir;
  logger_set_level(LOG_LEVEL_ERROR + 1); /* keep expected-error noise out of the output */
  scratch_enter("config");

  RUN_TEST(test_parse_full_config);
  RUN_TEST(test_parse_defaults);
  RUN_TEST(test_parse_video_size_from_crops);
  RUN_TEST(test_parse_displays_and_mirrors);
  RUN_TEST(test_parse_display_mode_not_overridden);
  RUN_TEST(test_parse_display_unknown_session);
  RUN_TEST(test_parse_too_many_mirrors);
  RUN_TEST(test_parse_session_window_and_scale);
  RUN_TEST(test_parse_display_without_sessions_ignored);
  RUN_TEST(test_parse_truncates_arrays);
  RUN_TEST(test_parse_long_strings_truncated);
  RUN_TEST(test_parse_wrong_types_ignored);
  RUN_TEST(test_parse_errors);
  RUN_TEST(test_load_and_validate);
  RUN_TEST(test_validate_baseline);
  RUN_TEST(test_validate_interfaces);
  RUN_TEST(test_validate_video_size);
  RUN_TEST(test_validate_fps);
  RUN_TEST(test_validate_fmt);
  RUN_TEST(test_validate_display_mode);
  RUN_TEST(test_validate_sessions);
  RUN_TEST(test_validate_windows);
  RUN_TEST(test_validate_mirrors);
  RUN_TEST(test_validate_bandwidth_is_warning_only);
  RUN_TEST(test_validate_log_file);
  RUN_TEST(test_port_for_nic);
  RUN_TEST(test_repo_configs_valid);

  scratch_leave();
  return test_summary();
}
