/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Unit tests for session_manager against the fake MTL (tests/fake_mtl.c) and a
 * virtual X server (run under xvfb-run).  Checks every frame taken from MTL is
 * returned, sessions/windows are set up as configured and failures unwind.
 */

#include "test_common.h"

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <time.h>

#include "config_reader.h"
#include "fake_mtl.h"
#include "logger.h"
#include "session_manager.h"

static char g_display_env[64];
static char g_output[32]; /* first XRandR output of the X server */

static void sleep_ms(int ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

static void no_display(void) { unsetenv("DISPLAY"); }
static void with_display(void) { setenv("DISPLAY", g_display_env, 1); }

/* Parse json into app the way main() does (fmt yuv422p10le, transport from fmt). */
static int make_app(struct rx_app_context* app, const char* json) {
  memset(app, 0, sizeof(*app));
  if (write_file("cfg.json", json) != 0 || parse_rx_config("cfg.json", &app->json_config) != 0)
    return -1;
  app->width = (uint32_t)app->json_config.video.width;
  app->height = (uint32_t)app->json_config.video.height;
  app->fps = ST_FPS_P30;
  app->fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
  app->st20p_sessions = app->json_config.num_rx_sessions;
  return 0;
}

/* Main-loop stand-in: pump X events for ms milliseconds. */
static void pump(session_manager_t* m, int ms) {
  for (int t = 0; t < ms; t += 10) {
    if (m->composite) process_composite_x11_events(m->composite);
    for (int i = 0; i < m->st20p_count && m->st20p_sessions; i++) {
      process_x11_events(&m->st20p_sessions[i].display);
      for (int k = 0; k < m->st20p_sessions[i].num_mirror_displays; k++)
        process_x11_events(&m->st20p_sessions[i].mirror_display[k]);
    }
    sleep_ms(10);
  }
}

/* Every frame handed out by MTL must come back exactly once, nothing leaked. */
static void check_frames_balanced(void) {
  CHECK_EQ_INT(fake_mtl.frames_put, fake_mtl.frames_got);
  CHECK_EQ_INT(fake_mtl.bad_puts, 0);
  CHECK_EQ_INT(fake_mtl.leaked, 0);
}

/* init + start, run for ms, then stop/cleanup as main() does. */
static int run_app(struct rx_app_context* app, int ms, session_manager_t* seen) {
  session_manager_t m;
  if (session_manager_init(&m, app) != 0) return -1;
  int rc = session_manager_start(&m, app);
  if (rc == 0) pump(&m, ms);
  if (seen) *seen = m;
  app->exit = true;
  session_manager_cleanup(&m);
  CHECK(!session_manager_is_running(&m));
  return rc;
}

#define IFACE "\"interfaces\": [ { \"name\": \"0000:03:00.0\", \"sip\": \"192.168.50.65\", \"dip\": \"239.168.85.20\" } ]"

/* ------------------------------------------------------------------ tests */

static void test_null_args(void) {
  struct rx_app_context app;
  session_manager_t m;
  memset(&app, 0, sizeof(app));
  CHECK_EQ_INT(session_manager_init(NULL, &app), -1);
  CHECK_EQ_INT(session_manager_init(&m, NULL), -1);
  CHECK_EQ_INT(session_manager_start(NULL, &app), -1);
  CHECK_EQ_INT(session_manager_start(&m, NULL), -1);
  session_manager_stop(NULL);
  session_manager_cleanup(NULL);
  CHECK(!session_manager_is_running(NULL));
}

static void test_init_failure(void) {
  struct rx_app_context app;
  session_manager_t m;
  REQUIRE(make_app(&app, "{ " IFACE ", \"rx_sessions\": [ { \"crop\": { \"w\": 32, \"h\": 16 } } ] }") == 0);
  fake_mtl_reset();
  fake_mtl.fail_init = true;
  CHECK_EQ_INT(session_manager_init(&m, &app), -1);
  CHECK(m.mtl == NULL);
  session_manager_cleanup(&m);
  CHECK_EQ_INT(fake_mtl.uninit_calls, 0);
}

static void test_init_params(void) {
  struct rx_app_context app;
  session_manager_t m;
  REQUIRE(make_app(&app,
      "{ \"tasklet_sleep\": true, \"interfaces\": [\n"
      "    { \"nic_index\": 0, \"name\": \"0000:03:00.0\", \"sip\": \"192.168.50.65\", \"dip\": \"239.1.1.1\" },\n"
      "    { \"nic_index\": 1, \"name\": \"0000:03:00.1\", \"sip\": \"192.168.50.66\", \"dip\": \"239.1.1.2\" } ],\n"
      "  \"video\": { \"width\": 96, \"height\": 16 },\n"
      "  \"rx_sessions\": [ { \"nic_index\": 0, \"udp_port\": 20000, \"crop\": { \"w\": 32, \"h\": 16 } },\n"
      "                     { \"nic_index\": 0, \"udp_port\": 20002, \"crop\": { \"x\": 32, \"w\": 32, \"h\": 16 } },\n"
      "                     { \"nic_index\": 1, \"udp_port\": 20004, \"crop\": { \"x\": 64, \"w\": 32, \"h\": 16 } } ] }") == 0);
  fake_mtl_reset();
  REQUIRE(session_manager_init(&m, &app) == 0);
  const struct mtl_init_params* p = &fake_mtl.last_init;
  CHECK_EQ_INT(p->num_ports, 2);
  CHECK_EQ_STR(p->port[0], "0000:03:00.0");
  CHECK_EQ_STR(p->port[1], "0000:03:00.1");
  CHECK(p->sip_addr[1][0] == 192 && p->sip_addr[1][3] == 66);
  CHECK_EQ_INT(p->rx_queues_cnt[0], 2 + 2); /* sessions on the port + 2 housekeeping */
  CHECK_EQ_INT(p->rx_queues_cnt[1], 1 + 2);
  CHECK(p->flags & MTL_FLAG_TASKLET_SLEEP);
  CHECK(p->flags & MTL_FLAG_RX_VIDEO_MIGRATE);
  CHECK(!(p->flags & MTL_FLAG_RX_USE_CNI)); /* no VF */
  CHECK(app.mtl == m.mtl && m.mtl != NULL);
  CHECK_EQ_INT(m.st20p_count, 3);
  session_manager_cleanup(&m);
  CHECK_EQ_INT(fake_mtl.uninit_calls, 1);
}

/* Odd NIC names go through the PCI address check without touching sysfs paths. */
static void test_unusual_nic_names(void) {
  const char* names[] = {"03:00.0", "0000:03:00.8", "../../etc", "0000:3:00.0", "0000:03:00.0x", ""};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    char json[512];
    struct rx_app_context app;
    session_manager_t m;
    snprintf(json, sizeof(json),
             "{ \"interfaces\": [ { \"name\": \"%s\", \"sip\": \"10.0.0.1\", \"dip\": \"239.1.1.1\" } ],\n"
             "  \"rx_sessions\": [ { \"crop\": { \"w\": 32, \"h\": 16 } } ] }", names[i]);
    REQUIRE(make_app(&app, json) == 0);
    fake_mtl_reset();
    CHECK_EQ_INT(session_manager_init(&m, &app), 0);
    CHECK(!(fake_mtl.last_init.flags & MTL_FLAG_RX_USE_CNI));
    session_manager_cleanup(&m);
  }
}

