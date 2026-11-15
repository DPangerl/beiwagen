/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl */
/* The loop canceller (apps/beiwagen/bw_aec.h) on a simulated OP-1 loop, through its C API only, as app.c drives it:
 * per block bw_aec_input(y) -> "effects" (a slapback and a soft clip) -> bw_aec_output(x); the OP-1 returns
 * y = s + g * x delayed by D plus a fraction (the FM-1's cubic resampler at a fraction that may drift, the OP-1's linear
 * one), quantised to 16 bits. s, the live playing: harmonic notes with envelopes, varied levels, gaps, a loud
 * double-talk stretch, a little hiss. ERLE is measured from the known echo: 10 log(echo^2 / (e - s)^2).
 * Checks the delay found, the echo removed, double talk, a failed calibration, the pass-through when off, and that a
 * loop which runs away without the canceller does not with it; reports a gain step and drift. Writes
 * build/host/out/aec_{y,e,s}.wav. Run by tools/aec_test.py. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_aec.h"
#include "fw.h"

#define FS ((double)FW_FS_HZ)
#define HIST 16384
#define SLAP 2646 /* the effect's slapback, 60 ms */
#define WIN 2.0   /* seconds per ERLE window */
#define MAXW 64

static int failures;
static void check(int ok, const char *what)
{
    printf("  %s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static uint64_t rng, seed = 88172645463325252ull;
static double urand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (double)(rng >> 11) / 9007199254740992.0;
}

/* ---------------------------------------------------------------------------------- the live playing --- */
typedef struct {
    double f, ph, level, t, dur, gap, env;
} voice_t;

static double voice_next(voice_t *v, int loud, int silent)
{
    if (v->t >= v->dur + v->gap) { /* a new note: a minor pentatonic over two octaves from A2 */
        static const double scale[] = {0, 3, 5, 7, 10, 12, 15, 17, 19, 22, 24};
        v->f = 110.0 * pow(2.0, scale[(int)(urand() * 11)] / 12.0) * (urand() < 0.5 ? 1.0 : 2.0);
        v->level = pow(10.0, (loud ? -6.0 + 4.0 * urand() : -30.0 + 24.0 * urand()) / 20.0) * 32767.0 * 0.5;
        v->dur = 0.15 + 1.1 * urand();
        v->gap = loud ? 0.0 : urand() < 0.3 ? 0.0 : 0.8 * urand();
        v->t = 0;
    }
    const double t = v->t;
    v->t += 1.0 / FS;
    double target = 0;
    if (!silent && t < v->dur) target = t < 0.005 ? t / 0.005 : 0.6 + 0.4 * exp(-(t - 0.005) / 0.1);
    v->env += (target - v->env) * (target > v->env ? 0.05 : 1.0 / (0.05 * FS)); /* ~50 ms release */
    v->ph += 2.0 * M_PI * v->f / FS;
    if (v->ph > 2.0 * M_PI) v->ph -= 2.0 * M_PI;
    double s = 0;
    for (int k = 1; k <= 6; k++) s += sin(k * v->ph) / k;
    return s * v->env * v->level * 0.5;
}

/* ------------------------------------------------------------------------------------------- a scenario --- */
typedef struct {
    const char *name;
    int D;                      /* the loop's whole-sample delay */
    double g, g_after, step_at; /* the loop gain, and after a step at step_at s (0: none) */
    double drift, drift_t;      /* +- samples of fractional delay, over a period of drift_t s */
    double seconds;
    double enable_at;           /* s, < 0: the canceller stays off */
    double fx_gain;             /* the effects' gain (the loop gain is about g * fx_gain) */
    double play_from, play_to;  /* s plays meanwhile */
    double loud_from, loud_to;  /* the double-talk stretch: loud, no gaps, two voices */
    int wav;
} scenario_t;

typedef struct {
    int delay_found, state_end, windows;
    double delay_true, cal_seconds;
    double erle[MAXW];
    int tel[MAXW];
    double y_end; /* dBFS over the last 2 s */
    int passthrough; /* bw_aec_input left y untouched in every block while off */
} result_t;

static int16_t *wav_y, *wav_e, *wav_s;
static void wav_write(const char *path, const int16_t *d, int frames)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("cannot write %s\n", path);
        failures++;
        return;
    }
    const uint32_t bytes = (uint32_t)frames * 4u, fs = FW_FS, rate = FW_FS * 4, riff = 36 + bytes, fmt = 16;
    const uint16_t pcm = 1, chn = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f);
    fwrite(&chn, 2, 1, f);
    fwrite(&fs, 4, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&bytes, 4, 1, f);
    fwrite(d, 2, (size_t)frames * 2, f);
    fclose(f);
    printf("  wav %s\n", path);
}

