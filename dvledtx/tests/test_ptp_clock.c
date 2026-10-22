/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 *
 * Unit tests for the src/util/ptp_clock.c servo and published time model.
 *
 * The module samples the NIC PHC on a background thread and serves MTL a
 * frequency-corrected linear model over CLOCK_MONOTONIC_RAW. The sampling
 * syscalls live in phc_sample()/sampler_thread(), while sampler_update() takes
 * the two timestamps as plain integers — so the servo itself can be driven
 * with synthetic samples and no PHC hardware.
 *
 * Covers:
 *   clamp_rate()            — +/-1000 ppm ceiling
 *   sampler_update()        — first-sample anchoring, EMA rate tracking,
 *                             step-on-jump, stalled-clock guard, rate clamp
 *   model_publish()         — generation counter and slot ring wrap
 *   ptp_clock_get_time_ns() — served time, continuity across re-anchor
 *   ptp_clock_stats()       — offset / freq / sample counters
 *
 * src/util/ptp_clock.c is included directly to reach its static functions and
 * the opaque struct ptp_clock, as tests/test_main.c does for src/main.c.
 * clock_gettime is wrapped so the served time is deterministic.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* ── Deterministic CLOCK_MONOTONIC_RAW ──────────────────────────────────── */

static bool     g_fake_mono_enabled = false;
static uint64_t g_fake_mono_ns      = 0;

int __real_clock_gettime(clockid_t clk_id, struct timespec *tp);

int __wrap_clock_gettime(clockid_t clk_id, struct timespec *tp)
{
    if (g_fake_mono_enabled && clk_id == CLOCK_MONOTONIC_RAW && tp != NULL) {
        tp->tv_sec  = (time_t)(g_fake_mono_ns / 1000000000ull);
        tp->tv_nsec = (long)(g_fake_mono_ns % 1000000000ull);
        return 0;
    }
    return __real_clock_gettime(clk_id, tp);
}

static void fake_mono_set(uint64_t ns)
{
    g_fake_mono_enabled = true;
    g_fake_mono_ns      = ns;
}

#include "../src/util/ptp_clock.c"

/* ── Helpers ────────────────────────────────────────────────────────────── */

#define NS_PER_S 1000000000ull

static void clk_reset(struct ptp_clock *clk)
{
    memset(clk, 0, sizeof(*clk));
    clk->fd        = -1;
    clk->base_rate = 1.0;
    g_fake_mono_enabled = false;
    g_fake_mono_ns      = 0;
}

static const struct ptp_model *current_model(const struct ptp_clock *clk)
{
    uint64_t gen = atomic_load_explicit(&clk->gen, memory_order_acquire);
    return &clk->slots[gen % PTP_MODEL_SLOTS];
}

/* Served time at an arbitrary monotonic instant. */
static uint64_t served_at(struct ptp_clock *clk, uint64_t mono_ns)
{
    fake_mono_set(mono_ns);
    return ptp_clock_get_time_ns(clk);
}

/* ── clamp_rate ─────────────────────────────────────────────────────────── */

static void test_clamp_rate_limits_to_one_thousand_ppm(void **state)
{
    (void)state;
    assert_true(clamp_rate(1.0) == 1.0);
    assert_true(clamp_rate(2.0) == 1.0 + PTP_MAX_RATE_DEV);
    assert_true(clamp_rate(0.0) == 1.0 - PTP_MAX_RATE_DEV);

    /* A value inside the band is passed through untouched. */
    assert_true(clamp_rate(1.0 + PTP_MAX_RATE_DEV / 2) == 1.0 + PTP_MAX_RATE_DEV / 2);
}

/* ── First sample ───────────────────────────────────────────────────────── */

/* With no prior model the first sample anchors exactly on the PHC: the served
 * time must equal the sampled PHC value at that instant, with no slewing. */
static void test_first_sample_anchors_on_phc(void **state)
{
    (void)state;
    struct ptp_clock clk;
    clk_reset(&clk);

    sampler_update(&clk, 1 * NS_PER_S, 5 * NS_PER_S);

    assert_true(clk.have_model);
    assert_int_equal((int)atomic_load(&clk.stat_samples), 1);
    assert_int_equal((int)atomic_load(&clk.stat_offset_ns), 0);
    assert_int_equal((int)served_at(&clk, 1 * NS_PER_S), (int)(5 * NS_PER_S));

    /* One second later the model advances at rate 1.0. */
    assert_int_equal((int)(served_at(&clk, 2 * NS_PER_S) - 5 * NS_PER_S),
                     (int)NS_PER_S);
}

/* Before the anchor instant the model clamps rather than extrapolating
 * backwards, so the served clock never runs below the anchor. */
static void test_served_time_before_anchor_clamps(void **state)
{
    (void)state;
    struct ptp_clock clk;
    clk_reset(&clk);

    sampler_update(&clk, 10 * NS_PER_S, 100 * NS_PER_S);

    assert_int_equal((int)(served_at(&clk, 9 * NS_PER_S) - 100 * NS_PER_S), 0);
    assert_int_equal((int)(served_at(&clk, 10 * NS_PER_S) - 100 * NS_PER_S), 0);
}

