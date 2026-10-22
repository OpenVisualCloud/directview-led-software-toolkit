/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Unit tests for frame_converter: BT.601 math, every supported ST 2110 pixel
 * format to BGRA (incl. padded line strides) and nearest-neighbour scaling.
 */

#include "test_common.h"

#include <stdint.h>

#include "frame_converter.h"
#include "logger.h"
#include "st_pipeline_api.h"

/* ------------------------------------------------------------ test frames */

static uint32_t g_rand = 12345;
static uint32_t rnd(void) {
  g_rand = g_rand * 1103515245u + 12345u;
  return g_rand >> 8;
}

static bool is_12bit(enum st_frame_fmt fmt) {
  return fmt == ST_FRAME_FMT_YUV422PLANAR12LE || fmt == ST_FRAME_FMT_YUV444PLANAR12LE ||
         fmt == ST_FRAME_FMT_GBRPLANAR12LE;
}

/* 16-bit planar frame with pad_px extra pixels per line; random samples at the
 * format's bit depth, or every sample = value when value >= 0. */
static void frame_alloc(struct st_frame* f, enum st_frame_fmt fmt, uint32_t w, uint32_t h,
                        uint32_t pad_px, int value) {
  memset(f, 0, sizeof(*f));
  f->fmt = fmt;
  f->width = w;
  f->height = h;
  uint16_t mask = is_12bit(fmt) ? 0xFFF : 0x3FF;
  for (int p = 0; p < 3; p++) {
    uint32_t pw = (p > 0 && is_yuv422_format(fmt)) ? w / 2 : w;
    size_t stride = pw + pad_px;
    uint16_t* plane = malloc(stride * h * sizeof(uint16_t));
    for (size_t i = 0; i < stride * h; i++)
      plane[i] = value >= 0 ? (uint16_t)value : (uint16_t)(rnd() & mask);
    f->addr[p] = plane;
    f->linesize[p] = stride * sizeof(uint16_t);
  }
}

static void frame_free(struct st_frame* f) {
  for (int p = 0; p < ST_MAX_PLANES; p++) {
    free(f->addr[p]);
    f->addr[p] = NULL;
  }
}

/* Expected BGRA of source pixel (x, y), written from the format definitions. */
static void ref_bgra(const struct st_frame* f, uint32_t x, uint32_t y, uint8_t out[4]) {
  int sh = is_12bit(f->fmt) ? 4 : 2;
  const uint16_t* p0 = (const uint16_t*)f->addr[0] + y * (f->linesize[0] / 2);
  const uint16_t* p1 = (const uint16_t*)f->addr[1] + y * (f->linesize[1] / 2);
  const uint16_t* p2 = (const uint16_t*)f->addr[2] + y * (f->linesize[2] / 2);
  out[3] = 255;
  if (f->fmt == ST_FRAME_FMT_GBRPLANAR10LE || f->fmt == ST_FRAME_FMT_GBRPLANAR12LE) {
    out[0] = (p1[x] >> sh) & 0xFF; /* plane order is G, B, R */
    out[1] = (p0[x] >> sh) & 0xFF;
    out[2] = (p2[x] >> sh) & 0xFF;
    return;
  }
  uint32_t cx = is_yuv422_format(f->fmt) ? x / 2 : x;
  yuv_to_rgb((p0[x] >> sh) & 0xFF, (p1[cx] >> sh) & 0xFF, (p2[cx] >> sh) & 0xFF,
             &out[2], &out[1], &out[0]);
}

static const enum st_frame_fmt k_planar_fmts[] = {
    ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_YUV422PLANAR12LE,
    ST_FRAME_FMT_YUV444PLANAR10LE, ST_FRAME_FMT_YUV444PLANAR12LE,
    ST_FRAME_FMT_GBRPLANAR10LE,    ST_FRAME_FMT_GBRPLANAR12LE,
};
#define N_PLANAR_FMTS (sizeof(k_planar_fmts) / sizeof(k_planar_fmts[0]))

/* ------------------------------------------------------------------ tests */

