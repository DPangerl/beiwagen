/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl
 * The loop canceller (bw_aec.h): measures the OP-1's return path with a noise burst, then subtracts it */
/* Calibration: a binary noise probe (PROBE samples, a fixed seed; -30 dBFS, louder under loud input, at most -18)
 * goes out on both channels while the input is muted; the returned y is recorded and correlated with the probe's
 * second difference (that whitens music playing meanwhile, the probe stays sharp) over lags LAG_MIN..LAG_HI-1, a slice
 * of lags per block. A peak well above the correlation's noise gives the delay. Each channel's response h (TAPS taps,
 * PRE before the peak) is solved from the plain correlation and the probe's own autocorrelation (a Toeplitz system)
 * and shrunk where it is noise; while playing drowns the probe, more bursts are averaged (up to BURSTS).
 * Cancelling: e = y - h*x, h fixed. It does not adapt: the live playing s reaches x through the effects, so s is
 * correlated with x one loop earlier (a held note is like itself D samples ago), and every adaptive variant tried on
 * host/aec_test.c's loop (NLMS, prediction-error NLMS, gain and delay tracking, gated on echo-only blocks) learned to
 * cancel s too and ended worse than the fixed h. A changed loop (OP-1 level, delay) needs a new calibration. */
#include "bw_aec.h"

#include "bw_probe.h"
#include "fw.h"

namespace {

using bwprobe::PROBE;

const int TAPS = 32;                        /* the response per channel */
const int PRE = 10;                         /* its taps before the peak (the resamplers' smear) */
const int RING = 8192;                      /* reference history, frames; a power of 2 */
const int REC = 6144;                       /* the recording after a burst's start, frames */
const int LAG_MIN = (int)FW_BLOCK + PRE;    /* the response must lie wholly before the block */
const int LAG_HI = REC - PROBE - 1;         /* lags searched: LAG_MIN .. LAG_HI-1 (93 ms) */
const int SLICE = 32;                       /* lags correlated per block */
const float PROBE_MIN = 1036.0f;            /* the probe's level: -30 dBFS, */
const float PROBE_MAX = 4125.0f;            /* up to -18 dBFS under loud input (masked by it) */
const float DETECT = 30.0f;                 /* peak^2 against the mean correlation^2: about 5.5 sigma */
const int BURSTS = 4;                       /* probe bursts at most */
const float CAL_GOOD = 1000.0f;             /* the response 30 dB over its noise: no more bursts */
const float Q = 1.0f / 32768.0f;

enum { IDLE, ARM, PROBING, SEARCH, GATHER, WARM, CANCEL };

union Mem {
    int16_t ring[RING][2];   /* x, the reference (WARM, CANCEL) */
    int16_t rec[REC][2];     /* y after a burst's start (PROBING .. GATHER) */
};
Mem mem FW_POOL;

float probe_acf[TAPS];       /* the probe's autocorrelation */

volatile int want, gen;      /* the main loop's request */
volatile int st, delay_out, erle_out;

struct Channel {
    float w[TAPS];           /* reversed: w[j] weighs x at lag base + TAPS-1 - j */
    float acc[TAPS];         /* the correlation over the window, summed over the bursts, each times its level */
    float noise;             /* the correlation's noise power, summed likewise (times the level squared) */
};
Channel ch[2];
float xs[FW_BLOCK + TAPS];  /* a channel's reference for one block (off the interrupt's stack) */

int phase, seen_gen;
int bursts;                  /* bursts averaged */
float amp, amp2;             /* this burst's level; the levels squared, summed */
int part;                    /* GATHER's step */
uint32_t now;                /* the current block's first sample */
uint32_t t0;                 /* the burst's first sample */
uint32_t filled;             /* reference frames recorded since WARM */
int base;                    /* the response's first lag */
int lag, best_lag;
int32_t best_r;
float r2_sum;
int r2_n;
float in_gain;               /* 0 muted .. 1 */

inline int16_t sat16(float v) {
    return (int16_t)(v > 32767.0f ? 32767 : v < -32767.0f ? -32767 : (int)(v + (v < 0.0f ? -0.5f : 0.5f)));
}
inline int16_t clip16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v); }
inline float absf(float v) { return v < 0.0f ? -v : v; }
inline int probe_bit(int i) { return bwprobe::bit(i); }
inline int probe_sign(int i) { return bwprobe::sign(i); }

