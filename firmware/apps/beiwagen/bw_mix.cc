/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl */
/* The mixdown engine (bw_mix.h). The main loop writes the knobs; the interrupt reads them all at each block's start,
 * so it never needs more than a block to follow, and hands an effect its four only when one of them changed.
 * Per block: each track to float, through its insert (the filter, faded in and out), its gains ramped across the block
 * into the mix and the two send buses; the delay and the reverb on their buses, wet only, back into the mix at their
 * return levels; the master, to ints. A track that is silent and stays so (muted, the return, level off) costs
 * nothing, nor does an insert that is off. */
#include "bw_mix.h"

#include <string.h>

#include "bw_probe.h"
#include "fx_dsp.h"

using namespace bwfx;

namespace {

const int T = BW_MIX_TRACKS;
const int TP = BW_MIX_TRACK_PARAMS;
const int MODES = 9; /* Off and the filter's eight */
const float FADE = 1.0f / (0.01f * FW_FS_HZ); /* 10 ms */
const float IN = 1.0f / 32768.0f;
const float SQRT2 = 1.41421356f;

const char *const TRACK_NAME[TP] = {"Level", "Pan", "Delay", "Reverb", "Cutoff", "Reso", "Drive", "Mode", "Mute",
                                    "Return"};
const int TRACK_DEF[TP] = {900, 500, 0, 0, 1000, 0, 0, 0, 0, 0};
const int TRACK_STEPS[TP] = {0, 0, 0, 0, 0, 0, 0, MODES, 2, 2};
const char *const SHARED_NAME[BW_MIX_PARAMS - T * TP] = {"Time", "Feedback", "Tone", "Return", "Size", "Decay",
                                                         "Damping", "Return", "Master", "Inserts"};
const int SHARED_DEF[BW_MIX_PARAMS - T * TP] = {926, 421, 674, 1000, 500, 436, 400, 1000, 900, 0};
const int SHARED_STEPS[BW_MIX_PARAMS - T * TP] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 2};

volatile int value[BW_MIX_PARAMS]; /* the main loop's (the return flags also the detection's) */

struct Track {
    float gl, gr, sd, sr;   /* the gains where the last block left them */
    int applied[4];         /* the filter's knobs as last handed to it */
    int running;            /* the filter reset since it last went quiet */
    int restart;            /* the rate changed: fade out, reset at the new rate, fade back in */
    float ins;              /* the insert's fade, 0 dry .. 1 */
    void *filter;
    volatile int peak;
};
Track track[T];

struct Shared {
    const Effect *e;
    int applied[4];
    int running;
    float ret;              /* the return gain where the last block left it */
};
Shared delay_fx, reverb_fx;

float master, fade_out = 1.0f; /* the master gain; the detection's fade of the whole mix */
int single;                    /* the inserts' rate as last applied (BW_MIX_INSERTS) */

float TL[FW_BLOCK], TR[FW_BLOCK]; /* a track */
float SL[FW_BLOCK], SR[FW_BLOCK]; /* before its insert, for the fade */
float ML[FW_BLOCK], MR[FW_BLOCK]; /* the mix */
float DL[FW_BLOCK], DR[FW_BLOCK]; /* the delay's bus */
float RL[FW_BLOCK], RR[FW_BLOCK]; /* the reverb's bus */

/* ---------------------------------------------------------------------------------------------- the knobs --- */
int valid(int p) { return p >= 0 && p < BW_MIX_PARAMS; }
int clampi(int v) { return v < 0 ? 0 : v > BW_MIX_MAX ? BW_MIX_MAX : v; }

/* Off, then -54 .. +6 dB, 0.06 dB a step (900: 0 dB) */
float level_gain(int v) { return v <= 0 ? 0.0f : exp2f((float)(v - 900) * (0.06f * 0.16609640f)); }
/* the filter's mode knob for a mixer mode 1..8 (0 is Off): the middle of its position */
int filter_mode(int m) { return ((m - 1) * 1001 + 500) / 8; }

