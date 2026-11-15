/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl
 * Ported for Beiwagen from the Steckfeld engine (Engine/src/Saturate.h, Oversampler.cpp), float only, static buffers */
/* What the effects (fx_*.cc) share with the chain (bw_fx.cc). Internal to them: app.c includes bw_fx.h only.
 * Signals are float, +-1 full scale, stereo as two arrays of one block. Single precision throughout: the FPU has no
 * double. */
#ifndef BW_FX_DSP_H
#define BW_FX_DSP_H
#include <math.h>
#include <stdint.h>

#include "fw.h"

namespace bwfx {

const float FS = FW_FS_HZ;
const float PI = 3.14159265f;
/* Steckfeld's per-sample glide and fade coefficients are tuned at 48 kHz: the same times at this rate */
const float AT48 = 48000.0f / FW_FS_HZ;

/* An effect as the chain drives it. apply() turns the knobs (0..1000) into targets; reset() clears the state and the
   lines and jumps every glide to its target; process() runs a block in place. apply and process run in the
   interrupt, apply at a block's start; reset there too, before the first block after the effect was off. */
struct Effect {
    const char *name;
    const char *param[4];
    int def[4];
    int steps[4]; /* a choice parameter's positions, 0 = continuous */
    void (*reset)();
    void (*apply)(const int *v);
    void (*process)(float *l, float *r, int n);
    void (*format)(int p, int v, char *out, int max);
};
extern const Effect DISTORTION, FILTER, CHORUS, DELAY, REVERB;

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
inline float unit(int v) { return clampf((float)v * 0.001f, 0.0f, 1.0f); }
inline float lin(int v, float lo, float hi) { return lo + (hi - lo) * unit(v); }
inline float expo(int v, float lo, float hi) { return lo * powf(hi / lo, unit(v)); }
inline int choice(int v, int n) { return v * n / 1001; }
/* the coefficient of a one-pole lowpass at hz */
inline float one_pole(float hz) { return 1.0f - expf(-2.0f * PI * hz / FS); }
/* a recursive state that has died away: zero before it turns denormal (once a block) */
inline void flush(float &s) { if (fabsf(s) < 1e-20f) s = 0.0f; }

/* tanh to within 2.5%, without the exponentials (Saturate.h) */
inline float saturate(float x) {
    const float c = clampf(x, -3.0f, 3.0f);
    const float s = c * c;
    return c * (27.0f + s) / (27.0f + 9.0f * s);
}

/* 16-bit delay lines: q = the line's units per 1.0. Truncation toward zero, so a recirculating tail dies to 0 instead
   of a limit cycle */
inline int16_t to_q(float x, float q) { return (int16_t)clampf(x * q, -32767.0f, 32767.0f); }

/* Runs a nonlinear stage at twice the rate (Oversampler.cpp): an eighth-order Butterworth at 0.45 of the base rate,
   as four biquads, on the way up and on the way down. */
struct Oversampler2x {
    struct Biquad {
        float b0, b1, b2, a1, a2;
        float z1[2], z2[2];
        float run(float x, int c) {
            const float y = x * b0 + z1[c];
            z1[c] = x * b1 - y * a1 + z2[c];
            z2[c] = x * b2 - y * a2;
            return y;
        }
    };
    Biquad up_[4], down_[4];

    void init();
    void flush_all();
    /* one sample in, two out: the input stuffed with a zero, made up for by the gain */
    void up(float x, int c, float out[2]) {
        float a = x * 2.0f, b = 0.0f;
        for (int i = 0; i < 4; ++i) a = up_[i].run(a, c);
        for (int i = 0; i < 4; ++i) b = up_[i].run(b, c);
        out[0] = a;
        out[1] = b;
    }
    float down(const float in[2], int c) {
        float a = in[0], b = in[1];
        for (int i = 0; i < 4; ++i) a = down_[i].run(a, c);
        for (int i = 0; i < 4; ++i) b = down_[i].run(b, c);
        return b;
    }
};

/* the display strings (bw_fx.cc) */
void fmt_hz(char *out, int max, float hz);
void fmt_time(char *out, int max, float seconds);
void fmt_pct(char *out, int max, int v);
void fmt_text(char *out, int max, const char *s);

} // namespace bwfx

#endif