static void test_single_session_without_display(void) {
  struct rx_app_context app;
  REQUIRE(make_app(&app, "{ " IFACE ", \"video\": { \"width\": 32, \"height\": 16 },\n"
                         "  \"rx_sessions\": [ { \"udp_port\": 20010, \"payload_type\": 112,\n"
                         "                       \"crop\": { \"w\": 32, \"h\": 16 } } ] }") == 0);
  no_display();
  fake_mtl_reset();
  fake_mtl.frames_per_session = 120; /* > 50 for the progress log */
  session_manager_t seen;
  CHECK_EQ_INT(run_app(&app, 300, &seen), 0);
  with_display();
  CHECK(seen.composite == NULL);
  CHECK_EQ_INT(fake_mtl.frames_got, 120);
  check_frames_balanced();
  CHECK_EQ_INT(fake_mtl.create_calls, 1);
  CHECK_EQ_INT(fake_mtl.free_calls, 1);
  CHECK_EQ_INT(fake_mtl.wake_calls, 1);
  const struct st20p_rx_ops* ops = &fake_mtl.ops[0];
  CHECK_EQ_STR(ops->name, "st20p_rx_0");
  CHECK_EQ_INT(ops->port.udp_port[MTL_SESSION_PORT_P], 20010);
  CHECK_EQ_INT(ops->port.payload_type, 112);
  CHECK(ops->port.ip_addr[MTL_SESSION_PORT_P][0] == 239 &&
        ops->port.ip_addr[MTL_SESSION_PORT_P][3] == 20);
  CHECK_EQ_STR(ops->port.port[MTL_SESSION_PORT_P], "0000:03:00.0");
  CHECK_EQ_INT(ops->width, 32);
  CHECK_EQ_INT(ops->height, 16);
  CHECK_EQ_INT(ops->transport_fmt, ST20_FMT_YUV_422_10BIT);
  CHECK_EQ_INT(ops->output_fmt, ST_FRAME_FMT_YUV422PLANAR10LE);
  CHECK(ops->flags & ST20P_RX_FLAG_BLOCK_GET);
}