/* ----------------------------------------------------------------------------------------- the detection --- */
using bwprobe::PROBE;
const int REC = 6144;                       /* frames recorded from the probe's start */
const int LAG_MIN = 16;
const int LAG_HI = REC - PROBE - 1;         /* lags searched: LAG_MIN .. LAG_HI-1 (93 ms) */
const int SLICE = 64;                       /* correlations a block */
const float PROBE_MIN = 1036.0f;            /* -30 dBFS, */
const float PROBE_MAX = 4125.0f;            /* up to -18 dBFS under loud input */
const float DETECT = 30.0f;                 /* a pair's peak^2 against its mean correlation^2 (bw_aec's) */
const float STANDS_OUT = 8.0f;              /* ... and that ratio this many times every other pair's */

enum { D_IDLE, D_FADE, D_ARM, D_REC, D_SEARCH };
volatile int want_gen, d_state = BW_MIX_DETECT_IDLE, d_track = -1, d_delay = -1;
int seen_gen, phase = D_IDLE;
int16_t *rec;                               /* the delay's line meanwhile: REC frames per pair, mono */
float amp;
uint32_t now, t0;
int s_pair, s_lag;
int32_t best_r[T];
int best_lag[T];
float r2_sum[T];
int r2_n[T];
float level_in;                             /* the input's loudest pair this block, power */

int32_t corr(const int16_t *y, int lag) {
    int32_t acc = 0;
    y += lag - 1;
    for (int i = 0; i < PROBE + 2; i++) acc += bwprobe::d2[i] * (int32_t)y[i];
    return acc;
}

void finish(int found, int lag) {
    if (found >= 0) {
        for (int t = 0; t < T; t++) value[BW_MIX_P(t, BW_MIX_RETURN)] = t == found ? BW_MIX_MAX : 0;
        d_track = found;
        d_delay = lag;
        d_state = BW_MIX_DETECT_FOUND;
    } else {
        d_state = BW_MIX_DETECT_FAILED;
    }
    phase = D_IDLE;
    delay_fx.running = reverb_fx.running = 0; /* back from silence: the delay's line was the recording */
}

/* a slice of the correlations; at the end, the pair whose peak stands out */
void search() {
    for (int k = 0; k < SLICE && s_pair < T; k++) {
        const int32_t r = corr(rec + s_pair * REC, s_lag);
        const float f = (float)r;
        r2_sum[s_pair] += f * f;
        r2_n[s_pair]++;
        if ((r < 0 ? -r : r) > (best_r[s_pair] < 0 ? -best_r[s_pair] : best_r[s_pair])) {
            best_r[s_pair] = r;
            best_lag[s_pair] = s_lag;
        }
        if (++s_lag == LAG_HI) {
            s_lag = LAG_MIN;
            s_pair++;
        }
    }
    if (s_pair < T) return;
    int found = -1;
    float top = 0.0f, second = 0.0f;
    for (int t = 0; t < T; t++) {
        const float peak = (float)best_r[t];
        const float noise = (r2_sum[t] - peak * peak) / (float)(r2_n[t] - 1);
        const float ratio = noise > 0.0f ? peak * peak / noise : 0.0f;
        if (ratio > top) {
            second = top;
            top = ratio;
            found = t;
        } else if (ratio > second) {
            second = ratio;
        }
    }
    const int ok = found >= 0 && top >= DETECT && top >= STANDS_OUT * second;
    finish(ok ? found : -1, ok ? best_lag[found] : -1);
}