static void test_yuv_to_rgb_reference_colours(void) {
  uint8_t r, g, b;
  yuv_to_rgb(16, 128, 128, &r, &g, &b); /* studio black */
  CHECK(r == 0 && g == 0 && b == 0);
  yuv_to_rgb(235, 128, 128, &r, &g, &b); /* studio white */
  CHECK(r == 255 && g == 255 && b == 255);
  yuv_to_rgb(81, 90, 240, &r, &g, &b); /* BT.601 red */
  CHECK(r == 255 && g == 0 && b == 0);
  yuv_to_rgb(126, 128, 128, &r, &g, &b); /* mid grey: neutral chroma keeps r == g == b */
  CHECK(r == g && g == b && r > 100 && r < 150);
  yuv_to_rgb(0, 128, 128, &r, &g, &b); /* below black clamps to 0 */
  CHECK(r == 0 && g == 0 && b == 0);
  yuv_to_rgb(255, 255, 255, &r, &g, &b); /* above white clamps to 255 */
  CHECK(r == 255 && b == 255);
}

static void test_format_helpers(void) {
  CHECK_EQ_INT(get_format_bit_shift(ST_FRAME_FMT_YUV422PLANAR10LE), 2);
  CHECK_EQ_INT(get_format_bit_shift(ST_FRAME_FMT_YUV444PLANAR10LE), 2);
  CHECK_EQ_INT(get_format_bit_shift(ST_FRAME_FMT_GBRPLANAR10LE), 2);
  CHECK_EQ_INT(get_format_bit_shift(ST_FRAME_FMT_YUV422PLANAR12LE), 4);
  CHECK_EQ_INT(get_format_bit_shift(ST_FRAME_FMT_YUV444PLANAR12LE), 4);
  CHECK_EQ_INT(get_format_bit_shift(ST_FRAME_FMT_GBRPLANAR12LE), 4);
  CHECK_EQ_INT(get_format_bit_shift(ST_FRAME_FMT_YUV420PLANAR8), 0);
  CHECK_EQ_INT(get_format_bit_shift(ST_FRAME_FMT_BGRA), 0);
  CHECK(is_yuv422_format(ST_FRAME_FMT_YUV422PLANAR10LE));
  CHECK(is_yuv422_format(ST_FRAME_FMT_YUV422PLANAR12LE));
  CHECK(!is_yuv422_format(ST_FRAME_FMT_YUV444PLANAR10LE));
  CHECK(!is_yuv422_format(ST_FRAME_FMT_GBRPLANAR12LE));
}

static void test_convert_rejects_bad_input(void) {
  struct st_frame f;
  uint8_t buf[16 * 4 * 4];
  frame_alloc(&f, ST_FRAME_FMT_YUV422PLANAR10LE, 4, 4, 0, 0);
  CHECK_EQ_INT(convert_frame_to_rgb(NULL, buf, 4, 4), -1);
  CHECK_EQ_INT(convert_frame_to_rgb(&f, NULL, 4, 4), -1);
  CHECK_EQ_INT(convert_frame_to_rgb(&f, buf, 4, 2), -1); /* size mismatch */
  CHECK_EQ_INT(convert_frame_to_rgb(&f, buf, 2, 4), -1);
  void* y = f.addr[0];
  f.addr[0] = NULL;
  CHECK_EQ_INT(convert_frame_to_rgb(&f, buf, 4, 4), -1);
  f.addr[0] = y;
  frame_free(&f);
}

static void test_convert_uniform_white_all_formats(void) {
  for (size_t i = 0; i < N_PLANAR_FMTS; i++) {
    struct st_frame f;
    enum st_frame_fmt fmt = k_planar_fmts[i];
    bool gbr = fmt == ST_FRAME_FMT_GBRPLANAR10LE || fmt == ST_FRAME_FMT_GBRPLANAR12LE;
    int sh = is_12bit(fmt) ? 4 : 2;
    frame_alloc(&f, fmt, 8, 2, 0, (gbr ? 255 : 235) << sh);
    if (!gbr) { /* neutral chroma */
      for (int p = 1; p < 3; p++)
        for (size_t k = 0; k < f.linesize[p] / 2 * f.height; k++)
          ((uint16_t*)f.addr[p])[k] = 128 << sh;
    }
    uint8_t buf[8 * 2 * 4];
    memset(buf, 0, sizeof(buf));
    CHECK_EQ_INT(convert_frame_to_rgb(&f, buf, 8, 2), 0);
    for (size_t k = 0; k < sizeof(buf); k++) CHECK_EQ_INT(buf[k], 255);
    frame_free(&f);
  }
}

