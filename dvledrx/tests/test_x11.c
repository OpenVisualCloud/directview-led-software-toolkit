/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Unit tests for x11_handler on a virtual X server (run under xvfb-run):
 * windows on outputs, frame conversion into the window image, mirrors,
 * window events (expose, resize, close, Escape) and the composite window.
 */

#include "test_common.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <X11/keysym.h>
#include <stdint.h>
#include <time.h>

#include "config_reader.h"
#include "logger.h"
#include "st_pipeline_api.h"
#include "x11_handler.h"

static Display* g_dpy; /* the test's own connection, used to send events */
static char g_display_env[64];
static char g_output[32];

static void sleep_ms(int ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

static Window win_of(const struct x11_display_ctx* d) { return (Window)(uintptr_t)d->window; }

/* Let the server deliver what we sent, then run the handler's event pump. */
static void pump(struct x11_display_ctx* d) {
  XSync(g_dpy, False);
  for (int i = 0; i < 5; i++) {
    sleep_ms(10);
    process_x11_events(d);
  }
}

static void send_event(Window w, long mask, XEvent* ev) {
  XSendEvent(g_dpy, w, False, mask, ev);
  XSync(g_dpy, False);
}

static void send_key(Window w, bool press) {
  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.xkey.type = press ? KeyPress : KeyRelease;
  ev.xkey.display = g_dpy;
  ev.xkey.window = w;
  ev.xkey.root = DefaultRootWindow(g_dpy);
  ev.xkey.keycode = XKeysymToKeycode(g_dpy, XK_Escape);
  ev.xkey.same_screen = True;
  send_event(w, press ? KeyPressMask : KeyReleaseMask, &ev);
}

static void send_expose(Window w) {
  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.xexpose.type = Expose;
  ev.xexpose.window = w;
  ev.xexpose.width = 1;
  ev.xexpose.height = 1;
  send_event(w, ExposureMask, &ev);
}

static void send_close(Window w) {
  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.xclient.type = ClientMessage;
  ev.xclient.window = w;
  ev.xclient.message_type = XInternAtom(g_dpy, "WM_PROTOCOLS", False);
  ev.xclient.format = 32;
  ev.xclient.data.l[0] = (long)XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
  send_event(w, NoEventMask, &ev);
}

static void send_reparent_to_root(Window w) {
  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.xreparent.type = ReparentNotify;
  ev.xreparent.event = w;
  ev.xreparent.window = w;
  ev.xreparent.parent = DefaultRootWindow(g_dpy);
  send_event(w, StructureNotifyMask, &ev);
}

/* 16-bit planar test frame, every sample = value (chroma neutral for YUV). */
static void frame_alloc(struct st_frame* f, enum st_frame_fmt fmt, uint32_t w, uint32_t h,
                        uint16_t value) {
  memset(f, 0, sizeof(*f));
  f->fmt = fmt;
  f->width = w;
  f->height = h;
  bool sub = fmt == ST_FRAME_FMT_YUV422PLANAR10LE || fmt == ST_FRAME_FMT_YUV422PLANAR12LE;
  bool gbr = fmt == ST_FRAME_FMT_GBRPLANAR10LE || fmt == ST_FRAME_FMT_GBRPLANAR12LE;
  bool b12 = fmt == ST_FRAME_FMT_YUV422PLANAR12LE || fmt == ST_FRAME_FMT_YUV444PLANAR12LE ||
             fmt == ST_FRAME_FMT_GBRPLANAR12LE;
  uint16_t neutral = (uint16_t)(128 << (b12 ? 4 : 2));
  for (int p = 0; p < 3; p++) {
    uint32_t pw = (p > 0 && sub) ? w / 2 : w;
    uint16_t* plane = malloc((size_t)pw * h * 2);
    for (size_t i = 0; i < (size_t)pw * h; i++) plane[i] = (p > 0 && !gbr) ? neutral : value;
    f->addr[p] = plane;
    f->linesize[p] = (size_t)pw * 2;
    f->data_size += (size_t)pw * h * 2;
  }
}

static void frame_free(struct st_frame* f) {
  for (int p = 0; p < ST_MAX_PLANES; p++) free(f->addr[p]);
  memset(f, 0, sizeof(*f));
}

static const uint8_t* px(const struct x11_display_ctx* d, uint32_t x, uint32_t y) {
  return (const uint8_t*)d->image_data + ((size_t)y * d->width + x) * 4;
}

/* ------------------------------------------------------------------ tests */

static void test_init_without_x_server(void) {
  struct x11_display_ctx d;
  unsetenv("DISPLAY");
  CHECK_EQ_INT(init_x11_display(&d, 16, 16, 0, NULL, false, NULL), -1);
  struct composite_x11_ctx c;
  CHECK_EQ_INT(init_composite_x11_display(&c, 16, 16, 1), -1);
  setenv("DISPLAY", ":250", 1); /* nothing listens there */
  CHECK_EQ_INT(init_x11_display(&d, 16, 16, 0, NULL, false, NULL), -1);
  CHECK_EQ_INT(init_composite_x11_display(&c, 16, 16, 1), -1);
  setenv("DISPLAY", g_display_env, 1);
  CHECK_EQ_INT(init_composite_x11_display(NULL, 16, 16, 1), -1);
  CHECK_EQ_INT(init_composite_x11_display(&c, 16, 16, 0), -1);
}

static void test_null_and_uninitialised_are_ignored(void) {
  struct x11_display_ctx d;
  memset(&d, 0, sizeof(d));
  cleanup_x11_display(NULL);
  cleanup_x11_display(&d);
  display_frame_x11(NULL, NULL);
  display_frame_x11(&d, NULL);
  process_x11_events(NULL);
  process_x11_events(&d);
  CHECK(!display_copy_x11(NULL, &d));
  CHECK(!display_copy_x11(&d, &d));
  struct composite_x11_ctx c;
  memset(&c, 0, sizeof(c));
  cleanup_composite_x11_display(NULL);
  cleanup_composite_x11_display(&c);
  process_composite_x11_events(NULL);
  process_composite_x11_events(&c);
  composite_write_strip(&c, NULL, 0);
  composite_write_crop(&c, NULL, NULL);
}

/* Ordinary window: frame of the same size converts straight into the image. */
static void test_normal_window_direct_frame(void) {
  struct x11_display_ctx d;
  REQUIRE(init_x11_display(&d, 8, 4, 0, NULL, false, NULL) == 0);
  CHECK(d.initialized);
  CHECK_EQ_STR(d.output_name, "(window)");
  CHECK_EQ_INT(d.wall_kind, X11_WALL_NONE);
  CHECK_EQ_INT(d.width, 8);
  CHECK_EQ_INT(d.height, 4);

  struct st_frame f;
  frame_alloc(&f, ST_FRAME_FMT_YUV422PLANAR10LE, 8, 4, 235 << 2); /* white */
  for (int i = 0; i < 6; i++) display_frame_x11(&d, &f);       /* > 5: past the diagnostics */
  const uint8_t* p = px(&d, 7, 3);
  CHECK(p[0] == 255 && p[1] == 255 && p[2] == 255);
  frame_free(&f);

  frame_alloc(&f, ST_FRAME_FMT_YUV422PLANAR12LE, 8, 4, 16 << 4); /* black, 12-bit */
  display_frame_x11(&d, &f);
  CHECK(px(&d, 0, 0)[0] == 0 && px(&d, 0, 0)[1] == 0 && px(&d, 0, 0)[2] == 0);
  frame_free(&f);

  frame_alloc(&f, ST_FRAME_FMT_YUV444PLANAR12LE, 8, 4, 235 << 4); /* white, 12-bit */
  display_frame_x11(&d, &f);
  CHECK(px(&d, 3, 2)[0] == 255 && px(&d, 3, 2)[1] == 255 && px(&d, 3, 2)[2] == 255);
  frame_free(&f);

  send_expose(win_of(&d));
  pump(&d);
  cleanup_x11_display(&d);
  CHECK(!d.initialized);
}

/* Frame size != window size: scaled conversion, BGRA scaler fallback, placeholder. */
static void test_frame_scaling_and_fallbacks(void) {
  struct x11_display_ctx d;
  REQUIRE(init_x11_display(&d, 8, 8, 1, NULL, false, NULL) == 0);

  struct st_frame f;
  frame_alloc(&f, ST_FRAME_FMT_GBRPLANAR10LE, 4, 4, 0);
  ((uint16_t*)f.addr[2])[0] = 200 << 2; /* R of source pixel (0,0) */
  display_frame_x11(&d, &f);            /* 4x4 -> 8x8 nearest */
  CHECK(px(&d, 1, 1)[2] == 200 && px(&d, 0, 0)[2] == 200 && px(&d, 2, 0)[2] == 0);
  frame_free(&f);

  uint8_t bgra[4 * 2 * 4];
  for (size_t i = 0; i < sizeof(bgra); i++) bgra[i] = (uint8_t)(i * 9);
  struct st_frame b = {0};
  b.fmt = ST_FRAME_FMT_BGRA; /* not handled by the fused scaler */
  b.width = 4;
  b.height = 2;
  b.addr[0] = bgra;
  b.data_size = sizeof(bgra);
  display_frame_x11(&d, &b);
  CHECK(memcmp(px(&d, 7, 7), bgra + (1 * 4 + 3) * 4, 4) == 0);

  frame_alloc(&f, ST_FRAME_FMT_YUV444PLANAR10LE, 8, 8, 0);
  f.width = 0; /* no size: last size is kept, conversion fails, animated placeholder */
  f.height = 0;
  display_frame_x11(&d, &f);
  frame_free(&f);
  display_frame_x11(&d, NULL); /* waiting-for-video placeholder */
  cleanup_x11_display(&d);
}

static void test_mirror_copy(void) {
  struct x11_display_ctx a, b, c;
  REQUIRE(init_x11_display(&a, 6, 4, 0, NULL, false, NULL) == 0);
  REQUIRE(init_x11_display(&b, 6, 4, 1, NULL, false, NULL) == 0);
  REQUIRE(init_x11_display(&c, 3, 4, 2, NULL, false, NULL) == 0);
  struct st_frame f;
  frame_alloc(&f, ST_FRAME_FMT_YUV444PLANAR10LE, 6, 4, 235 << 2);
  display_frame_x11(&a, &f);
  CHECK(display_copy_x11(&b, &a));
  CHECK(memcmp(a.image_data, b.image_data, 6 * 4 * 4) == 0);
  CHECK(!display_copy_x11(&c, &a)); /* different size */
  frame_free(&f);
  cleanup_x11_display(&c);
  cleanup_x11_display(&b);
  cleanup_x11_display(&a);
}

/* The window manager (here: us) resizes the window: the image follows. */
static void test_resize_and_non_shm_buffers(void) {
  struct x11_display_ctx d;
  REQUIRE(init_x11_display(&d, 8, 4, 0, NULL, false, NULL) == 0);
  d.use_shm = false; /* next buffer uses plain XPutImage */
  XResizeWindow(g_dpy, win_of(&d), 12, 6);
  pump(&d);
  CHECK_EQ_INT(d.width, 12);
  CHECK_EQ_INT(d.height, 6);
  CHECK(d.shminfo == NULL);
  struct st_frame f;
  frame_alloc(&f, ST_FRAME_FMT_YUV422PLANAR10LE, 12, 6, 235 << 2);
  display_frame_x11(&d, &f);
  CHECK(px(&d, 11, 5)[1] == 255);
  frame_free(&f);
  XResizeWindow(g_dpy, win_of(&d), 12, 6); /* same size: nothing to do */
  pump(&d);
  CHECK_EQ_INT(d.width, 12);
  cleanup_x11_display(&d);
}

/* Fullscreen on an output (by name and by index) and Escape toggling. */
static void test_fullscreen_output_and_escape(void) {
  struct x11_display_ctx d;
  REQUIRE(init_x11_display(&d, 16, 16, 0, g_output, true, NULL) == 0);
  CHECK_EQ_STR(d.output_name, g_output);
  CHECK_EQ_INT(d.wall_kind, X11_WALL_FULLSCREEN);
  CHECK(d.width == (uint32_t)d.mon_w && d.height == (uint32_t)d.mon_h);

  send_key(win_of(&d), true); /* Escape: normal window */
  pump(&d);
  CHECK(d.windowed);
  send_key(win_of(&d), true); /* held key repeating: ignored */
  pump(&d);
  CHECK(d.windowed);
  send_key(win_of(&d), false);
  send_key(win_of(&d), true); /* fresh press: back to wall mode */
  send_key(win_of(&d), false);
  pump(&d);
  CHECK(!d.windowed);
  cleanup_x11_display(&d);

  struct x11_display_ctx by_index;
  REQUIRE(init_x11_display(&by_index, 16, 16, 0, "", true, NULL) == 0);
  CHECK_EQ_STR(by_index.output_name, g_output);
  cleanup_x11_display(&by_index);
}

static void test_missing_output(void) {
  struct x11_display_ctx d;
  CHECK_EQ_INT(init_x11_display(&d, 16, 16, 0, "NO-SUCH-OUTPUT", true, NULL), -1);
  CHECK_EQ_INT(init_x11_display(&d, 16, 16, 7, NULL, true, NULL), -1); /* no 8th monitor */
  CHECK(!d.initialized);
}

/* Fixed window on an output; Escape leaves and re-enters wall mode, including
 * the WM-release (ReparentNotify) and timeout paths. */
static void test_fixed_window_and_escape(void) {
  struct crop_config rect = {10, 20, 32, 16};
  struct x11_display_ctx d;
  REQUIRE(init_x11_display(&d, 64, 64, 0, g_output, true, &rect) == 0);
  CHECK_EQ_INT(d.wall_kind, X11_WALL_FIXED);
  CHECK_EQ_INT(d.width, 32);
  CHECK_EQ_INT(d.height, 16);
  CHECK_EQ_INT(d.out_x, 10);
  CHECK_EQ_INT(d.out_y, 20);

  struct st_frame f;
  frame_alloc(&f, ST_FRAME_FMT_YUV422PLANAR10LE, 32, 16, 235 << 2);
  display_frame_x11(&d, &f);
  if (d.depth == 32) CHECK(px(&d, 0, 0)[3] == 255); /* ARGB window stays opaque */
  frame_free(&f);

  send_key(win_of(&d), true);
  send_key(win_of(&d), false);
  pump(&d);
  CHECK(d.windowed);

  send_key(win_of(&d), true); /* back: waits for the WM to release the window */
  send_key(win_of(&d), false);
  pump(&d);
  CHECK(d.wall_pending);
  send_key(win_of(&d), true); /* toggle while pending: ignored */
  send_key(win_of(&d), false);
  pump(&d);
  CHECK(d.wall_pending);
  send_reparent_to_root(win_of(&d));
  pump(&d);
  CHECK(!d.wall_pending);
  CHECK(!d.windowed);

  send_key(win_of(&d), true); /* windowed again, then back via the 1 s timeout */
  send_key(win_of(&d), false);
  pump(&d);
  send_key(win_of(&d), true);
  send_key(win_of(&d), false);
  pump(&d);
  CHECK(d.wall_pending);
  sleep_ms(1100);
  process_x11_events(&d);
  CHECK(!d.wall_pending);
  cleanup_x11_display(&d);

  struct crop_config big = {0, 0, 100000, 16}; /* larger than the output: warning only */
  REQUIRE(init_x11_display(&d, 16, 16, 0, g_output, true, &big) == 0);
  cleanup_x11_display(&d);
}

/* Composite window: crop and strip writes, expose, resize to scaled and back. */
static void test_composite_window(void) {
  struct composite_x11_ctx c;
  REQUIRE(init_composite_x11_display(&c, 8, 4, 1) == 0);
  CHECK(c.initialized);

  struct st_frame f;
  frame_alloc(&f, ST_FRAME_FMT_GBRPLANAR10LE, 4, 4, 255 << 2); /* white */
  struct crop_config crop = {6, 2, 4, 4}; /* partly outside: clamped */
  composite_write_crop(&c, &f, &crop);
  const uint8_t* img = (const uint8_t*)c.image_data;
  CHECK(img[(3 * 8 + 7) * 4] == 255); /* (7,3) written */
  CHECK(img[(1 * 8 + 7) * 4] == 0);   /* (7,1) above the crop untouched */
  frame_free(&f);

  frame_alloc(&f, ST_FRAME_FMT_GBRPLANAR10LE, 8, 4, 128 << 2);
  composite_write_strip(&c, &f, 0);
  CHECK(img[0] == 128);
  frame_free(&f);

  Window w = (Window)(uintptr_t)c.window;
  send_expose(w);
  XResizeWindow(g_dpy, w, 16, 8);
  XSync(g_dpy, False);
  sleep_ms(30);
  process_composite_x11_events(&c);
  CHECK_EQ_INT(c.win_width, 16);
  CHECK(c.scaled_ximage != NULL);
  frame_alloc(&f, ST_FRAME_FMT_GBRPLANAR10LE, 8, 4, 64 << 2);
  composite_write_strip(&c, &f, 0); /* flush through the scaled image */
  CHECK(((const uint8_t*)c.scaled_image_data)[(7 * 16 + 15) * 4] == 64);
  frame_free(&f);

  XResizeWindow(g_dpy, w, 8, 4); /* native size again: scaled image dropped */
  XSync(g_dpy, False);
  sleep_ms(30);
  process_composite_x11_events(&c);
  CHECK(c.scaled_ximage == NULL);
  cleanup_composite_x11_display(&c);
  CHECK(!c.initialized);
}

/* A session alone at a 2-session barrier would wait forever; abort releases it. */
static void test_composite_abort_releases_waiters(void) {
  composite_abort(NULL);
  struct composite_x11_ctx c;
  REQUIRE(init_composite_x11_display(&c, 4, 4, 2) == 0);
  composite_abort(&c);
  struct st_frame f;
  frame_alloc(&f, ST_FRAME_FMT_GBRPLANAR10LE, 4, 4, 255 << 2);
  struct crop_config crop = {0, 0, 4, 4};
  composite_write_crop(&c, &f, &crop); /* returns instead of blocking */
  composite_write_strip(&c, &f, 0);
  CHECK(((const uint8_t*)c.image_data)[0] == 255);
  frame_free(&f);
  cleanup_composite_x11_display(&c);
}

/* X protocol errors are logged, not fatal. */
static void test_x_error_is_logged(void) {
  struct x11_display_ctx d;
  REQUIRE(init_x11_display(&d, 4, 4, 0, NULL, false, NULL) == 0); /* installs the handler */
  XMapWindow(g_dpy, (Window)0x1fffff0); /* BadWindow */
  XSync(g_dpy, False);
  cleanup_x11_display(&d);
}

/* Close button on any window (session or composite) asks the app to stop. */
static void test_close_button(void) {
  struct x11_display_ctx d;
  REQUIRE(init_x11_display(&d, 4, 4, 0, NULL, false, NULL) == 0);
  CHECK(!x11_close_requested());
  send_close(win_of(&d));
  pump(&d);
  CHECK(x11_close_requested());
  cleanup_x11_display(&d);

  struct composite_x11_ctx c;
  REQUIRE(init_composite_x11_display(&c, 4, 4, 1) == 0);
  send_close((Window)(uintptr_t)c.window);
  sleep_ms(20);
  process_composite_x11_events(&c);
  CHECK(x11_close_requested());
  cleanup_composite_x11_display(&c);
}

static int find_first_output(void) {
  int n = 0;
  Window root = DefaultRootWindow(g_dpy);
  XRRMonitorInfo* mons = XRRGetMonitors(g_dpy, root, True, &n);
  XRRScreenResources* res = XRRGetScreenResourcesCurrent(g_dpy, root);
  if (n > 0 && mons[0].noutput > 0 && res) {
    XRROutputInfo* info = XRRGetOutputInfo(g_dpy, res, mons[0].outputs[0]);
    if (info) {
      snprintf(g_output, sizeof(g_output), "%s", info->name);
      XRRFreeOutputInfo(info);
    }
  }
  if (res) XRRFreeScreenResources(res);
  if (mons) XRRFreeMonitors(mons);
  return g_output[0] ? 0 : -1;
}

int main(void) {
  XInitThreads();
  const char* env = getenv("DISPLAY");
  if (!env || !(g_dpy = XOpenDisplay(NULL)) || find_first_output() != 0) {
    fprintf(stderr, "needs an X server (run under xvfb-run)\n");
    return 77; /* meson: skipped */
  }
  snprintf(g_display_env, sizeof(g_display_env), "%s", env);
  logger_set_level(LOG_LEVEL_ERROR + 1);
  scratch_enter("x11");

  RUN_TEST(test_init_without_x_server);
  RUN_TEST(test_null_and_uninitialised_are_ignored);
  RUN_TEST(test_normal_window_direct_frame);
  RUN_TEST(test_frame_scaling_and_fallbacks);
  RUN_TEST(test_mirror_copy);
  RUN_TEST(test_resize_and_non_shm_buffers);
  RUN_TEST(test_fullscreen_output_and_escape);
  RUN_TEST(test_missing_output);
  RUN_TEST(test_fixed_window_and_escape);
  RUN_TEST(test_composite_window);
  RUN_TEST(test_composite_abort_releases_waiters);
  RUN_TEST(test_x_error_is_logged);
  RUN_TEST(test_close_button);

  XCloseDisplay(g_dpy);
  scratch_leave();
  return test_summary();
}