static double cubic(const double *h, long at, double frac) /* Catmull-Rom between h[at] and h[at+1] */
{
    const double p0 = h[(at - 1) & (HIST - 1)], p1 = h[at & (HIST - 1)], p2 = h[(at + 1) & (HIST - 1)],
                 p3 = h[(at + 2) & (HIST - 1)];
    return p1 + 0.5 * frac * (p2 - p0 + frac * (2 * p0 - 5 * p1 + 4 * p2 - p3 + frac * (3 * (p1 - p2) + p3 - p0)));
}

static double tau(const scenario_t *sc, long t) /* the cubic resampler's fraction beyond D */
{
    return 0.4 + (sc->drift_t > 0 ? sc->drift * sin(2.0 * M_PI * t / FS / sc->drift_t) : 0.0);
}

static result_t run(const scenario_t *sc)
{
    static double xh[2][HIST], slap[2][HIST];
    static int32_t blk[2 * FW_BLOCK], yb[2 * FW_BLOCK];
    double zprev[2] = {0, 0};
    result_t r;
    memset(&r, 0, sizeof r);
    r.delay_found = -1;
    r.passthrough = 1;
    memset(xh, 0, sizeof xh);
    memset(slap, 0, sizeof slap);
    rng = seed;
    voice_t v1 = {0}, v2 = {0};
    bw_aec_init();
    const long total = (long)(sc->seconds * FS), win = (long)(WIN * FS), end2 = total - (long)(2.0 * FS);
    double echo2 = 0, res2 = 0, y2 = 0;
    long on_at = -1;
    int enabled = 0;
    for (long t0 = 0; t0 < total; t0 += FW_BLOCK) {
        const double sec = t0 / FS;
        if (!enabled && sc->enable_at >= 0 && sec >= sc->enable_at) {
            bw_aec_enable(1);
            enabled = 1;
        }
        const int loud = sec >= sc->loud_from && sec < sc->loud_to;
        const int silent = sec < sc->play_from || sec >= sc->play_to;
        const double g = sc->step_at > 0 && sec >= sc->step_at ? sc->g_after : sc->g;
        double sv[FW_BLOCK][2], ev[FW_BLOCK][2];
        for (int i = 0; i < (int)FW_BLOCK; i++) {
            const long t = t0 + i;
            const double s = voice_next(&v1, loud, silent) + voice_next(&v2, loud, silent || !loud);
            const double pos = (double)(t - sc->D) - tau(sc, t);
            const long at = (long)floor(pos);
            for (int c = 0; c < 2; c++) {
                const double s_c = s * (c ? 1.0 : 0.85) + 3.0 * (urand() - 0.5); /* and hiss, -80 dBFS */
                const double z = cubic(xh[c], at, pos - (double)at);
                const double echo = g * (c ? 0.9 : 1.0) * (0.7 * z + 0.3 * zprev[c]); /* the OP-1's resampler */
                zprev[c] = z;
                double y = round(s_c + echo);
                y = y > 32767 ? 32767 : y < -32767 ? -32767 : y;
                blk[2 * i + c] = yb[2 * i + c] = (int32_t)y;
                sv[i][c] = s_c;
                ev[i][c] = echo;
            }
        }
        bw_aec_input(blk, FW_BLOCK);
        if (bw_aec_state() == BW_AEC_OFF && memcmp(blk, yb, sizeof yb)) r.passthrough = 0;
        if (bw_aec_state() == BW_AEC_ON && on_at < 0) {
            on_at = t0;
            r.cal_seconds = sec - sc->enable_at;
            r.delay_found = bw_aec_delay();
            r.delay_true = sc->D + tau(sc, t0) + 0.3;
        }
        for (int i = 0; i < (int)FW_BLOCK; i++) {
            const long t = t0 + i;
            for (int c = 0; c < 2; c++) {
                const double e = blk[2 * i + c];
                const double in = e + 0.3 * slap[c][(t - SLAP) & (HIST - 1)]; /* the effects */
                slap[c][t & (HIST - 1)] = e;
                blk[2 * i + c] = (int32_t)lrint(32767.0 * tanh(sc->fx_gain * in / 32767.0));
                if (on_at >= 0 && t0 >= on_at + (long)(0.5 * FS)) {
                    echo2 += ev[i][c] * ev[i][c];
                    res2 += (e - sv[i][c]) * (e - sv[i][c]);
                }
                if (t >= end2) y2 += (double)yb[2 * i + c] * yb[2 * i + c];
                if (sc->wav) {
                    wav_y[2 * t + c] = (int16_t)yb[2 * i + c];
                    wav_e[2 * t + c] = (int16_t)e;
                    wav_s[2 * t + c] = (int16_t)lrint(sv[i][c]);
                }
            }
        }
        bw_aec_output(blk, FW_BLOCK);
        for (int i = 0; i < (int)FW_BLOCK; i++)
            for (int c = 0; c < 2; c++) xh[c][(t0 + i) & (HIST - 1)] = blk[2 * i + c];
        if ((t0 + FW_BLOCK) / win != t0 / win && t0 / win < MAXW) {
            const int w = (int)(t0 / win);
            r.erle[w] = echo2 > 0 && res2 > 0 ? 10.0 * log10(echo2 / res2) : NAN;
            r.tel[w] = bw_aec_erle_db();
            r.windows = w + 1;
            echo2 = res2 = 0;
        }
    }
    r.y_end = 10.0 * log10(y2 / (2.0 * (double)(total - end2) * 32767.0 * 32767.0) + 1e-12);
    r.state_end = bw_aec_state();
    if (sc->wav) {
        wav_write("build/host/out/aec_y.wav", wav_y, (int)total);
        wav_write("build/host/out/aec_e.wav", wav_e, (int)total);
        wav_write("build/host/out/aec_s.wav", wav_s, (int)total);
    }
    return r;
}