/* Random content with padded lines: every pixel must match the reference. */
static void test_convert_matches_reference_all_formats(void) {
  const uint32_t w = 38, h = 7, pad = 5;
  for (size_t i = 0; i < N_PLANAR_FMTS; i++) {
    struct st_frame f;
    frame_alloc(&f, k_planar_fmts[i], w, h, pad, -1);
    uint8_t* buf = malloc(w * h * 4);
    CHECK_EQ_INT(convert_frame_to_rgb(&f, buf, w, h), 0);
    int bad = 0;
    for (uint32_t y = 0; y < h; y++)
      for (uint32_t x = 0; x < w; x++) {
        uint8_t exp[4];
        ref_bgra(&f, x, y, exp);
        if (memcmp(exp, buf + (y * w + x) * 4, 4) != 0) bad++;
      }
    if (bad) fprintf(stderr, "  fmt %d: %d pixel(s) differ\n", k_planar_fmts[i], bad);
    CHECK_EQ_INT(bad, 0);
    free(buf);
    frame_free(&f);
  }
}

static void test_convert_bit_depth_uses_top_bits(void) {
  /* 12-bit 0xABC keeps 0xAB, 10-bit 0x2F3 keeps 0xBC (drops the 2 low bits) */
  struct st_frame f;
  uint8_t px[8];
  frame_alloc(&f, ST_FRAME_FMT_GBRPLANAR12LE, 2, 1, 0, 0xABC);
  CHECK_EQ_INT(convert_frame_to_rgb(&f, px, 2, 1), 0);
  CHECK(px[0] == 0xAB && px[1] == 0xAB && px[2] == 0xAB && px[3] == 255);
  frame_free(&f);

  frame_alloc(&f, ST_FRAME_FMT_GBRPLANAR10LE, 2, 1, 0, 0x2F3);
  CHECK_EQ_INT(convert_frame_to_rgb(&f, px, 2, 1), 0);
  CHECK(px[0] == 0xBC && px[1] == 0xBC && px[2] == 0xBC);
  frame_free(&f);
}

static void test_convert_gbr_plane_order(void) {
  struct st_frame f;
  frame_alloc(&f, ST_FRAME_FMT_GBRPLANAR10LE, 1, 1, 0, 0);
  ((uint16_t*)f.addr[0])[0] = 20 << 2;  /* G */
  ((uint16_t*)f.addr[1])[0] = 30 << 2;  /* B */
  ((uint16_t*)f.addr[2])[0] = 10 << 2;  /* R */
  uint8_t px[4];
  CHECK_EQ_INT(convert_frame_to_rgb(&f, px, 1, 1), 0);
  CHECK(px[0] == 30 && px[1] == 20 && px[2] == 10 && px[3] == 255); /* B, G, R, A */
  frame_free(&f);
}

static void test_convert_yuv422_shares_chroma(void) {
  struct st_frame f;
  frame_alloc(&f, ST_FRAME_FMT_YUV422PLANAR10LE, 4, 1, 0, 126 << 2);
  ((uint16_t*)f.addr[1])[0] = 128 << 2;  /* pixels 0,1: neutral */
  ((uint16_t*)f.addr[2])[0] = 128 << 2;
  ((uint16_t*)f.addr[1])[1] = 90 << 2;   /* pixels 2,3: reddish */
  ((uint16_t*)f.addr[2])[1] = 240 << 2;
  uint8_t px[16];
  CHECK_EQ_INT(convert_frame_to_rgb(&f, px, 4, 1), 0);
  CHECK(memcmp(px, px + 4, 4) == 0);
  CHECK(memcmp(px + 8, px + 12, 4) == 0);
  CHECK(memcmp(px, px + 8, 4) != 0);
  CHECK(px[0] == px[1] && px[1] == px[2]); /* grey */
  CHECK(px[10] > px[8]);                   /* red > blue */
  frame_free(&f);
}

