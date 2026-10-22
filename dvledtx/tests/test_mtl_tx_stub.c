/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 *
 * Stub-based unit tests for the hardware-facing functions in src/mtl/mtl_tx.c.
 *
 * Strategy
 * --------
 * The MTL library entry points are intercepted at link time with
 * `-Wl,--wrap=<sym>` so NO real MTL/DPDK hardware is touched.  Each wrapper
 * returns a caller-controlled value via a small set of file-scope globals,
 * letting us exercise every success and failure branch of:
 *
 *   mtl_tx_init()           — MTL library init (success / mtl_init NULL)
 *   mtl_tx_uninit()         — release (handle present / already NULL)
 *   mtl_tx_session_create() — ST20P session (success / bad fmt / create NULL /
 *                             udp_port + payload_type defaulting)
 *   mtl_tx_session_free()   — free (handle present / already NULL)
 *   mtl_tx_send_yuv_frame() — get/copy/put (guards / get NULL / put fail /
 *                             per-session + shared-decoder timestamp paths)
 *   mtl_tx_send_raw_yuv()   — raw buffer send (guards / get NULL / put fail /
 *                             loop wrap-around)
 *
 * Compiled with -DENABLE_MTL_TX so the whole file is in scope.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/pixfmt.h>
#include <libavutil/frame.h>

#include "app_context.h"
#include "core/session_manager.h"
#include "mtl/mtl_tx.h"
#include "util/logger.h"

/* =========================================================================
 * Wrapper control state + MTL library stubs (--wrap targets)
 * ========================================================================= */
static char        g_mtl_obj;      /* dummy backing object for a fake mtl_handle    */
static char        g_sess_obj;     /* dummy backing object for a fake st20p handle   */
#define FAKE_MTL   ((mtl_handle)&g_mtl_obj)
#define FAKE_SESS  ((st20p_tx_handle)&g_sess_obj)

static mtl_handle       g_init_ret;        /* value __wrap_mtl_init returns          */
static st20p_tx_handle  g_create_ret;      /* value __wrap_st20p_tx_create returns    */
static size_t           g_frame_size_ret;  /* value __wrap_st20p_tx_frame_size returns*/
static struct st_frame* g_get_frame_ret;   /* value __wrap_st20p_tx_get_frame returns */
static int              g_put_frame_ret;    /* value __wrap_st20p_tx_put_frame returns */
static int              g_uninit_calls;     /* how many times mtl_uninit was invoked   */
static int              g_free_calls;       /* how many times st20p_tx_free was invoked*/

mtl_handle __wrap_mtl_init(struct mtl_init_params* p);
int __wrap_mtl_uninit(mtl_handle mt);
enum mtl_pmd_type __wrap_mtl_pmd_by_port_name(const char* port);
st20p_tx_handle __wrap_st20p_tx_create(mtl_handle mt, struct st20p_tx_ops* ops);
int __wrap_st20p_tx_free(st20p_tx_handle handle);
size_t __wrap_st20p_tx_frame_size(st20p_tx_handle handle);
struct st_frame* __wrap_st20p_tx_get_frame(st20p_tx_handle handle);
int __wrap_st20p_tx_put_frame(st20p_tx_handle handle, struct st_frame* frame);

mtl_handle __wrap_mtl_init(struct mtl_init_params* p) { (void)p; return g_init_ret; }
int __wrap_mtl_uninit(mtl_handle mt) { (void)mt; g_uninit_calls++; return 0; }
enum mtl_pmd_type __wrap_mtl_pmd_by_port_name(const char* port) {
    (void)port; return MTL_PMD_DPDK_USER;
}
st20p_tx_handle __wrap_st20p_tx_create(mtl_handle mt, struct st20p_tx_ops* ops) {
    (void)mt; (void)ops; return g_create_ret;
}
int __wrap_st20p_tx_free(st20p_tx_handle handle) { (void)handle; g_free_calls++; return 0; }
size_t __wrap_st20p_tx_frame_size(st20p_tx_handle handle) { (void)handle; return g_frame_size_ret; }
struct st_frame* __wrap_st20p_tx_get_frame(st20p_tx_handle handle) { (void)handle; return g_get_frame_ret; }
int __wrap_st20p_tx_put_frame(st20p_tx_handle handle, struct st_frame* frame) {
    (void)handle; (void)frame; return g_put_frame_ret;
}

/* =========================================================================
 * Fixtures
 * ========================================================================= */
static int setup(void **state)
{
    (void)state;
    g_init_ret       = FAKE_MTL;
    g_create_ret     = FAKE_SESS;
    g_frame_size_ret = 512;
    g_get_frame_ret  = NULL;
    g_put_frame_ret  = 0;
    g_uninit_calls   = 0;
    g_free_calls     = 0;
    return 0;
}