/* ── Rate tracking ──────────────────────────────────────────────────────── */

/* A PHC running 40 ppm fast must be tracked, so the served model converges on
 * the real rate instead of accumulating error. This is the drift the feature
 * exists to cancel. */
static void test_servo_converges_on_constant_drift(void **state)
{
    (void)state;
    struct ptp_clock clk;
    clk_reset(&clk);

    const double drift = 1.00004; /* +40 ppm */
    uint64_t mono = 1 * NS_PER_S;
    uint64_t phc  = 1 * NS_PER_S;

    sampler_update(&clk, mono, phc);
    for (int i = 0; i < 40; i++) {
        mono += NS_PER_S;
        phc  += (uint64_t)(NS_PER_S * drift);
        sampler_update(&clk, mono, phc);
    }

    int64_t freq_ppb = 0, offset_ns = 0;
    uint64_t samples = 0;
    assert_int_equal(ptp_clock_stats(&clk, &offset_ns, &freq_ppb, &samples), 0);
    assert_int_equal((int)samples, 41);

    /* Converged to ~+40000 ppb, and the residual error per interval is small. */
    assert_in_range(freq_ppb, 38000, 42000);
    assert_true(llabs(offset_ns) < 100000); /* < 100 us */
}

/* The model rate is bounded even when the implied correction is large, so a
 * noisy PHC cannot make the served clock run away. The error must stay under
 * the step threshold, otherwise the step path runs instead of the slew path:
 * 0.9 ms of error over a 100 ms interval implies +9000 ppm, which must be
 * clamped to +1000. */
static void test_rate_is_clamped_against_wild_sample(void **state)
{
    (void)state;
    struct ptp_clock clk;
    clk_reset(&clk);

    const uint64_t t0 = 1 * NS_PER_S;
    sampler_update(&clk, t0, t0);
    sampler_update(&clk, t0 + 100000000ull, t0 + 100000000ull + 900000ull);

    const struct ptp_model *m = current_model(&clk);
    assert_true(m->rate <= 1.0 + PTP_MAX_RATE_DEV);
    assert_true(m->rate >= 1.0 - PTP_MAX_RATE_DEV);

    int64_t freq_ppb = 0;
    ptp_clock_stats(&clk, NULL, &freq_ppb, NULL);
    assert_in_range(llabs(freq_ppb), 0, 1000000);
}

/* ── Step handling ──────────────────────────────────────────────────────── */

/* An error beyond PTP_STEP_NS means the PHC jumped (ptp4l stepped it, or the
 * sampler stalled). Slewing a jump that large would take minutes, so the model
 * re-anchors directly on the new PHC value. */
static void test_large_error_steps_the_model(void **state)
{
    (void)state;
    struct ptp_clock clk;
    clk_reset(&clk);

    sampler_update(&clk, 1 * NS_PER_S, 1 * NS_PER_S);
    sampler_update(&clk, 2 * NS_PER_S, 2 * NS_PER_S);

    /* +10 ms jump, well over the 1 ms step threshold. */
    const uint64_t jumped = 3 * NS_PER_S + 10000000ull;
    sampler_update(&clk, 3 * NS_PER_S, jumped);

    const struct ptp_model *m = current_model(&clk);
    assert_int_equal((int)(m->anchor_phc_ns - 3 * NS_PER_S), 10000000);
    assert_true(m->rate == clk.base_rate);
    assert_int_equal((int)(served_at(&clk, 3 * NS_PER_S) - 3 * NS_PER_S), 10000000);
}

/* Two samples at the same monotonic instant would divide by zero in the rate
 * calculation; the stalled-clock guard must take the step path instead. */
static void test_zero_elapsed_does_not_divide_by_zero(void **state)
{
    (void)state;
    struct ptp_clock clk;
    clk_reset(&clk);

    sampler_update(&clk, 5 * NS_PER_S, 5 * NS_PER_S);
    sampler_update(&clk, 5 * NS_PER_S, 5 * NS_PER_S);

    const struct ptp_model *m = current_model(&clk);
    assert_true(isfinite(m->rate));
    assert_true(m->rate == clk.base_rate);
    assert_int_equal((int)atomic_load(&clk.stat_samples), 2);
}

/* ── Continuity ─────────────────────────────────────────────────────────── */

/* A small error is absorbed over the following interval rather than jumped:
 * the model re-anchors on its own estimate, so the served clock stays
 * continuous. A backwards step here would corrupt RTP timestamps. */