/* a square root by Newton from the float's bits, without libm */
float sqrt_approx(float x) {
    union {
        float f;
        uint32_t u;
    } v;
    v.f = x;
    v.u = (v.u >> 1) + 0x1FC00000u;
    float r = v.f;
    for (int i = 0; i < 3; i++) r = 0.5f * (r + x / r);
    return r;
}

/* ln from the float's bits: within 1e-4, without libm */
float ln_approx(float x) {
    union {
        float f;
        uint32_t u;
    } v;
    v.f = x;
    const float e = (float)((int)((v.u >> 23) & 255u) - 127);
    v.u = (v.u & 0x7FFFFFu) | 0x3F800000u;
    const float m = v.f;
    return e * 0.69314718f + (-1.7417939f + (2.8212026f + (-1.4699568f + (0.44717955f - 0.056570851f * m) * m) * m) * m);
}

/* the recording's mono sum correlated with the probe's second difference at a lag */
int32_t corr_d2(int at) {
    int32_t acc = 0;
    const int16_t(*y)[2] = mem.rec + at - 1;
    for (int i = 0; i < PROBE + 2; i++) acc += bwprobe::d2[i] * ((int32_t)y[i][0] + y[i][1]);
    return acc;
}

/* one channel of the recording correlated with the probe itself */
float corr_plain(int at, int c) {
    int32_t acc = 0;
    const int16_t(*y)[2] = mem.rec + at;
    for (int w = 0; w < PROBE / 32; w++) {
        const uint32_t bits = bwprobe::bits[w];
        for (int b = 0; b < 32; b++, y++) acc += (bits >> b & 1u) ? y[0][c] : -y[0][c];
    }
    return (float)acc;
}

/* sum_m r[|k-m|] x[m] = c[k] for a symmetric Toeplitz r (Levinson); 0 if it breaks down */
int toeplitz_solve(const float *r, const float *c, float *x, int n) {
    float f[TAPS], b[TAPS], nf[TAPS];
    if (r[0] <= 0.0f) return 0;
    f[0] = b[0] = 1.0f / r[0];
    x[0] = c[0] / r[0];
    for (int k = 1; k < n; k++) {
        float ef = 0.0f, ex = 0.0f;
        for (int i = 0; i < k; i++) {
            ef += r[k - i] * f[i];
            ex += r[k - i] * x[i];
        }
        const float d = 1.0f - ef * ef;
        if (d <= 1e-6f) return 0;
        for (int i = 0; i <= k; i++) nf[i] = ((i < k ? f[i] : 0.0f) - ef * (i > 0 ? b[i - 1] : 0.0f)) / d;
        for (int i = 0; i <= k; i++) {
            f[i] = nf[i];
            b[i] = nf[k - i];
        }
        x[k] = 0.0f;
        const float a = c[k] - ex;
        for (int i = 0; i <= k; i++) x[i] += a * b[i];
    }
    return 1;
}

void fail() {
    phase = IDLE;
    st = BW_AEC_FAILED;
    delay_out = -1;
}

/* the delay: the largest correlation, a slice of lags per block, then checked against the correlation's noise */
void search() {
    const int end = lag + SLICE < LAG_HI ? lag + SLICE : LAG_HI;
    for (; lag < end; lag++) {
        const int32_t r = corr_d2(lag);
        const float f = (float)r;
        r2_sum += f * f;
        r2_n++;
        if ((r < 0 ? -r : r) > (best_r < 0 ? -best_r : best_r)) {
            best_r = r;
            best_lag = lag;
        }
    }
    if (lag < LAG_HI) return;
    const float peak = (float)best_r;
    const float noise = (r2_sum - peak * peak) / (float)(r2_n - 1);
    if (peak * peak < DETECT * noise || best_lag <= LAG_MIN + 1 || best_lag >= LAG_HI - 2) {
        fail();
        return;
    }
    /* the second difference rings around a smeared peak: the delay is the response's own largest tap */
    int at = best_lag;
    float top = -1.0f;
    for (int l = best_lag - 3; l <= best_lag + 3; l++) {
        const float m = absf(corr_plain(l, 0)) + absf(corr_plain(l, 1));
        if (m > top) {
            top = m;
            at = l;
        }
    }
    best_lag = at;
    base = best_lag - PRE;
    for (int c = 0; c < 2; c++) {
        ch[c].noise = 0.0f;
        for (int k = 0; k < TAPS; k++) ch[c].acc[k] = 0.0f;
    }
    phase = GATHER;
}