/* Build a minimal app context with `nics` NICs and `sessions` sessions. */
static void app_init(struct dvledtx_context* app, int nics, int sessions)
{
    memset(app, 0, sizeof(*app));
    app->nic_count = nics;
    app->nics = calloc((size_t)nics, sizeof(struct nic_config));
    assert_non_null(app->nics);
    for (int i = 0; i < nics; i++)
        snprintf(app->nics[i].port, PORT_NAME_LEN, "0000:00:%02d.0", i);

    app->st20p_sessions = sessions;
    app->session_net = calloc((size_t)sessions, sizeof(struct tx_session_net));
    assert_non_null(app->session_net);
    for (int i = 0; i < sessions; i++) {
        app->session_net[i].nic_index    = 0;
        app->session_net[i].udp_port      = (uint16_t)(20000 + i * 2);
        app->session_net[i].payload_type  = 96;
    }
    app->fps         = 30;
    app->fmt         = AV_PIX_FMT_YUV422P10LE;
    app->udp_port    = 20000;
    app->payload_type = 96;
}

static void app_free(struct dvledtx_context* app)
{
    free(app->nics);
    free(app->session_net);
}

/* Allocate an st_frame with tightly-packed YUV422P10LE 16x16 plane buffers. */
static struct st_frame* make_st_frame(void)
{
    struct st_frame* f = calloc(1, sizeof(*f));
    assert_non_null(f);
    f->addr[0] = calloc(1, 16 * 16 * 2); /* Y  = 512 */
    f->addr[1] = calloc(1, 8  * 16 * 2); /* Cb = 256 */
    f->addr[2] = calloc(1, 8  * 16 * 2); /* Cr = 256 */
    assert_non_null(f->addr[0]);
    return f;
}

static void free_st_frame(struct st_frame* f)
{
    free(f->addr[0]);
    free(f->addr[1]);
    free(f->addr[2]);
    free(f);
}

/* Allocate a decoded 16x16 YUV422P10LE source AVFrame with real plane data. */
static AVFrame* make_src_frame(void)
{
    AVFrame* src = av_frame_alloc();
    assert_non_null(src);
    src->width  = 16;
    src->height = 16;
    src->format = AV_PIX_FMT_YUV422P10LE;
    assert_int_equal(av_frame_get_buffer(src, 0), 0);
    return src;
}

/* =========================================================================
 * mtl_tx_init / mtl_tx_uninit
 * ========================================================================= */
static void test_mtl_tx_init_success(void **state)
{
    (void)state;
    struct dvledtx_context app;
    app_init(&app, 2, 3);
    /* One session points at an out-of-range NIC to exercise the guard. */
    app.session_net[2].nic_index = 99;

    session_manager_t mgr;
    memset(&mgr, 0, sizeof(mgr));
    g_init_ret = FAKE_MTL;

    assert_int_equal(mtl_tx_init(&mgr, &app), 0);
    assert_ptr_equal(mgr.mtl, FAKE_MTL);

    app_free(&app);
}

static void test_mtl_tx_init_failure(void **state)
{
    (void)state;
    struct dvledtx_context app;
    app_init(&app, 1, 1);

    session_manager_t mgr;
    memset(&mgr, 0, sizeof(mgr));
    g_init_ret = NULL; /* mtl_init fails */

    assert_int_equal(mtl_tx_init(&mgr, &app), -1);
    assert_null(mgr.mtl);

    app_free(&app);
}

static void test_mtl_tx_uninit(void **state)
{
    (void)state;
    session_manager_t mgr;
    memset(&mgr, 0, sizeof(mgr));

    mgr.mtl = FAKE_MTL;
    mtl_tx_uninit(&mgr);
    assert_null(mgr.mtl);
    assert_int_equal(g_uninit_calls, 1);

    /* Second call is a no-op (handle already NULL). */
    mtl_tx_uninit(&mgr);
    assert_int_equal(g_uninit_calls, 1);
}

/* =========================================================================
 * mtl_tx_session_create / mtl_tx_session_free
 * ========================================================================= */
static void test_session_create_success(void **state)
{
    (void)state;
    struct dvledtx_context app;
    app_init(&app, 1, 1);
    session_manager_t mgr;
    memset(&mgr, 0, sizeof(mgr));
    mgr.mtl = FAKE_MTL;

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.app = &app;
    ctx.crop_width = 16;
    ctx.crop_height = 16;
    snprintf(ctx.session_name, sizeof(ctx.session_name), "s0");

    g_create_ret     = FAKE_SESS;
    g_frame_size_ret = 512;

    assert_int_equal(mtl_tx_session_create(&mgr, &ctx, &app, 0), 0);
    assert_ptr_equal(ctx.handle, FAKE_SESS);
    assert_int_equal((int)ctx.frame_size, 512);

    app_free(&app);
}