/* Frames handed to the display thread; runs past 1 s for the fps report. */
static void test_single_session_window(void) {
  struct rx_app_context app;
  REQUIRE(make_app(&app, "{ " IFACE ", \"video\": { \"width\": 48, \"height\": 32 },\n"
                         "  \"rx_sessions\": [ { \"crop\": { \"w\": 48, \"h\": 32 } } ] }") == 0);
  fake_mtl_reset();
  fake_mtl.empty_every = 7; /* frames without data are returned by the RX thread */
  session_manager_t seen;
  CHECK_EQ_INT(run_app(&app, 1200, &seen), 0);
  CHECK(fake_mtl.frames_got > 10);
  check_frames_balanced();
}

/* No crop: the session uses the full video size. */
static void test_session_size_from_video(void) {
  struct rx_app_context app;
  REQUIRE(make_app(&app, "{ " IFACE ", \"video\": { \"width\": 40, \"height\": 20 },\n"
                         "  \"rx_sessions\": [ { } ] }") == 0);
  app.width = 40;
  app.height = 20;
  fake_mtl_reset();
  fake_mtl.frames_per_session = 5;
  CHECK_EQ_INT(run_app(&app, 100, NULL), 0);
  CHECK_EQ_INT(fake_mtl.ops[0].width, 40);
  CHECK_EQ_INT(fake_mtl.ops[0].height, 20);
  check_frames_balanced();
}