void solve();

/* a burst's correlations, one part per block (each as much work as a search slice): the noise of each channel where
   there is no echo (past the response, or before it), then the window of each channel */
void gather() {
    const int c = part & 1;
    Channel &h = ch[c];
    if (part < 2) {
        const int at = base + TAPS + 64 + 32 <= REC - PROBE ? base + TAPS + 64 : base - 96;
        float n2 = 0.0f;
        for (int k = 0; k < 32; k++) {
            const float r = corr_plain(at + k, c);
            n2 += r * r;
        }
        h.noise += n2 * (1.0f / 32.0f) * amp * amp;
    } else {
        for (int k = 0; k < TAPS; k++) h.acc[k] += corr_plain(base + k, c) * amp;
    }
    if (++part == 4) solve();
}

/* the bursts so far averaged: the response, shrunk by its noise per tap; another burst while that noise is high */
void solve() {
    bursts++;
    amp2 += amp * amp;
    const float kb = 1.0f / amp2;
    float sig = 0.0f, err = 0.0f;
    for (int c = 0; c < 2; c++) {
        Channel &h = ch[c];
        float r[TAPS], x[TAPS];
        for (int k = 0; k < TAPS; k++) r[k] = h.acc[k] * kb;
        if (!toeplitz_solve(probe_acf, r, x, TAPS)) {
            fail();
            return;
        }
        const float n2 = h.noise * kb * kb / (probe_acf[0] * probe_acf[0]);
        for (int k = 0; k < TAPS; k++) {
            const float t2 = x[k] * x[k];
            sig += t2;
            err += t2 * n2 / (t2 + n2 + 1e-12f);
            h.w[TAPS - 1 - k] = x[k] * t2 / (t2 + n2 + 1e-12f);
        }
    }
    if (sig < CAL_GOOD * err && bursts < BURSTS) {
        phase = ARM;
        return;
    }
    const int db = (int)(4.3429448f * ln_approx(sig / (err + 1e-20f)) + 0.5f);
    erle_out = db < 1 ? 1 : db > 60 ? 60 : db;
    delay_out = best_lag;
    filled = 0;
    phase = WARM;
}

/* one block of cancelling, n <= FW_BLOCK */
void cancel(int32_t *lr, int n) {
    const uint32_t first = now - (uint32_t)base - (uint32_t)(TAPS - 1);
    for (int c = 0; c < 2; c++) {
        const float *w = ch[c].w;
        for (int m = 0; m < n + TAPS - 1; m++) xs[m] = (float)mem.ring[(first + (uint32_t)m) & (RING - 1)][c] * Q;
        for (int i = 0; i < n; i++) {
            const float *u = xs + i;
            float yhat = 0.0f;
            for (int j = 0; j < TAPS; j++) yhat += w[j] * u[j];
            const float y = (float)lr[2 * i + c] * Q, e = y - yhat;
            lr[2 * i + c] = sat16(e * 32768.0f * in_gain);
        }
    }
}

void ramp(int32_t *lr, int n, float target) {
    const float step = 1.0f / (float)FW_BLOCK;
    for (int i = 0; i < n; i++) {
        in_gain += in_gain < target ? step : in_gain > target ? -step : 0.0f;
        in_gain = in_gain < 0.0f ? 0.0f : in_gain > 1.0f ? 1.0f : in_gain;
        lr[2 * i] = (int32_t)((float)lr[2 * i] * in_gain);
        lr[2 * i + 1] = (int32_t)((float)lr[2 * i + 1] * in_gain);
    }
}

} // namespace

