/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl
 * Ported for Beiwagen from the Steckfeld engine (Engine/src/Filter.cpp), float only, static buffers */
/* Trapezoidal-integrated state variable filter (Andrew Simper / Cytomic), run at twice the sample rate. The
 * resonating state always saturates, as the transistors of an analog filter do, so resonance bites rather than rings
 * cleanly; at full resonance the loss is gone and the filter sings on its own. Drive overdrives the input too. At
 * 24 dB a second, plain Butterworth stage follows, so the resonance stays the first stage's.
 * For the interrupt: cutoff, resonance and drive glide (~20 ms) and the coefficients follow every 8 samples, not
 * every sample. No Morph mode: it needs a fifth knob. filter_oversample(.., 0) runs an instance at the sample rate
 * instead (the mixer's economy inserts). */
#include "fx_dsp.h"

namespace bwfx {
namespace {

const float MAX_DRIVE_GAIN = 6.0f;
const float OPEN_CEILING = 2.5f;
const float SELF_OSCILLATION = 0.9f;
const float NEGATIVE_DAMPING = 0.03f;
const float BASS_DROP = 0.5f;
const float BUTTERWORTH = 1.41421356f;
const float FS2 = 2.0f * FW_FS_HZ;
const int SUB = 8;                                   /* samples a coefficient update */
const float GLIDE = (float)SUB / (0.02f * FW_FS_HZ); /* a step every SUB samples: ~20 ms */

enum { LP, BP, HP, NOTCH };
const char *const MODES[] = {"LP 12", "LP 24", "BP 12", "BP 24", "HP 12", "HP 24", "Notch 12", "Notch 24"};

struct State {
    int mode, steep;
    float log_cut_t, res_t, drive_t; /* targets: log2 Hz, 0..1, 0..1 */
    float log_cut, res, drive;       /* where the glides are */
    float g, k, a1, a2, a3, low_gain, b1, b2, b3;
    float drive_gain, drive_trim, ceiling;
    float ic1[2], ic2[2], ic3[2], ic4[2];
    Oversampler2x os;
    int single;                      /* at the sample rate, not twice it (filter_oversample) */
    float rate;                      /* the rate the core runs at */
};
State inst[FILTER_INSTANCES];

void *state(int i) { return i >= 0 && i < FILTER_INSTANCES ? &inst[i] : 0; }

void coefficients(State &s) {
    const float hz = clampf(exp2f(s.log_cut), 10.0f, s.rate * 0.49f);
    s.g = tanf(PI * hz / s.rate);
    const float r = s.res;
    const float past = (r > SELF_OSCILLATION ? r - SELF_OSCILLATION : 0.0f) / (1.0f - SELF_OSCILLATION);
    s.k = 2.0f * (1.0f - r) - NEGATIVE_DAMPING * past;
    s.a1 = 1.0f / (1.0f + s.g * (s.g + s.k));
    s.a2 = s.g * s.a1;
    s.a3 = s.g * s.a2;
    s.low_gain = 1.0f - BASS_DROP * r;
    s.b1 = 1.0f / (1.0f + s.g * (s.g + BUTTERWORTH));
    s.b2 = s.g * s.b1;
    s.b3 = s.g * s.b2;
    s.drive_gain = 1.0f + (MAX_DRIVE_GAIN - 1.0f) * s.drive;
    /* quiet signals come out louder and loud ones flatter, so the level stays about where it was */
    s.drive_trim = 1.0f / sqrtf(s.drive_gain);
    s.ceiling = OPEN_CEILING - s.drive;
}

/* the 24 dB mode's second stage, the same response as the first */
inline float second(State &s, float in, int c) {
    const float v3 = in - s.ic4[c];
    const float v1 = s.b1 * s.ic3[c] + s.b2 * v3;
    const float v2 = s.ic4[c] + s.b2 * s.ic3[c] + s.b3 * v3;
    s.ic3[c] = 2.0f * v1 - s.ic3[c];
    s.ic4[c] = 2.0f * v2 - s.ic4[c];
    switch (s.mode) {
    case HP: return in - BUTTERWORTH * v1 - v2;
    case BP: return BUTTERWORTH * v1;
    case NOTCH: return in - BUTTERWORTH * v1;
    default: return v2;
    }
}

inline float core(State &s, float in, int c) {
    const float x = s.drive > 0.0f ? saturate(in * s.drive_gain) * s.drive_trim : in;
    const float v3 = x - s.ic2[c];
    const float v1 = s.a1 * s.ic1[c] + s.a2 * v3;
    const float v2 = s.ic2[c] + s.a2 * s.ic1[c] + s.a3 * v3;
    /* the saturation acts in continuous time, so only this sample's share of it, g, is applied: that way the
       self-oscillation settles at the same level whatever the cutoff */
    const float v1n = 2.0f * v1 - s.ic1[c];
    s.ic1[c] = v1n + (s.g < 1.0f ? s.g : 1.0f) * (s.ceiling * saturate(v1n / s.ceiling) - v1n);
    s.ic2[c] = 2.0f * v2 - s.ic2[c];
    float y;
    switch (s.mode) {
    case HP: y = x - s.k * v1 - v2; break;
    case BP: y = v1; break;
    case NOTCH: y = x - s.k * v1; break;
    default: y = v2 * s.low_gain; break;
    }
    return s.steep ? second(s, y, c) : y;
}

void reset(void *st) {
    State &s = *(State *)st;
    s.rate = s.single ? FW_FS_HZ : FS2;
    s.log_cut = s.log_cut_t;
    s.res = s.res_t;
    s.drive = s.drive_t;
    for (int c = 0; c < 2; ++c) s.ic1[c] = s.ic2[c] = s.ic3[c] = s.ic4[c] = 0.0f;
    s.os.init();
    coefficients(s);
}

void apply(void *st, const int *v) {
    State &s = *(State *)st;
    /* log2 of 20 Hz .. 20 kHz, without logf: libm's holds a constant tools/build.py takes for a ROM address */
    s.log_cut_t = 4.3219281f + 9.9657843f * unit(v[0]);
    s.res_t = unit(v[1]);
    s.drive_t = unit(v[2]);
    const int m = choice(v[3], 8);
    const int steep = m & 1;
    if (steep != s.steep) s.ic3[0] = s.ic3[1] = s.ic4[0] = s.ic4[1] = 0.0f;
    s.steep = steep;
    s.mode = m >> 1;
}

void process(void *st, float *l, float *r, int n) {
    State &s = *(State *)st;
    float *io[2] = {l, r};
    for (int i = 0; i < n; ++i) {
        if (i % SUB == 0) {
            s.log_cut += (s.log_cut_t - s.log_cut) * GLIDE;
            s.res += (s.res_t - s.res) * GLIDE;
            s.drive += (s.drive_t - s.drive) * GLIDE;
            if (fabsf(s.drive) < 1e-4f && s.drive_t == 0.0f) s.drive = 0.0f;
            coefficients(s);
        }
        if (s.single) {
            io[0][i] = core(s, io[0][i], 0);
            io[1][i] = core(s, io[1][i], 1);
            continue;
        }
        for (int c = 0; c < 2; ++c) {
            float x[2];
            s.os.up(io[c][i], c, x);
            x[0] = core(s, x[0], c);
            x[1] = core(s, x[1], c);
            io[c][i] = s.os.down(x, c);
        }
    }
    for (int c = 0; c < 2; ++c) {
        flush(s.ic1[c]);
        flush(s.ic2[c]);
        flush(s.ic3[c]);
        flush(s.ic4[c]);
    }
    s.os.flush_all();
}

void format(int p, int v, char *out, int max) {
    if (p == 0) fmt_hz(out, max, expo(v, 20.0f, 20000.0f));
    else if (p == 3) fmt_text(out, max, MODES[choice(v, 8)]);
    else fmt_pct(out, max, v);
}

} // namespace

void filter_oversample(void *st, int on) { ((State *)st)->single = !on; }

const Effect FILTER = {
    "Filter", {"Cutoff", "Reso", "Drive", "Mode"}, {593, 200, 0, 0}, {0, 0, 0, 8}, FILTER_INSTANCES, state, reset, apply,
    process, format,
};

} // namespace bwfx