static void test_transport_format_mapping(void) {
  const struct { enum st_frame_fmt f; enum st20_fmt t; } map[] = {
      {ST_FRAME_FMT_YUV422PLANAR10LE, ST20_FMT_YUV_422_10BIT},
      {ST_FRAME_FMT_YUV422PLANAR12LE, ST20_FMT_YUV_422_12BIT},
      {ST_FRAME_FMT_YUV444PLANAR10LE, ST20_FMT_YUV_444_10BIT},
      {ST_FRAME_FMT_YUV444PLANAR12LE, ST20_FMT_YUV_444_12BIT},
      {ST_FRAME_FMT_GBRPLANAR10LE, ST20_FMT_RGB_10BIT},
      {ST_FRAME_FMT_GBRPLANAR12LE, ST20_FMT_RGB_12BIT},
      {ST_FRAME_FMT_YUV420PLANAR8, ST20_FMT_YUV_420_8BIT},
      {ST_FRAME_FMT_YUV420CUSTOM8, ST20_FMT_YUV_420_8BIT},
      {ST_FRAME_FMT_BGRA, ST20_FMT_YUV_422_10BIT}, /* unknown: default */
  };
  no_display();
  for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
    struct rx_app_context app;
    REQUIRE(make_app(&app, "{ " IFACE ", \"rx_sessions\": [ { \"crop\": { \"w\": 16, \"h\": 8 } } ] }") == 0);
    app.fmt = map[i].f;
    fake_mtl_reset();
    fake_mtl.frames_per_session = 0;
    CHECK_EQ_INT(run_app(&app, 0, NULL), 0);
    CHECK_EQ_INT(fake_mtl.ops[0].transport_fmt, map[i].t);
    CHECK_EQ_INT(fake_mtl.ops[0].output_fmt, map[i].f);
  }

  /* An explicit transport format wins over the derived one. */
  struct rx_app_context app;
  REQUIRE(make_app(&app, "{ " IFACE ", \"rx_sessions\": [ { \"crop\": { \"w\": 16, \"h\": 8 } } ] }") == 0);
  app.transport_fmt = ST20_FMT_RGB_8BIT;
  fake_mtl_reset();
  fake_mtl.frames_per_session = 0;
  CHECK_EQ_INT(run_app(&app, 0, NULL), 0);
  CHECK_EQ_INT(fake_mtl.ops[0].transport_fmt, ST20_FMT_RGB_8BIT);
  with_display();
}

/* Two sessions without display_mode: one composite window sized to the crops.
 * Frames keep coming until stop, so a thread may be waiting at the barrier then. */
static void test_composite_crops(void) {
  struct rx_app_context app;
  REQUIRE(make_app(&app, "{ " IFACE ", \"video\": { \"width\": 64, \"height\": 16 },\n"
                         "  \"rx_sessions\": [ { \"udp_port\": 20000, \"crop\": { \"w\": 32, \"h\": 16 } },\n"
                         "    { \"udp_port\": 20002, \"crop\": { \"x\": 32, \"w\": 32, \"h\": 16 } } ] }") == 0);
  fake_mtl_reset();
  session_manager_t seen;
  CHECK_EQ_INT(run_app(&app, 400, &seen), 0);
  CHECK(seen.composite != NULL);
  CHECK(fake_mtl.frames_got > 2);
  check_frames_balanced();
}

/* Composite: session 1 fails to create while session 0 already waits at the barrier for it. */
static void test_composite_create_failure_unwinds(void) {
  struct rx_app_context app;
  REQUIRE(make_app(&app, "{ " IFACE ", \"video\": { \"width\": 64, \"height\": 16 },\n"
                         "  \"rx_sessions\": [ { \"udp_port\": 20000, \"crop\": { \"w\": 32, \"h\": 16 } },\n"
                         "    { \"udp_port\": 20002, \"crop\": { \"x\": 32, \"w\": 32, \"h\": 16 } } ] }") == 0);
  fake_mtl_reset();
  fake_mtl.fail_create_at = 1;
  session_manager_t m;
  REQUIRE(session_manager_init(&m, &app) == 0);
  CHECK_EQ_INT(session_manager_start(&m, &app), -1);
  sleep_ms(100); /* let session 0 reach the barrier */
  app.exit = true;
  session_manager_cleanup(&m);
  CHECK_EQ_INT(fake_mtl.free_calls, 1);
  check_frames_balanced();
}

