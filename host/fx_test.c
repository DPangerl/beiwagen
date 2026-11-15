/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl */
/* The effect chain (apps/beiwagen/bw_fx.h) on the host, through its C API only, as app.c drives it: each effect alone
 * at its defaults, with every knob at 0 and at 1000, and every mode; a sine, an impulse, silence. Fails on a value out
 * of +-32767, a clipped default, a default tail that does not die to silence, or (UBSan) a NaN / inf reaching the
 * conversion. Writes build/host/out/fx_<name>.wav (and fx_chain.wav) at the codec rate.
 * Run by tools/fx_test.py. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_fx.h"
#include "fw.h"

#define SECONDS(s) ((int)((s) * FW_FS))
#define MAX_FRAMES SECONDS(60)

static int32_t buf[2 * FW_BLOCK];
static int16_t wav[2 * MAX_FRAMES];
static int wav_n, failures;

static void fail(const char *what, const char *fx)
{
    printf("FAIL %s: %s\n", fx, what);
    failures++;
}

/* frames of a source through the chain, into the WAV; returns the samples at full scale, peak in *peak */
enum { SINE, IMPULSE, SILENCE };
static int run(int src, float seconds, int *peak)
{
    static double ph;
    int clipped = 0;
    for (int left = SECONDS(seconds); left > 0;) {
        const int n = left < (int)FW_BLOCK ? left : (int)FW_BLOCK;
        for (int i = 0; i < n; i++) {
            int32_t x = 0;
            if (src == SINE) {
                x = (int32_t)(16384.0 * sin(ph));
                ph += 2.0 * M_PI * 220.0 / FW_FS;
            } else if (src == IMPULSE && left == SECONDS(seconds) && i == 0) {
                x = 32000; /* under full scale: a clipped sample is the effect's */
            }
            buf[2 * i] = buf[2 * i + 1] = x;
        }
        bw_fx_process(buf, (uint32_t)n);
        for (int i = 0; i < 2 * n; i++) {
            const int32_t y = buf[i];
            if (y > 32767 || y < -32767) {
                printf("out of range: %d\n", y);
                exit(1);
            }
            if (y == 32767 || y == -32767) clipped++;
            if (abs(y) > *peak) *peak = abs(y);
            if (wav_n < 2 * MAX_FRAMES) wav[wav_n++] = (int16_t)y;
        }
        left -= n;
    }
    return clipped;
}

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static void save(const char *name)
{
    char path[128];
    snprintf(path, sizeof path, "build/host/out/fx_%s.wav", name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fail("cannot write the WAV", name);
        return;
    }
    const uint32_t bytes = (uint32_t)wav_n * 2u;
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 2);
    put32(f, FW_FS);
    put32(f, FW_FS * 4);
    put16(f, 4);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, bytes);
    fwrite(wav, 2, (size_t)wav_n, f);
    fclose(f);
    printf("wav %s (%.1f s)\n", path, wav_n / 2.0 / FW_FS);
    wav_n = 0;
}

static void lower(char *d, const char *s)
{
    while ((*d++ = (char)((*s >= 'A' && *s <= 'Z') ? *s + 32 : *s))) s++;
}

static void defaults(int fx, const int *def)
{
    for (int p = 0; p < BW_FX_PARAMS; p++) bw_fx_set(fx, p, def[p]);
}

/* a sine and an impulse at one setting: no blow-up past full scale for long */
static void excite(const char *name, const char *setting, int strict)
{
    int peak = 0;
    const int clipped = run(SINE, 1.0f, &peak) + run(IMPULSE, 1.0f, &peak);
    printf("  %-10s peak %5d, clipped %d\n", setting, peak, clipped);
    if (strict && clipped) fail("clips at its defaults", name);
    if (clipped > SECONDS(2.0f)) fail("full scale for most of a run", name);
}

/* the impulse response at the defaults, from silence (x: L R interleaved): the delay's first repeat is its time, the
   reverb's tail, undamped, falls 40 dB in 2/3 of its decay */
