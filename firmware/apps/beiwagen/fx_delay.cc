/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl
 * Ported for Beiwagen from the Steckfeld engine (Engine/src/Delay.cpp), float only, static buffers */
/* Feedback delay with an interpolated, smoothed read position, so time changes glide instead of click. Each pass goes
 * through a low and a high cut and saturates, as a bucket-brigade line does it, so the repeats fade warm instead of
 * staying a clean copy.
 * For the pool: at most MAX_SECONDS, the line in 16 bits (a repeat never leaves +-CEILING, so +-2 full scale keeps
 * 6 dB of room). Steckfeld's other controls stay at its defaults: drive 0.5, low cut 20 Hz, stereo (no ping-pong, no
 * spread), no wow, no freeze. */
#include <string.h>

#include "fx_dsp.h"

namespace bwfx {
namespace {

const float MAX_SECONDS = 0.5f;
const float MIN_SECONDS = 0.01f;
const int SIZE = FW_FS / 2 + 4; /* frames: MAX_SECONDS and the interpolation's next sample */
const float Q = 16384.0f;       /* line units per 1.0 */
const float GLIDE = 0.0005f * AT48;
const float CEILING = 1.5f;     /* Steckfeld's 6 * 16^-drive at its default drive 0.5 */
const float LOW_CUT = 20.0f;

int16_t line[SIZE][2] FW_POOL;

struct State {
    int write;
    float delay_t, delay; /* samples */
    float feedback, mix_t, mix;
    float high_c, low_c;
    float high[2], low[2];
};
State s;

float seconds(int v) { return expo(v, MIN_SECONDS, MAX_SECONDS); }

void reset() {
    memset(line, 0, sizeof line);
    s.write = 0;
    s.delay = s.delay_t;
    s.mix = s.mix_t;
    s.high[0] = s.high[1] = s.low[0] = s.low[1] = 0.0f;
}

void apply(const int *v) {
    s.delay_t = clampf(seconds(v[0]) * FS, 1.0f, (float)(SIZE - 2));
    s.feedback = lin(v[1], 0.0f, 0.95f);
    s.high_c = one_pole(clampf(expo(v[2], 500.0f, 20000.0f), 10.0f, 0.45f * FS));
    s.low_c = one_pole(LOW_CUT);
    s.mix_t = unit(v[3]);
}

void process(float *l, float *r, int n) {
    const float dm = (s.mix_t - s.mix) / (float)n;
    float mix = s.mix;
    float *io[2] = {l, r};
    for (int i = 0; i < n; ++i) {
        mix += dm;
        s.delay += (s.delay_t - s.delay) * GLIDE;
        float pos = (float)s.write - s.delay;
        if (pos < 0.0f) pos += (float)SIZE;
        int a = (int)pos;
        const float t = pos - (float)a;
        if (a >= SIZE) a -= SIZE;
        const int b = a + 1 < SIZE ? a + 1 : 0;
        for (int c = 0; c < 2; ++c) {
            const float x0 = (float)line[a][c], x1 = (float)line[b][c];
            const float delayed = (x0 + (x1 - x0) * t) * (1.0f / Q);
            s.high[c] += (delayed - s.high[c]) * s.high_c;
            s.low[c] += (s.high[c] - s.low[c]) * s.low_c;
            const float wet = s.high[c] - s.low[c];
            const float dry = io[c][i];
            const float fed = dry + wet * s.feedback;
            line[s.write][c] = to_q(saturate(fed / CEILING) * CEILING, Q);
            io[c][i] = dry * (1.0f - mix) + wet * mix;
        }
        if (++s.write == SIZE) s.write = 0;
    }
    s.mix = s.mix_t;
    for (int c = 0; c < 2; ++c) {
        flush(s.high[c]);
        flush(s.low[c]);
    }
}

void format(int p, int v, char *out, int max) {
    if (p == 0) fmt_time(out, max, seconds(v));
    else if (p == 2) fmt_hz(out, max, expo(v, 500.0f, 20000.0f));
    else fmt_pct(out, max, v);
}

} // namespace

const Effect DELAY = {
    "Delay", {"Time", "Feedback", "Tone", "Mix"}, {926, 421, 674, 300}, {0, 0, 0, 0}, reset, apply, process, format,
};

} // namespace bwfx