/* Composite window cannot open: sessions still receive. */
static void test_composite_without_display(void) {
  struct rx_app_context app;
  REQUIRE(make_app(&app, "{ " IFACE ", \"video\": { \"width\": 64, \"height\": 16 },\n"
                         "  \"rx_sessions\": [ { \"udp_port\": 20000, \"crop\": { \"w\": 32, \"h\": 16 } },\n"
                         "    { \"udp_port\": 20002, \"crop\": { \"x\": 32, \"w\": 32, \"h\": 16 } } ] }") == 0);
  no_display();
  fake_mtl_reset();
  fake_mtl.frames_per_session = 10;
  session_manager_t seen;
  CHECK_EQ_INT(run_app(&app, 200, &seen), 0);
  with_display();
  CHECK(seen.composite == NULL);
  CHECK_EQ_INT(fake_mtl.frames_got, 20);
  check_frames_balanced();
}

/* per_output: fixed windows, a same-size mirror (copied) and a different-size one (converted). */
static void test_per_output_windows_and_mirrors(void) {
  char json[2048];
  snprintf(json, sizeof(json),
           "{ " IFACE ", \"video\": { \"width\": 64, \"height\": 32 },\n"
           "  \"rx_sessions\": [ { \"udp_port\": 20000, \"crop\": { \"w\": 32, \"h\": 32 } },\n"
           "    { \"udp_port\": 20002, \"crop\": { \"x\": 32, \"w\": 32, \"h\": 32 } } ],\n"
           "  \"displays\": [\n"
           "    { \"output\": \"%s\", \"sessions\": [\n"
           "      { \"session\": 0, \"x\": 0,  \"y\": 0, \"scale_w\": 16, \"scale_h\": 16 },\n"
           "      { \"session\": 1, \"x\": 16, \"y\": 0 },\n"
           "      { \"session\": 0, \"x\": 0,  \"y\": 40, \"scale_w\": 16, \"scale_h\": 16 },\n"
           "      { \"session\": 1, \"x\": 40, \"y\": 40, \"scale_w\": 8, \"scale_h\": 8 } ] },\n"
           "    { \"output\": \"NO-SUCH-OUTPUT\", \"sessions\": [ { \"session\": 1 } ] } ] }",
           g_output);
  struct rx_app_context app;
  REQUIRE(make_app(&app, json) == 0);
  fake_mtl_reset();
  session_manager_t seen;
  CHECK_EQ_INT(run_app(&app, 1100, &seen), 0);
  CHECK(seen.composite == NULL);
  CHECK(fake_mtl.frames_got > 4);
  check_frames_balanced();
}

/* per_output by index without a window/scale: fullscreen on the session's monitor. */
static void test_per_output_fullscreen_by_index(void) {
  struct rx_app_context app;
  REQUIRE(make_app(&app, "{ " IFACE ", \"display_mode\": \"per_output\", \"video\": { \"width\": 32, \"height\": 16 },\n"
                         "  \"rx_sessions\": [ { \"crop\": { \"w\": 32, \"h\": 16 } } ] }") == 0);
  fake_mtl_reset();
  fake_mtl.frames_per_session = 8;
  CHECK_EQ_INT(run_app(&app, 300, NULL), 0);
  CHECK_EQ_INT(fake_mtl.frames_got, 8);
  check_frames_balanced();
}

/* Output not connected: no window, frames are still received and returned. */
static void test_per_output_missing_output(void) {
  struct rx_app_context app;
  REQUIRE(make_app(&app, "{ " IFACE ", \"video\": { \"width\": 32, \"height\": 16 },\n"
                         "  \"rx_sessions\": [ { \"crop\": { \"w\": 32, \"h\": 16 } } ],\n"
                         "  \"displays\": [ { \"output\": \"NO-SUCH-OUTPUT\", \"sessions\": [ { \"session\": 0 } ] } ] }") == 0);
  fake_mtl_reset();
  fake_mtl.frames_per_session = 6;
  session_manager_t seen;
  CHECK_EQ_INT(run_app(&app, 200, &seen), 0);
  CHECK_EQ_INT(fake_mtl.frames_got, 6);
  check_frames_balanced();
}

