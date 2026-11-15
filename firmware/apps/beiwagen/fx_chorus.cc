/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl
 * Ported for Beiwagen from the Steckfeld engine (Engine/src/Chorus.cpp), float only, static buffers */
/* Three effects swept by one LFO, the right side a quarter cycle behind the left, which is what spreads them across
 * the stereo field. Chorus and flanger read a short delay: around 15 ms for the chorus; around 3 ms for the flanger,
 * where the comb is audible as a sweep and feedback sharpens it. The phaser sweeps a chain of allpass filters
 * instead, whose notches move with the LFO.
 * For the interrupt and the pool: the LFO is a turning phasor (no sin / cos a sample), the phaser's exp2 a polynomial,
 * the line 16 bits. Feedback is no knob of its own: each mode has Steckfeld's useful amount (FEEDBACK). */
#include <string.h>

#include "fx_dsp.h"

namespace bwfx {
namespace {

enum { CHORUS_MODE, FLANGER, PHASER };
const char *const MODES[] = {"Chorus", "Flanger", "Phaser"};
const float FEEDBACK[] = {0.0f, 0.5f, 0.5f};

const int SIZE = FW_FS / 20 + 2; /* 50 ms */
const float Q = 8192.0f;         /* +-4 full scale: the flanger's feedback piles up */
const float CHORUS_CENTRE = 0.015f, CHORUS_SWEEP = 0.007f;
const float FLANGER_CENTRE = 0.003f, FLANGER_SWEEP = 0.0025f;
const float GLIDE = 0.0005f * AT48;
const float PHASER_CENTRE = 700.0f; /* Hz */
const float PHASER_OCTAVES = 2.5f;  /* each way at full depth */
const float PHASER_FADE = 0.002f * AT48;
const float TONE_HZ = 8000.0f;      /* where the delayed voice rolls off, as out of a bucket-brigade line */
const int STAGES = 4;

int16_t line[SIZE][2] FW_POOL;

struct State {
    int mode, write;
    float spin_s, spin_c, sine, cosine; /* the LFO */
    float depth, cur_depth, cur_centre, phaser_depth, phaser_blend;
    float feedback_t, feedback, mix_t, mix, tone_c;
    float ap_in[2][STAGES], ap_out[2][STAGES];
    float phased[2], tone[2];
};
State s;

/* 2^x, within 2e-6 relative: 2^floor(x) in the exponent bits, a polynomial for the rest */
inline float fast_exp2(float x) {
    int n = (int)x;
    if ((float)n > x) --n;
    const float f = x - (float)n;
    const float p =
        1.0f + f * (0.69314718f + f * (0.24022650f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
    const uint32_t bits = (uint32_t)(n + 127) << 23;
    float scale;
    memcpy(&scale, &bits, sizeof scale);
    return p * scale;
}

float rate(int v) { return expo(v, 0.05f, 10.0f); }

void centre_depth(float &centre, float &depth) {
    const int fl = s.mode == FLANGER;
    centre = (fl ? FLANGER_CENTRE : CHORUS_CENTRE) * FS;
    depth = s.depth * (fl ? FLANGER_SWEEP : CHORUS_SWEEP) * FS;
}

void reset() {
    memset(line, 0, sizeof line);
    s.write = 0;
    s.sine = 0.0f;
    s.cosine = 1.0f;
    centre_depth(s.cur_centre, s.cur_depth);
    s.phaser_depth = s.depth;
    s.phaser_blend = s.mode == PHASER ? 1.0f : 0.0f;
    s.feedback = s.feedback_t;
    s.mix = s.mix_t;
    memset(s.ap_in, 0, sizeof s.ap_in);
    memset(s.ap_out, 0, sizeof s.ap_out);
    s.phased[0] = s.phased[1] = s.tone[0] = s.tone[1] = 0.0f;
    s.tone_c = one_pole(TONE_HZ);
}

void apply(const int *v) {
    s.mode = choice(v[0], 3);
    const float w = 2.0f * PI * rate(v[1]) / FS;
    s.spin_s = sinf(w);
    s.spin_c = cosf(w);
    s.depth = unit(v[2]);
    s.mix_t = unit(v[3]);
    s.feedback_t = FEEDBACK[s.mode];
}

inline float read(int c, float delay) {
    float pos = (float)s.write - delay;
    if (pos < 0.0f) pos += (float)SIZE;
    int a = (int)pos;
    const float t = pos - (float)a;
    if (a >= SIZE) a -= SIZE;
    const int b = a + 1 < SIZE ? a + 1 : 0;
    const float x0 = (float)line[a][c], x1 = (float)line[b][c];
    return (x0 + (x1 - x0) * t) * (1.0f / Q);
}

/* one side through the allpass chain, each stage turning the phase a quarter cycle at the swept frequency */
inline float phase(int c, float in, float sweep) {
    const float hz = PHASER_CENTRE * fast_exp2(sweep * s.phaser_depth * PHASER_OCTAVES);
    const float angle = PI * hz / FS; /* tan of a small angle, near enough this far below Nyquist */
    const float k = (angle - 1.0f) / (angle + 1.0f);
    float x = in + s.phased[c] * s.feedback;
    for (int i = 0; i < STAGES; ++i) {
        const float y = k * x + s.ap_in[c][i] - k * s.ap_out[c][i];
        s.ap_in[c][i] = x;
        s.ap_out[c][i] = y;
        x = y;
    }
    s.phased[c] = x;
    return x;
}

void process(float *l, float *r, int n) {
    const float dm = (s.mix_t - s.mix) / (float)n;
    float mix = s.mix;
    float centre_t, depth_t;
    centre_depth(centre_t, depth_t);
    const float blend_t = s.mode == PHASER ? 1.0f : 0.0f;
    for (int i = 0; i < n; ++i) {
        mix += dm;
        /* glide, so turning a knob or switching mode sweeps the taps instead of clicking */
        s.cur_centre += (centre_t - s.cur_centre) * GLIDE;
        s.cur_depth += (depth_t - s.cur_depth) * GLIDE;
        s.phaser_depth += (s.depth - s.phaser_depth) * GLIDE;
        s.feedback += (s.feedback_t - s.feedback) * GLIDE;
        s.phaser_blend += (blend_t - s.phaser_blend) * PHASER_FADE;
        if (fabsf(blend_t - s.phaser_blend) < 1e-4f) s.phaser_blend = blend_t;

        const float sn = s.sine * s.spin_c + s.cosine * s.spin_s;
        const float cs = s.cosine * s.spin_c - s.sine * s.spin_s;
        const float drift = 1.5f - 0.5f * (sn * sn + cs * cs); /* keeps it on the circle */
        s.sine = sn * drift;
        s.cosine = cs * drift;
        const float sweep[2] = {s.sine, s.cosine};
        const float in[2] = {l[i], r[i]};

        float delayed[2] = {0.0f, 0.0f};
        if (s.phaser_blend < 1.0f)
            for (int c = 0; c < 2; ++c) {
                s.tone[c] += (read(c, s.cur_centre + s.cur_depth * sweep[c]) - s.tone[c]) * s.tone_c;
                delayed[c] = s.tone[c];
            }
        for (int c = 0; c < 2; ++c) line[s.write][c] = to_q(in[c] + delayed[c] * s.feedback, Q);
        if (++s.write == SIZE) s.write = 0;

        float phased[2] = {0.0f, 0.0f};
        if (s.phaser_blend > 0.0f) {
            for (int c = 0; c < 2; ++c) phased[c] = phase(c, in[c], sweep[c]);
        } else if (s.phased[0] != 0.0f || s.phased[1] != 0.0f) {
            /* left idle, the chain starts its next turn from silence */
            memset(s.ap_in, 0, sizeof s.ap_in);
            memset(s.ap_out, 0, sizeof s.ap_out);
            s.phased[0] = s.phased[1] = 0.0f;
        }
        const float b = s.phaser_blend;
        l[i] = in[0] * (1.0f - mix) + (delayed[0] * (1.0f - b) + phased[0] * b) * mix;
        r[i] = in[1] * (1.0f - mix) + (delayed[1] * (1.0f - b) + phased[1] * b) * mix;
    }
    s.mix = s.mix_t;
    for (int c = 0; c < 2; ++c) {
        flush(s.tone[c]);
        flush(s.phased[c]);
        for (int i = 0; i < STAGES; ++i) {
            flush(s.ap_in[c][i]);
            flush(s.ap_out[c][i]);
        }
    }
}

void format(int p, int v, char *out, int max) {
    if (p == 0) fmt_text(out, max, MODES[choice(v, 3)]);
    else if (p == 1) fmt_hz(out, max, rate(v));
    else fmt_pct(out, max, v);
}

} // namespace

const Effect CHORUS = {
    "Chorus", {"Mode", "Rate", "Depth", "Mix"}, {0, 523, 500, 500}, {3, 0, 0, 0}, reset, apply, process, format,
};

} // namespace bwfx