/* the input side of a block: start, record, search */
void detect_input(const int16_t *pairs, int npairs, int stride, int n) {
    if (want_gen != seen_gen) {
        seen_gen = want_gen;
        int count;
        rec = delay_scratch(&count);
        if (count < T * REC) {
            d_state = BW_MIX_DETECT_FAILED;
        } else {
            phase = D_FADE;
            d_state = BW_MIX_DETECT_RUNNING;
            d_track = d_delay = -1;
        }
    }
    if (phase == D_FADE || phase == D_ARM) {
        level_in = 0.0f;
        for (int p = 0; p < npairs && pairs; p++) {
            float e = 0.0f;
            for (int i = 0; i < 2 * n; i++) e += (float)pairs[p * stride + i] * (float)pairs[p * stride + i];
            e /= (float)(2 * n);
            level_in = e > level_in ? e : level_in;
        }
    } else if (phase == D_REC) {
        for (int p = 0; p < T; p++) {
            int16_t *y = rec + p * REC;
            for (int i = 0; i < n; i++) {
                const uint32_t j = now + (uint32_t)i - t0;
                if (j >= (uint32_t)REC) break;
                y[j] = p < npairs && pairs
                           ? (int16_t)(((int32_t)pairs[p * stride + 2 * i] + pairs[p * stride + 2 * i + 1]) >> 1)
                           : 0;
            }
        }
        if (now + (uint32_t)n - t0 >= (uint32_t)REC) {
            for (int t = 0; t < T; t++) {
                best_r[t] = 0;
                best_lag[t] = LAG_MIN;
                r2_sum[t] = 0.0f;
                r2_n[t] = 0;
            }
            s_pair = 0;
            s_lag = LAG_MIN;
            phase = D_SEARCH;
        }
    } else if (phase == D_SEARCH) {
        search();
    }
}

/* ------------------------------------------------------------------------------------------------ a block --- */
/* an effect's four knobs, handed on when one changed */
void hand(const Effect &e, void *st, int *applied, const int *v) {
    if (applied[0] == v[0] && applied[1] == v[1] && applied[2] == v[2] && applied[3] == v[3]) return;
    for (int k = 0; k < 4; k++) applied[k] = v[k];
    e.apply(st, v);
}

void insert(Track &tr, int t, int n) {
    const volatile int *v = value + BW_MIX_P(t, 0);
    const int mode = choice(v[BW_MIX_MODE], MODES);
    const int knobs[4] = {v[BW_MIX_CUTOFF], v[BW_MIX_RESO], v[BW_MIX_DRIVE],
                          mode ? filter_mode(mode) : tr.applied[3]};
    hand(FILTER, tr.filter, tr.applied, knobs);
    const int on = mode && !tr.restart;
    if (!on && tr.ins == 0.0f) {
        tr.running = 0;
        if (tr.restart) {
            filter_oversample(tr.filter, !single);
            tr.restart = 0;
        }
        return;
    }
    if (!tr.running) {
        FILTER.reset(tr.filter);
        tr.running = 1;
    }
    if (on && tr.ins == 1.0f) {
        FILTER.process(tr.filter, TL, TR, n);
        return;
    }
    memcpy(SL, TL, sizeof(float) * (size_t)n);
    memcpy(SR, TR, sizeof(float) * (size_t)n);
    FILTER.process(tr.filter, TL, TR, n);
    float g = tr.ins;
    const float step = on ? FADE : -FADE;
    for (int i = 0; i < n; i++) {
        g = clampf(g + step, 0.0f, 1.0f);
        TL[i] = SL[i] + (TL[i] - SL[i]) * g;
        TR[i] = SR[i] + (TR[i] - SR[i]) * g;
    }
    tr.ins = g;
}

void track_block(int t, const int16_t *pair, int n, int sends) {
    Track &tr = track[t];
    const volatile int *v = value + BW_MIX_P(t, 0);
    const int in = !v[BW_MIX_MUTE] && !v[BW_MIX_RETURN];
    const float g = in ? level_gain(v[BW_MIX_LEVEL]) : 0.0f;
    const float pan = unit(v[BW_MIX_PAN]) * (0.5f * PI);
    const float gl = g * SQRT2 * cosf(pan), gr = g * SQRT2 * sinf(pan);
    const float sd = sends ? g * unit(v[BW_MIX_SEND_DELAY]) : 0.0f;
    const float sr = sends ? g * unit(v[BW_MIX_SEND_REVERB]) : 0.0f;
    int peak = 0;
    if (pair)
        for (int i = 0; i < 2 * n; i++) {
            const int a = pair[i] < 0 ? -pair[i] : pair[i];
            peak = a > peak ? a : peak;
        }
    tr.peak = peak;
    if (g == 0.0f && tr.gl == 0.0f && tr.gr == 0.0f && tr.sd == 0.0f && tr.sr == 0.0f) { /* silent, stays so */
        tr.running = 0;
        tr.ins = 0.0f;
        if (tr.restart) {
            filter_oversample(tr.filter, !single);
            tr.restart = 0;
        }
        return;
    }
    for (int i = 0; i < n; i++) {
        TL[i] = pair ? (float)pair[2 * i] * IN : 0.0f;
        TR[i] = pair ? (float)pair[2 * i + 1] * IN : 0.0f;
    }
    insert(tr, t, n);
    const float k = 1.0f / (float)n;
    const float dgl = (gl - tr.gl) * k, dgr = (gr - tr.gr) * k, dsd = (sd - tr.sd) * k, dsr = (sr - tr.sr) * k;
    float a = tr.gl, b = tr.gr, c = tr.sd, d = tr.sr;
    for (int i = 0; i < n; i++) {
        a += dgl;
        b += dgr;
        c += dsd;
        d += dsr;
        ML[i] += TL[i] * a;
        MR[i] += TR[i] * b;
        DL[i] += TL[i] * c;
        DR[i] += TR[i] * c;
        RL[i] += TL[i] * d;
        RR[i] += TR[i] * d;
    }
    tr.gl = gl;
    tr.gr = gr;
    tr.sd = sd;
    tr.sr = sr;
}