/* Second session fails to create: start fails, the first session is still stopped and freed. */
static void test_create_failure_unwinds(void) {
  struct rx_app_context app;
  REQUIRE(make_app(&app, "{ " IFACE ", \"display_mode\": \"per_output\", \"video\": { \"width\": 64, \"height\": 16 },\n"
                         "  \"rx_sessions\": [ { \"udp_port\": 20000, \"crop\": { \"w\": 32, \"h\": 16 } },\n"
                         "    { \"udp_port\": 20002, \"crop\": { \"x\": 32, \"w\": 32, \"h\": 16 } } ] }") == 0);
  no_display();
  fake_mtl_reset();
  fake_mtl.fail_create_at = 1;
  CHECK_EQ_INT(run_app(&app, 0, NULL), -1);
  with_display();
  CHECK_EQ_INT(fake_mtl.create_calls, 2);
  CHECK_EQ_INT(fake_mtl.free_calls, 1);
  CHECK_EQ_INT(fake_mtl.uninit_calls, 1);
  check_frames_balanced();
}

static void test_stop_twice(void) {
  struct rx_app_context app;
  session_manager_t m;
  REQUIRE(make_app(&app, "{ " IFACE ", \"rx_sessions\": [ { \"crop\": { \"w\": 16, \"h\": 8 } } ] }") == 0);
  no_display();
  fake_mtl_reset();
  REQUIRE(session_manager_init(&m, &app) == 0);
  REQUIRE(session_manager_start(&m, &app) == 0);
  CHECK(session_manager_is_running(&m));
  app.exit = true;
  session_manager_stop(&m);
  session_manager_stop(&m);
  CHECK_EQ_INT(fake_mtl.free_calls, 1);
  session_manager_cleanup(&m);
  session_manager_cleanup(&m);
  CHECK_EQ_INT(fake_mtl.uninit_calls, 1);
  with_display();
  check_frames_balanced();
}

/* Name of the X server's first output (Xvfb calls it "screen"). */
static int find_first_output(void) {
  Display* dpy = XOpenDisplay(NULL);
  if (!dpy) return -1;
  int n = 0;
  XRRMonitorInfo* mons = XRRGetMonitors(dpy, DefaultRootWindow(dpy), True, &n);
  XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, DefaultRootWindow(dpy));
  if (n > 0 && mons[0].noutput > 0 && res) {
    XRROutputInfo* info = XRRGetOutputInfo(dpy, res, mons[0].outputs[0]);
    if (info) {
      snprintf(g_output, sizeof(g_output), "%s", info->name);
      XRRFreeOutputInfo(info);
    }
  }
  if (res) XRRFreeScreenResources(res);
  if (mons) XRRFreeMonitors(mons);
  XCloseDisplay(dpy);
  return g_output[0] ? 0 : -1;
}

int main(void) {
  XInitThreads();
  const char* d = getenv("DISPLAY");
  if (!d || find_first_output() != 0) {
    fprintf(stderr, "needs an X server (run under xvfb-run)\n");
    return 77; /* meson: skipped */
  }
  snprintf(g_display_env, sizeof(g_display_env), "%s", d);
  logger_set_level(LOG_LEVEL_ERROR + 1);
  scratch_enter("session");

  RUN_TEST(test_null_args);
  RUN_TEST(test_init_failure);
  RUN_TEST(test_init_params);
  RUN_TEST(test_unusual_nic_names);
  RUN_TEST(test_single_session_without_display);
  RUN_TEST(test_single_session_window);
  RUN_TEST(test_session_size_from_video);
  RUN_TEST(test_transport_format_mapping);
  RUN_TEST(test_composite_crops);
  RUN_TEST(test_composite_create_failure_unwinds);
  RUN_TEST(test_composite_without_display);
  RUN_TEST(test_per_output_windows_and_mirrors);
  RUN_TEST(test_per_output_fullscreen_by_index);
  RUN_TEST(test_per_output_missing_output);
  RUN_TEST(test_create_failure_unwinds);
  RUN_TEST(test_stop_twice);

  scratch_leave();
  return test_summary();
}