/* the worst ERLE over the windows that start at from s or later, skipping those that touch [skip_from, skip_to) */
static double worst(const result_t *r, double from, double skip_from, double skip_to)
{
    double m = 1e9;
    for (int w = 0; w < r->windows; w++) {
        const double a = w * WIN;
        if (a < from || (a + WIN > skip_from && a < skip_to) || isnan(r->erle[w])) continue;
        if (r->erle[w] < m) m = r->erle[w];
    }
    return m;
}

/* the mean ERLE (power average) over the windows from from s on */
static double mean(const result_t *r, double from)
{
    double a = 0;
    int k = 0;
    for (int w = 0; w < r->windows; w++)
        if (w * WIN >= from && !isnan(r->erle[w])) {
            a += pow(10.0, -r->erle[w] / 10.0);
            k++;
        }
    return k ? -10.0 * log10(a / k) : NAN;
}

static void report(const scenario_t *sc, const result_t *r)
{
    printf("\n%s: D %d, g %.2f", sc->name, sc->D, sc->g);
    if (sc->step_at > 0) printf(" -> %.2f at %.0f s", sc->g_after, sc->step_at);
    if (sc->drift > 0) printf(", drift +-%.2f samples over %.0f s", sc->drift, sc->drift_t);
    printf(", playing %s the calibration\n", sc->play_from <= sc->enable_at ? "during" : "after");
    printf("  delay found %d (true %.2f), calibrated in %.2f s, state %d\n", r->delay_found, r->delay_true,
           r->cal_seconds, r->state_end);
    printf("  ERLE per %.0f s, true/telemetry dB:", WIN);
    for (int w = 0; w < r->windows; w++) {
        if (isnan(r->erle[w])) printf(" -");
        else printf(" %.0f/%d", r->erle[w], r->tel[w]);
    }
    printf("\n");
}

static int delay_ok(const result_t *r)
{
    return r->delay_found >= 0 && fabs(r->delay_found - r->delay_true) <= 1.0;
}