static void test_convert_bgra_passthrough(void) {
  uint8_t src[3 * 2 * 4], dst[3 * 2 * 4];
  for (size_t i = 0; i < sizeof(src); i++) src[i] = (uint8_t)(i * 7);
  struct st_frame f = {0};
  f.fmt = ST_FRAME_FMT_BGRA;
  f.width = 3;
  f.height = 2;
  f.addr[0] = src;
  CHECK_EQ_INT(convert_frame_to_rgb(&f, dst, 3, 2), 0);
  CHECK(memcmp(src, dst, sizeof(src)) == 0);
}

static void test_convert_i420_custom8(void) {
  const uint32_t w = 6, h = 4;
  uint8_t src[6 * 4 + 2 * 3 * 2];
  for (size_t i = 0; i < sizeof(src); i++) src[i] = (uint8_t)(rnd() & 0xFF);
  struct st_frame f = {0};
  f.fmt = ST_FRAME_FMT_YUV420CUSTOM8;
  f.width = w;
  f.height = h;
  f.addr[0] = src;
  uint8_t dst[6 * 4 * 4];
  CHECK_EQ_INT(convert_frame_to_rgb(&f, dst, w, h), 0);
  int bad = 0;
  for (uint32_t y = 0; y < h; y++)
    for (uint32_t x = 0; x < w; x++) {
      uint8_t r, g, b;
      uint8_t U = src[w * h + (y / 2) * (w / 2) + x / 2];
      uint8_t V = src[w * h + (w / 2) * (h / 2) + (y / 2) * (w / 2) + x / 2];
      yuv_to_rgb(src[y * w + x], U, V, &r, &g, &b);
      const uint8_t* px = dst + (y * w + x) * 4;
      if (px[0] != b || px[1] != g || px[2] != r || px[3] != 255) bad++;
    }
  CHECK_EQ_INT(bad, 0);
}

static void test_convert_unsupported_fills_pattern(void) {
  uint8_t src[4 * 3 * 2];
  struct st_frame f = {0};
  f.fmt = ST_FRAME_FMT_UYVY;
  f.width = 4;
  f.height = 3;
  f.addr[0] = src;
  uint8_t dst[4 * 3 * 4];
  CHECK_EQ_INT(convert_frame_to_rgb(&f, dst, 4, 3), 0);
  const uint8_t* px = dst + (2 * 4 + 3) * 4; /* x = 3, y = 2 */
  CHECK(px[0] == 5 && px[1] == 3 && px[2] == 2 && px[3] == 255);
}

static void test_convert_yuv420_planar8(void) {
  const uint32_t w = 6, h = 4;
  uint8_t py[6 * 4], pu[3 * 2], pv[3 * 2];
  for (size_t i = 0; i < sizeof(py); i++) py[i] = (uint8_t)rnd();
  for (size_t i = 0; i < sizeof(pu); i++) pu[i] = (uint8_t)rnd(), pv[i] = (uint8_t)rnd();
  struct st_frame f = {0};
  f.fmt = ST_FRAME_FMT_YUV420PLANAR8;
  f.width = w;
  f.height = h;
  f.addr[0] = py;
  f.addr[1] = pu;
  f.addr[2] = pv;
  uint8_t dst[6 * 4 * 4];
  CHECK_EQ_INT(convert_frame_to_rgb(&f, dst, w, h), 0);
  int bad = 0;
  for (uint32_t y = 0; y < h; y++)
    for (uint32_t x = 0; x < w; x++) {
      uint8_t r, g, b;
      uint32_t c = (y / 2) * (w / 2) + x / 2;
      yuv_to_rgb(py[y * w + x], pu[c], pv[c], &r, &g, &b);
      const uint8_t* px = dst + (y * w + x) * 4;
      if (px[0] != b || px[1] != g || px[2] != r || px[3] != 255) bad++;
    }
  CHECK_EQ_INT(bad, 0);
}