static void test_session_create_defaults_port_and_pt(void **state)
{
    (void)state;
    struct dvledtx_context app;
    app_init(&app, 1, 1);
    /* Force the udp_port==0 and payload_type==0 defaulting branches. */
    app.session_net[0].udp_port     = 0;
    app.session_net[0].payload_type = 0;

    session_manager_t mgr;
    memset(&mgr, 0, sizeof(mgr));
    mgr.mtl = FAKE_MTL;

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.app = &app;
    ctx.crop_width = 16;
    ctx.crop_height = 16;

    g_create_ret = FAKE_SESS;
    assert_int_equal(mtl_tx_session_create(&mgr, &ctx, &app, 0), 0);

    app_free(&app);
}

static void test_session_create_bad_format(void **state)
{
    (void)state;
    struct dvledtx_context app;
    app_init(&app, 1, 1);
    app.fmt = AV_PIX_FMT_RGB24; /* unsupported -> transport/input fmt == -1 */

    session_manager_t mgr;
    memset(&mgr, 0, sizeof(mgr));
    mgr.mtl = FAKE_MTL;

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.app = &app;
    ctx.crop_width = 16;
    ctx.crop_height = 16;

    assert_int_equal(mtl_tx_session_create(&mgr, &ctx, &app, 0), -1);
    assert_null(ctx.handle);

    app_free(&app);
}

static void test_session_create_handle_null(void **state)
{
    (void)state;
    struct dvledtx_context app;
    app_init(&app, 1, 1);
    session_manager_t mgr;
    memset(&mgr, 0, sizeof(mgr));
    mgr.mtl = FAKE_MTL;

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.app = &app;
    ctx.crop_width = 16;
    ctx.crop_height = 16;

    g_create_ret = NULL; /* st20p_tx_create fails */
    assert_int_equal(mtl_tx_session_create(&mgr, &ctx, &app, 0), -1);
    assert_null(ctx.handle);

    app_free(&app);
}

static void test_session_free(void **state)
{
    (void)state;
    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.handle = FAKE_SESS;
    mtl_tx_session_free(&ctx);
    assert_null(ctx.handle);
    assert_int_equal(g_free_calls, 1);

    /* Already NULL -> no-op */
    mtl_tx_session_free(&ctx);
    assert_int_equal(g_free_calls, 1);
}

/* =========================================================================
 * mtl_tx_send_yuv_frame
 * ========================================================================= */
static void test_send_yuv_guards(void **state)
{
    (void)state;
    struct dvledtx_context app;
    app_init(&app, 1, 1);
    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.app = &app;

    /* NULL handle */
    ctx.handle = NULL;
    AVFrame* src = make_src_frame();
    assert_int_equal(mtl_tx_send_yuv_frame(&ctx, src, 0, 0, 16, 16), -1);

    /* NULL src */
    ctx.handle = FAKE_SESS;
    assert_int_equal(mtl_tx_send_yuv_frame(&ctx, NULL, 0, 0, 16, 16), -1);

    /* get_frame returns NULL */
    g_get_frame_ret = NULL;
    assert_int_equal(mtl_tx_send_yuv_frame(&ctx, src, 0, 0, 16, 16), -1);

    av_frame_free(&src);
    app_free(&app);
}

static void test_send_yuv_success_per_session(void **state)
{
    (void)state;
    struct dvledtx_context app;
    app_init(&app, 1, 1);
    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.app = &app;
    ctx.handle = FAKE_SESS;
    ctx.frames_sent = 99; /* next send hits the "% 100 == 0" log branch */

    AVFrame* src = make_src_frame();
    struct st_frame* frame = make_st_frame();
    g_get_frame_ret = frame;
    g_put_frame_ret = 0;

    assert_int_equal(mtl_tx_send_yuv_frame(&ctx, src, 0, 0, 16, 16), 0);
    assert_int_equal((int)ctx.frames_sent, 100);

    free_st_frame(frame);
    av_frame_free(&src);
    app_free(&app);
}

static void test_send_yuv_success_shared_decoder(void **state)
{
    (void)state;
    struct dvledtx_context app;
    app_init(&app, 1, 1);
    struct shared_decode_ctx sd;
    memset(&sd, 0, sizeof(sd));
    sd.frame_counter = 7;

    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.app = &app;
    ctx.handle = FAKE_SESS;
    ctx.shared_dec = &sd; /* timestamp derived from shared frame_counter */

    AVFrame* src = make_src_frame();
    struct st_frame* frame = make_st_frame();
    g_get_frame_ret = frame;
    g_put_frame_ret = 0;

    assert_int_equal(mtl_tx_send_yuv_frame(&ctx, src, 0, 0, 16, 16), 0);
    assert_int_equal((int)ctx.frames_sent, 1);

    free_st_frame(frame);
    av_frame_free(&src);
    app_free(&app);
}