/* a shared effect on its bus, wet only, into the mix at its return level; stopped while the return is off */
void shared_block(Shared &s, int first, float *bl, float *br, int n, int on) {
    const float ret = on ? unit(value[first + 3]) : 0.0f;
    const int knobs[4] = {value[first], value[first + 1], value[first + 2], BW_MIX_MAX};
    void *st = s.e->state(0);
    hand(*s.e, st, s.applied, knobs);
    if (ret == 0.0f && s.ret == 0.0f) {
        s.running = 0;
        return;
    }
    if (!s.running) {
        s.e->reset(st);
        s.running = 1;
    }
    s.e->process(st, bl, br, n);
    const float dr = (ret - s.ret) / (float)n;
    float g = s.ret;
    for (int i = 0; i < n; i++) {
        g += dr;
        ML[i] += bl[i] * g;
        MR[i] += br[i] * g;
    }
    s.ret = ret;
}

void block(const int16_t *pairs, int npairs, int stride, int32_t *lr, int n) {
    if (shared_owner != OWNER_MIX) { /* the chain had the lines: from silence, with the mixer's knobs */
        delay_fx.running = reverb_fx.running = 0;
        delay_fx.applied[0] = reverb_fx.applied[0] = -1;
        shared_owner = OWNER_MIX;
    }
    const int s = value[BW_MIX_INSERTS] >= BW_MIX_MAX / 2;
    if (s != single) {
        single = s;
        for (int t = 0; t < T; t++) track[t].restart = 1;
    }
    detect_input(pairs, npairs, stride, n);
    const int sends = phase == D_IDLE || phase == D_FADE;
    memset(ML, 0, sizeof(float) * (size_t)n);
    memset(MR, 0, sizeof(float) * (size_t)n);
    memset(DL, 0, sizeof(float) * (size_t)n);
    memset(DR, 0, sizeof(float) * (size_t)n);
    memset(RL, 0, sizeof(float) * (size_t)n);
    memset(RR, 0, sizeof(float) * (size_t)n);
    for (int t = 0; t < T; t++) track_block(t, t < npairs && pairs ? pairs + t * stride : 0, n, sends);
    if (!sends) { /* the detection has the delay's line: both stop at once (the mix is faded out) */
        delay_fx.running = reverb_fx.running = 0;
        delay_fx.ret = reverb_fx.ret = 0.0f;
    }
    shared_block(delay_fx, BW_MIX_DELAY_TIME, DL, DR, n, sends);
    shared_block(reverb_fx, BW_MIX_REVERB_SIZE, RL, RR, n, sends);

    const float m1 = level_gain(value[BW_MIX_MASTER]), dm = (m1 - master) / (float)n;
    const float out_t = phase == D_FADE || phase == D_ARM || phase == D_REC ? 0.0f : 1.0f;
    const float df = out_t > fade_out ? FADE : out_t < fade_out ? -FADE : 0.0f;
    float m = master, f = fade_out;
    for (int i = 0; i < n; i++) {
        m += dm;
        f = clampf(f + df, 0.0f, 1.0f);
        const float g = m * f * 32768.0f;
        lr[2 * i] = (int32_t)clampf(ML[i] * g, -32767.0f, 32767.0f);
        lr[2 * i + 1] = (int32_t)clampf(MR[i] * g, -32767.0f, 32767.0f);
    }
    master = m1;
    fade_out = f;
    if (phase == D_FADE && f == 0.0f) { /* faded out: the probe's level, 10 dB under the loudest pair */
        const float a2 = level_in * 0.1f;
        amp = a2 < PROBE_MIN * PROBE_MIN ? PROBE_MIN : a2 > PROBE_MAX * PROBE_MAX ? PROBE_MAX : sqrtf(a2);
        phase = D_ARM;
    }
}

} // namespace