static void test_scaled_rejects_bad_input(void) {
  struct st_frame f;
  uint8_t buf[64];
  frame_alloc(&f, ST_FRAME_FMT_YUV422PLANAR10LE, 4, 2, 0, 0);
  CHECK_EQ_INT(convert_frame_to_rgb_scaled(NULL, buf, 2, 2), -1);
  CHECK_EQ_INT(convert_frame_to_rgb_scaled(&f, NULL, 2, 2), -1);
  CHECK_EQ_INT(convert_frame_to_rgb_scaled(&f, buf, 0, 2), -1);
  CHECK_EQ_INT(convert_frame_to_rgb_scaled(&f, buf, 2, 0), -1);
  void* v = f.addr[2];
  f.addr[2] = NULL;
  CHECK_EQ_INT(convert_frame_to_rgb_scaled(&f, buf, 2, 2), -1);
  f.addr[2] = v;
  f.fmt = ST_FRAME_FMT_BGRA; /* only 16-bit planar formats are scaled */
  CHECK_EQ_INT(convert_frame_to_rgb_scaled(&f, buf, 2, 2), -1);
  f.fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
  f.width = 0;
  CHECK_EQ_INT(convert_frame_to_rgb_scaled(&f, buf, 2, 2), -1);
  f.width = 4;
  frame_free(&f);
}

static void test_scaled_identity_equals_full_conversion(void) {
  const uint32_t w = 24, h = 6;
  for (size_t i = 0; i < N_PLANAR_FMTS; i++) {
    struct st_frame f;
    frame_alloc(&f, k_planar_fmts[i], w, h, 3, -1);
    uint8_t a[24 * 6 * 4], b[24 * 6 * 4];
    CHECK_EQ_INT(convert_frame_to_rgb(&f, a, w, h), 0);
    CHECK_EQ_INT(convert_frame_to_rgb_scaled(&f, b, w, h), 0);
    CHECK(memcmp(a, b, sizeof(a)) == 0);
    frame_free(&f);
  }
}

/* Nearest neighbour: dst(x, y) = src(x * sw / dw, y * sh / dh). */
static void check_scaled(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh) {
  for (size_t i = 0; i < N_PLANAR_FMTS; i++) {
    struct st_frame f;
    frame_alloc(&f, k_planar_fmts[i], sw, sh, 2, -1);
    uint8_t* buf = malloc((size_t)dw * dh * 4);
    CHECK_EQ_INT(convert_frame_to_rgb_scaled(&f, buf, dw, dh), 0);
    int bad = 0;
    for (uint32_t y = 0; y < dh; y++)
      for (uint32_t x = 0; x < dw; x++) {
        uint8_t exp[4];
        ref_bgra(&f, (uint32_t)((uint64_t)x * sw / dw), (uint32_t)((uint64_t)y * sh / dh), exp);
        if (memcmp(exp, buf + ((size_t)y * dw + x) * 4, 4) != 0) bad++;
      }
    if (bad) fprintf(stderr, "  fmt %d %ux%u -> %ux%u: %d pixel(s) differ\n",
                     k_planar_fmts[i], sw, sh, dw, dh, bad);
    CHECK_EQ_INT(bad, 0);
    free(buf);
    frame_free(&f);
  }
}

static void test_scaled_up_and_down(void) {
  check_scaled(16, 8, 32, 16);  /* 2x up */
  check_scaled(16, 8, 8, 4);    /* 2x down */
  check_scaled(16, 8, 5, 3);    /* odd down */
  check_scaled(12, 6, 30, 23);  /* non-integer up */
  check_scaled(20, 10, 20, 40); /* stretch one axis */
}

int main(void) {
  logger_set_level(LOG_LEVEL_ERROR + 1); /* keep expected warnings out of the output */
  scratch_enter("converter");

  RUN_TEST(test_yuv_to_rgb_reference_colours);
  RUN_TEST(test_format_helpers);
  RUN_TEST(test_convert_rejects_bad_input);
  RUN_TEST(test_convert_uniform_white_all_formats);
  RUN_TEST(test_convert_matches_reference_all_formats);
  RUN_TEST(test_convert_bit_depth_uses_top_bits);
  RUN_TEST(test_convert_gbr_plane_order);
  RUN_TEST(test_convert_yuv422_shares_chroma);
  RUN_TEST(test_convert_bgra_passthrough);
  RUN_TEST(test_convert_i420_custom8);
  RUN_TEST(test_convert_unsupported_fills_pattern);
  RUN_TEST(test_convert_yuv420_planar8);
  RUN_TEST(test_scaled_rejects_bad_input);
  RUN_TEST(test_scaled_identity_equals_full_conversion);
  RUN_TEST(test_scaled_up_and_down);

  scratch_leave();
  return test_summary();
}
