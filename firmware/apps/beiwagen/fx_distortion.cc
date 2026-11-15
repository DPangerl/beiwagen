/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl
 * Ported for Beiwagen from the Steckfeld engine (Engine/src/Distortion.cpp), float only, static buffers */
/* Waveshaper with soft clip, wavefold and bitcrush. Clip and fold run at twice the rate so their harmonics don't
 * alias; the crush keeps its aliasing, that is its sound. A one-pole tone control darkens the result.
 * For the interrupt: the clip's tanh and the fold's sine are a rational function and a polynomial, not libm's (both run
 * four times a sample); drive and mix glide over a block instead of a sample. */
#include "fx_dsp.h"

namespace bwfx {
namespace {

enum { CLIP, FOLD, CRUSH };
const char *const MODES[] = {"CLIP", "FOLD", "CRUSH"};

struct State {
    int mode;
    float gain, gain_t;         /* into the shaper */
    float trim, trim_t;         /* after it: the shapers pack in more energy than they are fed */
    float mix, mix_t;
    float tone_c;
    float tone[2];
    float held[2];
    float hold_count, hold, levels;
    Oversampler2x os;
};
State s;

/* sin(x) for any x: reduced to -pi/2..pi/2, then its Taylor series to x^9 (within 1e-7) */
inline float fast_sin(float x) {
    const float turns = x * (0.5f / PI);
    x -= (float)(int)(turns + (turns < 0.0f ? -0.5f : 0.5f)) * (2.0f * PI);
    if (x > 0.5f * PI) x = PI - x;
    else if (x < -0.5f * PI) x = -PI - x;
    const float x2 = x * x;
    return x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
}

/* tanh(x) within 1e-4: a Pade approximant, which reaches 1 at 4.97 */
inline float fast_tanh(float x) {
    x = clampf(x, -4.97f, 4.97f);
    const float x2 = x * x;
    return x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2))) /
           (135135.0f + x2 * (62370.0f + x2 * (3150.0f + x2 * 28.0f)));
}

inline float shape(float x, float gain, float trim, int mode) {
    return (mode == FOLD ? fast_sin(x * gain) : fast_tanh(x * gain)) * trim;
}

/* rounds half away from zero, as roundf: |x| stays under 2^17 here */
inline float round_q(float x) { return (float)(int)(x + (x < 0.0f ? -0.5f : 0.5f)); }

void reset() {
    s.gain = s.gain_t;
    s.trim = s.trim_t;
    s.mix = s.mix_t;
    s.tone[0] = s.tone[1] = s.held[0] = s.held[1] = 0.0f;
    s.hold_count = 0.0f;
    s.os.init();
}

void apply(const int *v) {
    s.mode = choice(v[0], 3);
    const float drive = unit(v[1]);
    if (s.mode == FOLD) {
        s.gain_t = powf(10.0f, drive) * 0.5f * PI;
        s.trim_t = 0.6f;
    } else {
        s.gain_t = powf(10.0f, 1.5f * drive);
        s.trim_t = 1.0f / powf(s.gain_t, 0.25f);
    }
    s.hold = 1.0f + drive * drive * 31.0f;
    s.levels = exp2f(1.0f + 15.0f * (1.0f - drive) * (1.0f - drive));
    const float tone = unit(v[2]);
    s.tone_c = tone >= 0.999f ? 1.0f : one_pole(400.0f * powf(60.0f, tone));
    s.mix_t = unit(v[3]);
}

void process(float *l, float *r, int n) {
    const float inv = 1.0f / (float)n;
    const float dg = (s.gain_t - s.gain) * inv, dt = (s.trim_t - s.trim) * inv, dm = (s.mix_t - s.mix) * inv;
    float gain = s.gain, trim = s.trim, mix = s.mix;
    const float tc = s.tone_c;
    const int mode = s.mode;
    float *io[2] = {l, r};
    for (int i = 0; i < n; ++i) {
        gain += dg;
        trim += dt;
        mix += dm;
        float wet[2];
        if (mode == CRUSH) {
            s.hold_count += 1.0f;
            if (s.hold_count >= s.hold) {
                s.hold_count -= s.hold;
                s.held[0] = l[i];
                s.held[1] = r[i];
            }
            for (int c = 0; c < 2; ++c) wet[c] = round_q(s.held[c] * s.levels) / s.levels;
        } else {
            for (int c = 0; c < 2; ++c) {
                float x[2];
                s.os.up(io[c][i], c, x);
                x[0] = shape(x[0], gain, trim, mode);
                x[1] = shape(x[1], gain, trim, mode);
                wet[c] = s.os.down(x, c);
            }
        }
        for (int c = 0; c < 2; ++c) {
            s.tone[c] += (wet[c] - s.tone[c]) * tc;
            io[c][i] = io[c][i] * (1.0f - mix) + s.tone[c] * mix;
        }
    }
    s.gain = s.gain_t;
    s.trim = s.trim_t;
    s.mix = s.mix_t;
    flush(s.tone[0]);
    flush(s.tone[1]);
    s.os.flush_all();
}

void format(int p, int v, char *out, int max) {
    if (p == 0) fmt_text(out, max, MODES[choice(v, 3)]);
    else fmt_pct(out, max, v);
}

} // namespace

const Effect DISTORTION = {
    "DISTORTION", {"MODE", "DRIVE", "TONE", "MIX"}, {0, 400, 1000, 1000}, {3, 0, 0, 0}, reset, apply, process, format,
};

} // namespace bwfx