/* ------------------------------------------------------------------------------------------------ the API --- */
extern "C" {

void bw_mix_init(void) {
    bwprobe::init();
    for (int p = 0; p < BW_MIX_PARAMS; p++) value[p] = bw_mix_default(p);
    for (int t = 0; t < T; t++) {
        Track &tr = track[t];
        memset(&tr, 0, sizeof tr);
        tr.filter = FILTER.state(1 + t);
        filter_oversample(tr.filter, 1);
        for (int k = 0; k < 4; k++) tr.applied[k] = -1;
        const int knobs[4] = {TRACK_DEF[BW_MIX_CUTOFF], TRACK_DEF[BW_MIX_RESO], TRACK_DEF[BW_MIX_DRIVE], 0};
        hand(FILTER, tr.filter, tr.applied, knobs);
        FILTER.reset(tr.filter);
    }
    delay_fx.e = &DELAY;
    reverb_fx.e = &REVERB;
    delay_fx.applied[0] = reverb_fx.applied[0] = -1;
    delay_fx.running = reverb_fx.running = 0;
    delay_fx.ret = reverb_fx.ret = 0.0f;
    master = level_gain(value[BW_MIX_MASTER]);
    fade_out = 1.0f;
    single = 0;
    phase = D_IDLE;
    want_gen = seen_gen = 0;
    d_state = BW_MIX_DETECT_IDLE;
    d_track = d_delay = -1;
}

void bw_mix_process(const int16_t *pairs, int npairs, int32_t *lr, uint32_t frames) {
    const int stride = 2 * (int)frames;
    while (frames) {
        const int n = frames < FW_BLOCK ? (int)frames : (int)FW_BLOCK;
        block(pairs, npairs, stride, lr, n);
        if (pairs) pairs += 2 * n;
        lr += 2 * n;
        frames -= (uint32_t)n;
    }
}

void bw_mix_output(int32_t *lr, uint32_t frames) {
    if (phase == D_ARM) { /* the probe and its recording from the next block on, together */
        t0 = now + frames;
        phase = D_REC;
    } else if (phase == D_REC) {
        for (uint32_t i = 0; i < frames; i++) {
            const uint32_t j = now + i - t0;
            if (j >= (uint32_t)PROBE) break;
            const int32_t p = bwprobe::bit((int)j) ? (int32_t)amp : -(int32_t)amp;
            for (int c = 0; c < 2; c++) {
                const int32_t y = lr[2 * i + c] + p;
                lr[2 * i + c] = y > 32767 ? 32767 : y < -32767 ? -32767 : y;
            }
        }
    }
    now += frames;
}

const char *bw_mix_param_name(int p) {
    if (!valid(p)) return "";
    return p < T * TP ? TRACK_NAME[p % TP] : SHARED_NAME[p - T * TP];
}

void bw_mix_set(int p, int v) {
    if (valid(p)) value[p] = clampi(v);
}
int bw_mix_get(int p) { return valid(p) ? value[p] : 0; }
int bw_mix_default(int p) {
    if (!valid(p)) return 0;
    return p < T * TP ? TRACK_DEF[p % TP] : SHARED_DEF[p - T * TP];
}
int bw_mix_steps(int p) {
    if (!valid(p)) return 0;
    return p < T * TP ? TRACK_STEPS[p % TP] : SHARED_STEPS[p - T * TP];
}

void bw_mix_format(int p, char *out, int max) {
    if (!out || max <= 0) return;
    *out = 0;
    if (!valid(p)) return;
    const int v = value[p];
    char b[16];
    int k = 0;
    const int level = p < T * TP ? p % TP == BW_MIX_LEVEL : p == BW_MIX_MASTER;
    if (level || (p < T * TP && p % TP == BW_MIX_PAN)) {
        int x;
        const char *unit;
        if (level) {
            if (v <= 0) {
                fmt_text(out, max, "Off");
                return;
            }
            x = (v - 900) * 6 / 10; /* tenths of a dB */
            unit = " dB";
        } else {
            if (v == 500) {
                fmt_text(out, max, "C");
                return;
            }
            x = (v < 500 ? 500 - v : v - 500) / 5;
            unit = v < 500 ? " L" : " R";
        }
        if (x < 0) {
            b[k++] = '-';
            x = -x;
        }
        char d[8];
        int nd = 0;
        int whole = level ? x / 10 : x;
        do d[nd++] = (char)('0' + whole % 10); while ((whole /= 10) && nd < 7);
        while (nd) b[k++] = d[--nd];
        if (level) {
            b[k++] = '.';
            b[k++] = (char)('0' + x % 10);
        }
        for (; *unit; unit++) b[k++] = *unit;
        b[k] = 0;
        fmt_text(out, max, b);
        return;
    }
    if (p < T * TP) {
        switch (p % TP) {
        case BW_MIX_CUTOFF: FILTER.format(0, v, out, max); return;
        case BW_MIX_RESO: FILTER.format(1, v, out, max); return;
        case BW_MIX_DRIVE: FILTER.format(2, v, out, max); return;
        case BW_MIX_MODE: {
            const int m = choice(v, MODES);
            if (m) FILTER.format(3, filter_mode(m), out, max);
            else fmt_text(out, max, "Off");
            return;
        }
        case BW_MIX_MUTE: fmt_text(out, max, v >= 500 ? "On" : "Off"); return;
        case BW_MIX_RETURN: fmt_text(out, max, v >= 500 ? "Yes" : "No"); return;
        default: fmt_pct(out, max, v); return;
        }
    }
    if (p == BW_MIX_INSERTS) fmt_text(out, max, v >= 500 ? "1x" : "2x");
    else if (p < BW_MIX_DELAY_RETURN) DELAY.format(p - BW_MIX_DELAY_TIME, v, out, max);
    else if (p > BW_MIX_DELAY_RETURN && p < BW_MIX_REVERB_RETURN) REVERB.format(p - BW_MIX_REVERB_SIZE, v, out, max);
    else fmt_pct(out, max, v);
}

int bw_mix_peak(int t) { return t >= 0 && t < T ? track[t].peak : 0; }

int bw_mix_save(void *dst, int max) {
    if (!dst || max < BW_MIX_BLOB) return 0;
    uint8_t *b = (uint8_t *)dst;
    b[0] = 'B';
    b[1] = 'M';
    b[2] = 1; /* the format */
    b[3] = BW_MIX_PARAMS;
    for (int p = 0; p < BW_MIX_PARAMS; p++) {
        b[4 + 2 * p] = (uint8_t)(value[p] & 0xFF);
        b[5 + 2 * p] = (uint8_t)(value[p] >> 8);
    }
    return BW_MIX_BLOB;
}

int bw_mix_load(const void *src, int len) {
    const uint8_t *b = (const uint8_t *)src;
    if (!b || len < 4 || b[0] != 'B' || b[1] != 'M' || b[2] != 1 || len < 4 + 2 * b[3]) return 0;
    for (int p = 0; p < BW_MIX_PARAMS; p++) {
        if (p < T * TP && p % TP == BW_MIX_RETURN) continue;
        value[p] = p < b[3] ? clampi(b[4 + 2 * p] | b[5 + 2 * p] << 8) : bw_mix_default(p);
    }
    return 1;
}

void bw_mix_detect(void) {
    d_state = BW_MIX_DETECT_RUNNING;
    want_gen = want_gen + 1;
}
int bw_mix_detect_state(void) { return d_state; }
int bw_mix_detect_track(void) { return d_track; }
int bw_mix_detect_delay(void) { return d_delay; }

} // extern "C"
