/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl
 * Ported for Beiwagen from the Steckfeld engine (Engine/src/Reverb.cpp), float only, static buffers */
/* Feedback delay network: eight delay lines mixed into each other through a Hadamard matrix every pass. Size scales
 * the lines, Decay sets each line's loss so the tail falls 60 dB in that many seconds whatever the size, and Damping
 * makes the highs fall faster. The input goes through a pre-delay, a low cut and short allpasses that smear it before
 * it enters. Each line's read point sways slowly.
 * For the pool and the interrupt: the lines in 16 bits (+-2 full scale, rounded) and each only as long
 * as it gets, no shimmer (its reads and window), no freeze. Steckfeld's other controls stay at its defaults: low cut
 * 100 Hz, pre-delay 10 ms, Mod 0.25. */
#include <string.h>

#include "fx_dsp.h"

namespace bwfx {
namespace {

const int LINES = 8;
/* line lengths at the middle size, spread so their echoes don't line up */
const float LINE_SECONDS[LINES] = {0.0203f, 0.0241f, 0.0289f, 0.0337f, 0.0391f, 0.0453f, 0.0517f, 0.0593f};
const float SMALLEST = 0.4f, LARGEST = 2.5f; /* line scale at size 0 and 1 */
const float SWAY_HZ[LINES] = {0.31f, 0.43f, 0.53f, 0.61f, 0.73f, 0.83f, 0.97f, 1.09f};
const float SWAY = 0.25f * 0.0012f * FW_FS_HZ; /* samples each way: Mod 0.25 of 1.2 ms */
const int SWAY_ROOM = (int)(FW_FS * 3u / 10000u) + 1; /* SWAY, rounded up */
/* each line takes the input and gives the output with its own signs, two rows of the Hadamard matrix each, so left
   and right come out decorrelated */
const float LEFT_IN[LINES] = {1, 1, 1, 1, -1, -1, -1, -1};
const float RIGHT_IN[LINES] = {1, -1, -1, 1, 1, -1, -1, 1};
const float LEFT_OUT[LINES] = {1, -1, 1, -1, 1, -1, 1, -1};
const float RIGHT_OUT[LINES] = {1, 1, -1, -1, 1, 1, -1, -1};
const float DIFFUSER_SECONDS[4] = {0.00477f, 0.00359f, 0.01273f, 0.0093f};
const float DIFFUSER_GAINS[4] = {0.75f, 0.75f, 0.625f, 0.625f};
const float RIGHT_DIFFUSERS = 1.11f;
/* only there to stop a runaway: Steckfeld's 4, here 2 for 6 dB more resolution. A line at 2 is already far past the
   output's full scale (the lines are summed, then halved) */
const float CEILING = 2.0f;
const float Q = 32767.0f / CEILING; /* line units per 1.0 */
const float DAMPING_HZ = 4000.0f;   /* where Damping's high-frequency decay is measured */
const float INPUT_GAIN = 0.25f;
const float OUTPUT_GAIN = 0.5f;
const float LOW_CUT = 100.0f;
const float SIZE_GLIDE = 0.0002f * AT48; /* ~100 ms */
const float FADE = 0.002f * AT48;        /* ~10 ms */
const int PRE = FW_FS / 100;             /* the pre-delay, 10 ms */

/* the lines end to end: each the longest it reads (LARGEST, the sway, Catmull-Rom's 3) and a little; the sum of
   LINE_SECONDS is 0.3024 */
const int POOL = (int)(FW_FS * 756u / 1000u) + LINES * (SWAY_ROOM + 8);
/* the diffusers end to end, both sides: the sum of DIFFUSER_SECONDS is 0.03039, the right side's 1.11 times that */
const int DIFFUSE = (int)(FW_FS * 642u / 10000u) + 8;

int16_t lines[POOL] FW_POOL;
float diffuse[DIFFUSE] FW_POOL;
float pre[PRE][2] FW_POOL;

struct Line {
    int base, size, write;
    float length, target; /* samples, gliding to target */
    float gain, damping, damped;
    float sine, cosine, spin_s, spin_c; /* the sway */
};
struct Allpass {
    int base, size, index;
    float gain;
};
struct State {
    Line line[LINES];
    Allpass ap[2][4];
    float cut_in[2][2], cut_out[2][2];
    float low_cut_c;
    int pre_write;
    float input, input_t, mix_t, mix;
    float size, decay, damping; /* the knobs: 0..1, seconds, 0..1 */
};
State s;

void *state(int i) { return i == 0 ? &s : 0; }

float decay_seconds(int v) { return expo(v, 0.1f, 30.0f); }

/* the lines' and the diffusers' places in the pools (fixed: the longest size) */
void layout() {
    int at = 0;
    for (int i = 0; i < LINES; ++i) {
        Line &l = s.line[i];
        l.base = at;
        l.size = (int)(LINE_SECONDS[i] * LARGEST * FS) + SWAY_ROOM + 6;
        at += l.size;
    }
    at = 0;
    for (int side = 0; side < 2; ++side)
        for (int i = 0; i < 4; ++i) {
            Allpass &a = s.ap[side][i];
            const int n = (int)(DIFFUSER_SECONDS[i] * (side ? RIGHT_DIFFUSERS : 1.0f) * FS);
            a.base = at;
            a.size = n > 1 ? n : 1;
            a.gain = DIFFUSER_GAINS[i];
            at += a.size;
        }
}

/* the gains, the damping and the input from size, decay and damping; at once, as Steckfeld's update() */
void update(int snap) {
    const float scale = SMALLEST * powf(LARGEST / SMALLEST, s.size);
    /* highs fall up to twenty times faster than the rest */
    const float high_decay = s.decay * powf(0.05f, s.damping);
    const float angle = cosf(2.0f * PI * DAMPING_HZ / FS);
    for (int i = 0; i < LINES; ++i) {
        Line &l = s.line[i];
        l.target = LINE_SECONDS[i] * scale * FS;
        if (snap) l.length = l.target;
        const float seconds = l.target / FS;
        l.gain = powf(10.0f, -3.0f * seconds / s.decay);
        /* the lowpass that takes the highs' extra loss per pass at DAMPING_HZ: solve its magnitude there */
        const float loss = powf(10.0f, -3.0f * seconds * (1.0f / high_decay - 1.0f / s.decay));
        const float a = 1.0f - loss * loss;
        const float b = 1.0f - loss * loss * angle;
        l.damping = a < 1e-6f ? 0.0f : (b - sqrtf(clampf(b * b - a * a, 0.0f, 1e30f))) / a;
    }
    /* short lines pass the input round more often; feeding them less keeps the level whatever the size */
    s.input_t = INPUT_GAIN * sqrtf(scale);
    if (snap) s.input = s.input_t;
}

void reset(void *) {
    memset(lines, 0, sizeof lines);
    memset(diffuse, 0, sizeof diffuse);
    memset(pre, 0, sizeof pre);
    layout();
    for (int i = 0; i < LINES; ++i) {
        Line &l = s.line[i];
        l.write = 0;
        l.damped = 0.0f;
        const float spin = 2.0f * PI * SWAY_HZ[i] / FS;
        l.spin_s = sinf(spin);
        l.spin_c = cosf(spin);
        const float start = PI * (float)i / 4.0f;
        l.sine = sinf(start);
        l.cosine = cosf(start);
    }
    for (int side = 0; side < 2; ++side) {
        for (int i = 0; i < 4; ++i) s.ap[side][i].index = 0;
        s.cut_in[side][0] = s.cut_in[side][1] = s.cut_out[side][0] = s.cut_out[side][1] = 0.0f;
    }
    s.low_cut_c = expf(-2.0f * PI * LOW_CUT / FS);
    s.pre_write = 0;
    s.mix = s.mix_t;
    update(1);
}

void apply(void *, const int *v) {
    s.size = unit(v[0]);
    s.decay = decay_seconds(v[1]);
    s.damping = unit(v[2]);
    s.mix_t = unit(v[3]);
    if (s.line[0].size) update(0); /* laid out: glide there; else reset() does it */
}

/* Catmull-Rom read `delay` samples back from the sample about to be written; needs at least 3 */
inline float read(const Line &l, float delay) {
    float pos = (float)l.write - delay;
    if (pos < 0.0f) pos += (float)l.size;
    int i1 = (int)pos;
    const float t = pos - (float)i1;
    if (i1 >= l.size) i1 -= l.size;
    const int i0 = i1 ? i1 - 1 : l.size - 1;
    int i2 = i1 + 1, i3 = i1 + 2;
    if (i2 >= l.size) i2 -= l.size;
    if (i3 >= l.size) i3 -= l.size;
    const int16_t *p = lines + l.base;
    const float x0 = (float)p[i0], x1 = (float)p[i1], x2 = (float)p[i2], x3 = (float)p[i3];
    const float c1 = 0.5f * (x2 - x0);
    const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return (((c3 * t + c2) * t + c1) * t + x1) * (1.0f / Q);
}

inline float allpass(Allpass &a, float in) {
    float *buf = diffuse + a.base;
    const float delayed = buf[a.index];
    const float v = in + a.gain * delayed;
    buf[a.index] = v;
    if (++a.index == a.size) a.index = 0;
    return delayed - a.gain * v;
}

/* in-place fast Walsh-Hadamard transform, scaled to keep energy */
inline void hadamard(float *x) {
    for (int span = 1; span < LINES; span *= 2)
        for (int i = 0; i < LINES; i += span * 2)
            for (int j = i; j < i + span; ++j) {
                const float a = x[j], b = x[j + span];
                x[j] = a + b;
                x[j + span] = a - b;
            }
    for (int i = 0; i < LINES; ++i) x[i] *= 0.35355339f;
}

void process(void *, float *l, float *r, int n) {
    const float dm = (s.mix_t - s.mix) / (float)n;
    float mix = s.mix;
    for (int i = 0; i < n; ++i) {
        mix += dm;
        s.input += (s.input_t - s.input) * FADE;
        const float in[2] = {l[i], r[i]};
        float fed[2];
        for (int side = 0; side < 2; ++side) {
            float x = pre[s.pre_write][side]; /* PRE samples ago */
            pre[s.pre_write][side] = in[side];
            for (int k = 0; k < 2; ++k) { /* one-pole highpass, twice */
                s.cut_out[side][k] = s.low_cut_c * (s.cut_out[side][k] + x - s.cut_in[side][k]);
                s.cut_in[side][k] = x;
                x = s.cut_out[side][k];
            }
            for (int k = 0; k < 4; ++k) x = allpass(s.ap[side][k], x);
            fed[side] = x * s.input;
        }
        if (++s.pre_write == PRE) s.pre_write = 0;

        float out[LINES];
        float wet_l = 0.0f, wet_r = 0.0f;
        for (int k = 0; k < LINES; ++k) {
            Line &ln = s.line[k];
            ln.length += (ln.target - ln.length) * SIZE_GLIDE;
            const float sn = ln.sine * ln.spin_c + ln.cosine * ln.spin_s;
            const float cs = ln.cosine * ln.spin_c - ln.sine * ln.spin_s;
            const float drift = 1.5f - 0.5f * (sn * sn + cs * cs);
            ln.sine = sn * drift;
            ln.cosine = cs * drift;
            const float x = read(ln, ln.length + SWAY * ln.sine);
            ln.damped = x + (ln.damped - x) * ln.damping;
            out[k] = ln.damped * ln.gain;
            wet_l += out[k] * LEFT_OUT[k];
            wet_r += out[k] * RIGHT_OUT[k];
        }
        hadamard(out);
        for (int k = 0; k < LINES; ++k) {
            Line &ln = s.line[k];
            /* rounded, not truncated: a tail passes the lines many times, truncation would shorten it */
            const float y = clampf((out[k] + fed[0] * LEFT_IN[k] + fed[1] * RIGHT_IN[k]) * Q, -32767.0f, 32767.0f);
            lines[ln.base + ln.write] = (int16_t)(y + (y < 0.0f ? -0.5f : 0.5f));
            if (++ln.write == ln.size) ln.write = 0;
        }
        l[i] = in[0] * (1.0f - mix) + wet_l * (mix * OUTPUT_GAIN);
        r[i] = in[1] * (1.0f - mix) + wet_r * (mix * OUTPUT_GAIN);
    }
    s.mix = s.mix_t;
    for (int k = 0; k < LINES; ++k) flush(s.line[k].damped);
    for (int side = 0; side < 2; ++side)
        for (int k = 0; k < 2; ++k) {
            flush(s.cut_in[side][k]);
            flush(s.cut_out[side][k]);
        }
}

void format(int p, int v, char *out, int max) {
    if (p == 1) fmt_time(out, max, decay_seconds(v));
    else fmt_pct(out, max, v);
}

} // namespace

const Effect REVERB = {
    "Reverb", {"Size", "Decay", "Damping", "Mix"}, {500, 436, 400, 300}, {0, 0, 0, 0}, 1, state, reset, apply, process,
    format,
};

} // namespace bwfx