int main(void)
{
    const int max_frames = (int)(40 * FS) + FW_BLOCK;
    wav_y = calloc((size_t)max_frames * 2, 2);
    wav_e = calloc((size_t)max_frames * 2, 2);
    wav_s = calloc((size_t)max_frames * 2, 2);

    /* calibrated before the playing, then a stationary loop: the whole echo cancelled, double talk included */
    const scenario_t quiet = {"quiet calibration", 1234, 0.8, 0.8, 0, 0, 0, 40, 0.5, 1.0, 3, 1e9, 26, 34, 1};
    result_t r = run(&quiet);
    report(&quiet, &r);
    check(r.state_end == BW_AEC_ON && delay_ok(&r), "on, the delay within +-1");
    printf("  worst window %.1f dB, in the double talk (26-34 s) %.1f dB\n", worst(&r, 4, 1e9, 1e9),
           worst(&r, 26, 34, 1e9));
    check(worst(&r, 4, 1e9, 1e9) >= 30.0, "ERLE >= 30 dB in every window, double talk included");
    check(r.tel[r.windows - 1] <= mean(&r, 4) + 1.0, "the telemetry does not overstate the mean");

    /* playing (-30..-6 dBFS notes) through the calibration */
    const scenario_t during[] = {
        {"playing during calibration", 400, 0.5, 0.5, 0, 0, 0, 30, 0.5, 1.0, 0, 1e9, 20, 26, 0},
        {"playing during calibration", 1234, 0.8, 0.8, 0, 0, 0, 30, 0.5, 1.0, 0, 1e9, 20, 26, 0},
        {"playing during calibration", 2700, 0.3, 0.3, 0, 0, 0, 30, 0.5, 1.0, 0, 1e9, 20, 26, 0},
    };
    for (int k = 0; k < 3; k++) {
        r = run(&during[k]);
        report(&during[k], &r);
        check(r.state_end == BW_AEC_ON && delay_ok(&r), "on, the delay within +-1");
        printf("  worst window %.1f dB\n", worst(&r, 4, 1e9, 1e9));
        check(worst(&r, 4, 1e9, 1e9) >= 30.0, "ERLE >= 30 dB in every window");
        check(r.tel[r.windows - 1] <= mean(&r, 4) + 1.0, "the telemetry does not overstate the mean");
    }
    /* loud: the probe drowns, the response is only as good as the bursts allow; different music, different result */
    const scenario_t loud = {"loud playing during calibration", 1234, 0.5, 0.5, 0, 0, 0, 20, 0.5, 1.0, 0, 1e9, 0, 3, 0};
    for (int k = 0; k < 3; k++) {
        seed = 88172645463325252ull + 7919u * (uint64_t)k;
        r = run(&loud);
        report(&loud, &r);
        check(r.state_end == BW_AEC_ON && delay_ok(&r), "on, the delay within +-1");
        printf("  worst window %.1f dB\n", worst(&r, 4, 1e9, 1e9));
        check(worst(&r, 4, 1e9, 1e9) >= 6.0, "ERLE >= 6 dB in every window");
        printf("  mean %.1f dB, telemetry %d dB\n", mean(&r, 4), r.tel[r.windows - 1]);
        check(fabs(r.tel[r.windows - 1] - mean(&r, 4)) <= 4.0, "the telemetry within 4 dB of the mean");
    }
    seed = 88172645463325252ull;

    /* the loop's gain changes (the OP-1's level): not followed, it needs a new calibration */
    const scenario_t step[] = {
        {"gain step down", 1234, 0.8, 0.4, 16, 0, 0, 30, 0.5, 1.0, 3, 1e9, 1e9, 1e9, 0},
        {"gain step up", 1234, 0.4, 0.8, 16, 0, 0, 30, 0.5, 1.0, 3, 1e9, 1e9, 1e9, 0},
    };
    for (int k = 0; k < 2; k++) {
        r = run(&step[k]);
        report(&step[k], &r);
        printf("  worst window before the step %.1f dB, after it %.1f dB\n", worst(&r, 4, 16, 1e9),
               worst(&r, 18, 1e9, 1e9));
    }

    /* the loop's fraction drifts: the fixed response loses the high end */
    const double drifts[] = {0.05, 0.2, 1.0};
    for (int k = 0; k < 3; k++) {
        const scenario_t d = {"drift", 1234, 0.8, 0.8, 0, drifts[k], 60, 40, 0.5, 1.0, 3, 1e9, 1e9, 1e9, 0};
        r = run(&d);
        report(&d, &r);
        printf("  worst window %.1f dB\n", worst(&r, 4, 1e9, 1e9));
    }

    /* no loop (the OP-1 does not return the FM-1): the calibration fails; and off, the input passes untouched */
    const scenario_t none = {"no loop", 1234, 0.0, 0.0, 0, 0, 0, 3, 0.5, 1.0, 0, 1e9, 1e9, 1e9, 0};
    r = run(&none);
    printf("\nno loop: state %d, delay %d\n", r.state_end, r.delay_found);
    check(r.state_end == BW_AEC_FAILED, "the calibration fails");
    const scenario_t off = {"off", 1234, 0.5, 0.5, 0, 0, 0, 3, -1, 0.9, 0, 1e9, 1e9, 1e9, 0};
    r = run(&off);
    check(r.passthrough, "off: bw_aec_input leaves y untouched");

    /* the run-away: loop gain about 1.1 (and the slapback); notes from 3 to 6 s, then nothing played until 14 s */
    scenario_t ra = {"run-away", 1234, 1.0, 1.0, 0, 0, 0, 14, -1, 1.1, 3, 6, 1e9, 1e9, 0};
    const result_t ra_off = run(&ra);
    ra.enable_at = 0.5;
    const result_t ra_on = run(&ra);
    ra.drift = 1.0;
    ra.drift_t = 60;
    const result_t ra_drift = run(&ra);
    printf("\nrun-away, loop gain ~1.1, y over 12-14 s (6 s after the playing stopped): without %.1f dBFS, with %.1f "
           "dBFS (delay %d); with and +-1 sample drift %.1f dBFS\n",
           ra_off.y_end, ra_on.y_end, ra_on.delay_found, ra_drift.y_end);
    check(ra_off.y_end > -30.0, "without the canceller the loop runs away");
    check(ra_on.y_end < -70.0, "with it the loop dies away");

    printf(failures ? "\naec_test: %d FAILED\n" : "\naec_test: passed\n", failures);
    free(wav_y);
    free(wav_e);
    free(wav_s);
    return failures ? 1 : 0;
}