static void impulse(const char *name, const int16_t *x, int n)
{
    if (!strcmp(name, "delay")) {
        int best = 1;
        for (int i = 1; i < n; i++)
            if (abs(x[2 * i]) > abs(x[2 * best])) best = i;
        const double ms = best * 1000.0 / FW_FS;
        printf("  first repeat at %.1f ms (%d)\n", ms, x[2 * best]);
        if (fabs(ms - 374.0) > 3.0) fail("first repeat not at its time", name);
    } else if (!strcmp(name, "reverb")) {
        const int w = SECONDS(0.02f);
        double top = 0, t40 = 0;
        for (int i = w; i + w <= n; i += w) { /* past the dry impulse */
            double e = 0;
            for (int k = i; k < i + w; k++) e += (double)x[2 * k] * x[2 * k] + (double)x[2 * k + 1] * x[2 * k + 1];
            if (e > top) top = e;
            if (e >= top * 1e-4) t40 = (double)(i + w) / FW_FS; /* the last window above -40 dB */
        }
        printf("  tail -40 dB at %.2f s: RT60 ~%.2f s (set 1.20 s)\n", t40, t40 * 1.5);
        if (t40 * 1.5 < 0.9 || t40 * 1.5 > 1.5) fail("tail not at its decay", name);
    }
}

int main(void)
{
    char name[32], text[32];
    bw_fx_init();
    for (int fx = 0; fx < bw_fx_count(); fx++) {
        int def[BW_FX_PARAMS];
        lower(name, bw_fx_name(fx));
        printf("%s\n", bw_fx_name(fx));
        for (int p = 0; p < BW_FX_PARAMS; p++) {
            def[p] = bw_fx_get(fx, p);
            printf("  %-9s", bw_fx_param_name(fx, p));
            for (int v = 0; v <= 1000; v += 250) {
                bw_fx_set(fx, p, v);
                bw_fx_format(fx, p, text, sizeof text);
                printf(" %4d: %-9s", v, text);
            }
            bw_fx_format(fx, p, text, 4); /* cut short: still terminated */
            if (strlen(text) > 3) fail("format overruns max", name);
            printf("\n");
        }
        defaults(fx, def);
        bw_fx_enable(fx, 1);

        excite(name, "default", 1);
        int peak = 0;
        run(SILENCE, 8.0f, &peak);
        peak = 0;
        run(SILENCE, 0.1f, &peak);
        if (peak > 1) fail("the default tail does not die to silence", name);

        if (!strcmp(name, "reverb")) bw_fx_set(fx, 2, 0); /* no damping: the highs fall as fast as the rest */
        const int at = wav_n;
        run(IMPULSE, 3.0f, &peak);
        impulse(name, wav + at, (wav_n - at) / 2);

        for (int p = 0; p < BW_FX_PARAMS; p++) bw_fx_set(fx, p, 0);
        excite(name, "all 0", 0);
        for (int p = 0; p < BW_FX_PARAMS; p++) bw_fx_set(fx, p, 1000);
        excite(name, "all 1000", 0);
        for (int p = 0; p < BW_FX_PARAMS; p++) {
            const int steps = bw_fx_steps(fx, p);
            for (int k = 0; k < steps; k++) {
                defaults(fx, def);
                bw_fx_set(fx, p, k * 1000 / (steps - 1));
                bw_fx_format(fx, p, text, sizeof text);
                excite(name, text, 0);
                for (int q = 0; q < BW_FX_PARAMS; q++)
                    if (q != p) bw_fx_set(fx, q, 1000);
                excite(name, "+ all 1000", 0);
            }
        }
        defaults(fx, def);
        run(SILENCE, 0.5f, &peak);
        bw_fx_enable(fx, 0);
        run(SILENCE, 0.1f, &peak);
        save(name);
    }

    /* all of them in a row, the bypass in and out, an effect off and on again while it plays */
    for (int fx = 0; fx < bw_fx_count(); fx++) bw_fx_enable(fx, 1);
    printf("chain\n");
    excite("chain", "all on", 0);
    bw_fx_bypass(1);
    excite("chain", "bypassed", 1);
    if (!bw_fx_bypassed()) fail("bypass not kept", "chain");
    bw_fx_bypass(0);
    excite("chain", "back", 0);
    bw_fx_enable(3, 0);
    excite("chain", "delay off", 0);
    bw_fx_enable(3, 1);
    excite("chain", "delay on", 0);
    save("chain");

    if (bw_fx_name(-1)[0] || bw_fx_param_name(0, 4)[0] || bw_fx_get(9, 0)) fail("out-of-range index", "api");
    bw_fx_set(-1, 0, 5);
    bw_fx_format(0, 9, text, sizeof text);
    printf(failures ? "fx test: %d failures\n" : "fx test: ok\n", failures);
    return failures != 0;
}