static void test_send_yuv_put_fail(void **state)
{
    (void)state;
    struct dvledtx_context app;
    app_init(&app, 1, 1);
    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.app = &app;
    ctx.handle = FAKE_SESS;

    AVFrame* src = make_src_frame();
    struct st_frame* frame = make_st_frame();
    g_get_frame_ret = frame;
    g_put_frame_ret = -1; /* put_frame fails */

    assert_int_equal(mtl_tx_send_yuv_frame(&ctx, src, 0, 0, 16, 16), -1);

    free_st_frame(frame);
    av_frame_free(&src);
    app_free(&app);
}

/* =========================================================================
 * mtl_tx_send_raw_yuv
 * ========================================================================= */
static void test_send_raw_guards(void **state)
{
    (void)state;
    struct dvledtx_context app;
    app_init(&app, 1, 1);
    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.app = &app;

    /* NULL handle */
    assert_int_equal(mtl_tx_send_raw_yuv(&ctx), -1);

    /* handle set but no source buffer */
    ctx.handle = FAKE_SESS;
    assert_int_equal(mtl_tx_send_raw_yuv(&ctx), -1);

    /* source buffer set but source_size == 0 */
    uint8_t buf[2048];
    ctx.source_buffer = buf;
    ctx.source_size = 0;
    assert_int_equal(mtl_tx_send_raw_yuv(&ctx), -1);

    /* get_frame returns NULL */
    ctx.source_size = sizeof(buf);
    ctx.frame_size = 512;
    g_get_frame_ret = NULL;
    assert_int_equal(mtl_tx_send_raw_yuv(&ctx), -1);

    app_free(&app);
}

static void test_send_raw_success_and_wraparound(void **state)
{
    (void)state;
    struct dvledtx_context app;
    app_init(&app, 1, 1);
    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.app = &app;
    ctx.handle = FAKE_SESS;

    uint8_t buf[2048];
    memset(buf, 0xAB, sizeof(buf));
    ctx.source_buffer = buf;
    ctx.source_size = sizeof(buf);
    ctx.frame_size = 512;

    struct st_frame* frame = make_st_frame();
    g_get_frame_ret = frame;
    g_put_frame_ret = 0;

    /* Normal send: current_pos advances by frame_size. */
    ctx.current_pos = 0;
    ctx.frames_sent = 99; /* hit the "% 100 == 0" log branch */
    assert_int_equal(mtl_tx_send_raw_yuv(&ctx), 0);
    assert_int_equal((int)ctx.current_pos, 512);
    assert_int_equal((int)ctx.frames_sent, 100);

    /* Wrap-around: current_pos + frame_size > source_size resets to 0. */
    ctx.current_pos = 1800; /* 1800 + 512 = 2312 > 2048 */
    assert_int_equal(mtl_tx_send_raw_yuv(&ctx), 0);
    assert_int_equal((int)ctx.current_pos, 512);

    free_st_frame(frame);
    app_free(&app);
}

static void test_send_raw_put_fail(void **state)
{
    (void)state;
    struct dvledtx_context app;
    app_init(&app, 1, 1);
    struct st20p_tx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.app = &app;
    ctx.handle = FAKE_SESS;

    uint8_t buf[2048];
    ctx.source_buffer = buf;
    ctx.source_size = sizeof(buf);
    ctx.frame_size = 512;

    struct st_frame* frame = make_st_frame();
    g_get_frame_ret = frame;
    g_put_frame_ret = -1; /* put_frame fails */

    assert_int_equal(mtl_tx_send_raw_yuv(&ctx), -1);

    free_st_frame(frame);
    app_free(&app);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup(test_mtl_tx_init_success, setup),
        cmocka_unit_test_setup(test_mtl_tx_init_failure, setup),
        cmocka_unit_test_setup(test_mtl_tx_uninit, setup),
        cmocka_unit_test_setup(test_session_create_success, setup),
        cmocka_unit_test_setup(test_session_create_defaults_port_and_pt, setup),
        cmocka_unit_test_setup(test_session_create_bad_format, setup),
        cmocka_unit_test_setup(test_session_create_handle_null, setup),
        cmocka_unit_test_setup(test_session_free, setup),
        cmocka_unit_test_setup(test_send_yuv_guards, setup),
        cmocka_unit_test_setup(test_send_yuv_success_per_session, setup),
        cmocka_unit_test_setup(test_send_yuv_success_shared_decoder, setup),
        cmocka_unit_test_setup(test_send_yuv_put_fail, setup),
        cmocka_unit_test_setup(test_send_raw_guards, setup),
        cmocka_unit_test_setup(test_send_raw_success_and_wraparound, setup),
        cmocka_unit_test_setup(test_send_raw_put_fail, setup),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