static void test_small_error_is_absorbed_continuously(void **state)
{
    (void)state;
    struct ptp_clock clk;
    clk_reset(&clk);

    sampler_update(&clk, 1 * NS_PER_S, 1 * NS_PER_S);
    sampler_update(&clk, 2 * NS_PER_S, 2 * NS_PER_S);

    uint64_t before = served_at(&clk, 3 * NS_PER_S);

    /* 100 us of error — below the 1 ms step threshold. */
    sampler_update(&clk, 3 * NS_PER_S, 3 * NS_PER_S + 100000ull);

    uint64_t after = served_at(&clk, 3 * NS_PER_S);

    /* Served time does not jump to the raw PHC value at the sample instant. */
    assert_int_equal((int)(after - before), 0);
    assert_int_equal((int)atomic_load(&clk.stat_offset_ns), 100000);
}

/* The served clock must never run backwards across a sequence of noisy
 * samples — monotonicity is what the pacing path depends on. */
static void test_served_time_never_goes_backwards(void **state)
{
    (void)state;
    struct ptp_clock clk;
    clk_reset(&clk);

    uint64_t mono = 1 * NS_PER_S;
    uint64_t phc  = 1 * NS_PER_S;
    sampler_update(&clk, mono, phc);

    uint64_t prev = served_at(&clk, mono);
    const int64_t noise[] = {0, 50000, -40000, 120000, -90000, 10000, -5000};

    for (unsigned i = 0; i < sizeof(noise) / sizeof(noise[0]); i++) {
        mono += NS_PER_S;
        phc = (uint64_t)((int64_t)(phc + NS_PER_S) + noise[i]);
        sampler_update(&clk, mono, phc);

        uint64_t now = served_at(&clk, mono);
        assert_true(now >= prev);
        prev = now;
    }
}

/* ── Model ring ─────────────────────────────────────────────────────────── */

/* Readers pick a slot by generation counter, so publishing more updates than
 * there are slots must keep serving the newest model. */
static void test_model_ring_wraps_and_serves_latest(void **state)
{
    (void)state;
    struct ptp_clock clk;
    clk_reset(&clk);

    uint64_t mono = 1 * NS_PER_S;
    uint64_t phc  = 1 * NS_PER_S;
    sampler_update(&clk, mono, phc);

    for (unsigned i = 0; i < PTP_MODEL_SLOTS * 3; i++) {
        mono += NS_PER_S;
        phc  += NS_PER_S;
        sampler_update(&clk, mono, phc);
    }

    uint64_t gen = atomic_load(&clk.gen);
    assert_int_equal((int)gen, (int)(PTP_MODEL_SLOTS * 3 + 1));

    const struct ptp_model *m = current_model(&clk);
    assert_int_equal((int)(m->anchor_mono_ns / NS_PER_S), (int)(mono / NS_PER_S));
    assert_int_equal((int)(served_at(&clk, mono) / NS_PER_S), (int)(phc / NS_PER_S));
}

/* ── Stats ──────────────────────────────────────────────────────────────── */

static void test_stats_accept_null_out_pointers(void **state)
{
    (void)state;
    struct ptp_clock clk;
    clk_reset(&clk);
    sampler_update(&clk, 1 * NS_PER_S, 1 * NS_PER_S);

    assert_int_equal(ptp_clock_stats(&clk, NULL, NULL, NULL), 0);

    uint64_t samples = 0;
    assert_int_equal(ptp_clock_stats(&clk, NULL, NULL, &samples), 0);
    assert_int_equal((int)samples, 1);
}

/* ── Device reference parsing ───────────────────────────────────────────── */

/* resolve_device() turns a config value into a /dev/ptpN path. The explicit
 * path form must pass through untouched, without touching the filesystem. */
static void test_resolve_device_passes_through_explicit_path(void **state)
{
    (void)state;
    char out[64] = {0};
    assert_int_equal(resolve_device("/dev/ptp7", out, sizeof(out)), 0);
    assert_string_equal(out, "/dev/ptp7");
}

static void test_resolve_device_rejects_garbage(void **state)
{
    (void)state;
    char out[64] = {0};
    assert_int_not_equal(resolve_device("/etc/shadow", out, sizeof(out)), 0);
    assert_int_not_equal(resolve_device("bad name!", out, sizeof(out)), 0);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_clamp_rate_limits_to_one_thousand_ppm),
        cmocka_unit_test(test_first_sample_anchors_on_phc),
        cmocka_unit_test(test_served_time_before_anchor_clamps),
        cmocka_unit_test(test_servo_converges_on_constant_drift),
        cmocka_unit_test(test_rate_is_clamped_against_wild_sample),
        cmocka_unit_test(test_large_error_steps_the_model),
        cmocka_unit_test(test_zero_elapsed_does_not_divide_by_zero),
        cmocka_unit_test(test_small_error_is_absorbed_continuously),
        cmocka_unit_test(test_served_time_never_goes_backwards),
        cmocka_unit_test(test_model_ring_wraps_and_serves_latest),
        cmocka_unit_test(test_stats_accept_null_out_pointers),
        cmocka_unit_test(test_resolve_device_passes_through_explicit_path),
        cmocka_unit_test(test_resolve_device_rejects_garbage),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