extern "C" {

void bw_aec_init(void) {
    bwprobe::init();
    for (int k = 0; k < TAPS; k++) {
        int acc = 0;
        for (int i = 0; i + k < PROBE; i++) acc += probe_sign(i) * probe_sign(i + k);
        probe_acf[k] = (float)acc;
    }
    phase = IDLE;
    st = BW_AEC_OFF;
    delay_out = -1;
    erle_out = 0;
    in_gain = 1.0f;
    want = 0;
    gen = seen_gen = 0;
}

void bw_aec_enable(int on) {
    if (on) gen = gen + 1;
    want = on;
}

int bw_aec_state(void) { return st; }
int bw_aec_delay(void) { return delay_out; }
int bw_aec_erle_db(void) { return erle_out; }

void bw_aec_input(int32_t *lr, uint32_t frames) {
    const int n = (int)(frames > FW_BLOCK ? FW_BLOCK : frames);
    if (!want) {
        phase = IDLE;
        st = BW_AEC_OFF;
        delay_out = -1;
        erle_out = 0;
    } else if (gen != seen_gen) {
        seen_gen = gen;
        phase = ARM;
        st = BW_AEC_CALIBRATING;
        delay_out = -1;
        erle_out = 0;
        bursts = 0;
        amp2 = 0.0f;
        lag = LAG_MIN;
        best_lag = 0;
        best_r = 0;
        r2_sum = 0.0f;
        r2_n = 0;
    }
    if (phase == ARM) {
        /* the burst's level: 10 dB under the input's, within PROBE_MIN..PROBE_MAX */
        float p = 0.0f;
        for (int i = 0; i < 2 * n; i++) p += (float)lr[i] * (float)lr[i];
        const float a2 = p / (float)(2 * n) * 0.1f;
        amp = a2 < PROBE_MIN * PROBE_MIN ? PROBE_MIN : a2 > PROBE_MAX * PROBE_MAX ? PROBE_MAX : sqrt_approx(a2);
    }
    switch (phase) {
    case IDLE:
        if (in_gain < 1.0f) ramp(lr, n, 1.0f);
        return;
    case CANCEL:
        in_gain = in_gain + 1.0f / 8.0f > 1.0f ? 1.0f : in_gain + 1.0f / 8.0f;
        cancel(lr, n);
        return;
    case PROBING:
        for (int i = 0; i < n; i++) {
            const uint32_t j = now + (uint32_t)i - t0;
            if (j < (uint32_t)REC) {
                mem.rec[j][0] = clip16(lr[2 * i]);
                mem.rec[j][1] = clip16(lr[2 * i + 1]);
            }
        }
        if (now + (uint32_t)n - t0 >= (uint32_t)REC) {
            phase = bursts ? GATHER : SEARCH;
            part = 0;
        }
        break;
    case SEARCH:
        search();
        break;
    case GATHER:
        gather();
        break;
    case WARM:
        if (filled >= (uint32_t)(base + TAPS + (int)FW_BLOCK)) {
            phase = CANCEL;
            st = BW_AEC_ON;
            in_gain = 0.0f;
            cancel(lr, n);
            return;
        }
        break;
    default:
        break;
    }
    ramp(lr, n, 0.0f);
}

void bw_aec_output(int32_t *lr, uint32_t frames) {
    const int n = (int)(frames > FW_BLOCK ? FW_BLOCK : frames);
    if (phase == ARM) {
        t0 = now;
        phase = PROBING;
    }
    if (phase == PROBING) {
        for (int i = 0; i < n; i++) {
            const uint32_t j = now + (uint32_t)i - t0;
            if (j >= (uint32_t)PROBE) break;
            const int32_t p = probe_bit((int)j) ? (int32_t)amp : -(int32_t)amp;
            lr[2 * i] = clip16(lr[2 * i] + p);
            lr[2 * i + 1] = clip16(lr[2 * i + 1] + p);
        }
    } else if (phase == WARM || phase == CANCEL) {
        for (int i = 0; i < n; i++) {
            int16_t *x = mem.ring[(now + (uint32_t)i) & (RING - 1)];
            x[0] = clip16(lr[2 * i]);
            x[1] = clip16(lr[2 * i + 1]);
        }
        filled += (uint32_t)n;
    }
    now += (uint32_t)n;
}

} // extern "C"
