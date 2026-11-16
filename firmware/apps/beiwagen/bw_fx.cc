/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl
 * Ported for Beiwagen from the Steckfeld engine (Engine/src/Oversampler.cpp), float only, static buffers */
/* The chain (bw_fx.h): the knobs, the order, the fades in and out, the conversion from and to the codec's ints.
 * The main loop writes a knob and bumps the effect's version; the interrupt sees the new version at a block's start
 * and applies all four, so it never meets half an update. */
#include "bw_fx.h"

#include "fx_dsp.h"

namespace bwfx {

void Oversampler2x::init() {
    const float w0 = 2.0f * PI * 0.45f * 0.5f; /* 0.45 of the base rate, at twice it */
    const float cosine = cosf(w0), sine = sinf(w0);
    for (int i = 0; i < 4; ++i) {
        const float q = 1.0f / (2.0f * sinf((float)(2 * i + 1) * PI / 16.0f));
        const float alpha = sine / (2.0f * q);
        const float a0 = 1.0f + alpha;
        Biquad s = {};
        s.b0 = (1.0f - cosine) * 0.5f / a0;
        s.b1 = (1.0f - cosine) / a0;
        s.b2 = s.b0;
        s.a1 = -2.0f * cosine / a0;
        s.a2 = (1.0f - alpha) / a0;
        up_[i] = s;
        down_[i] = s;
    }
}

void Oversampler2x::flush_all() {
    for (int i = 0; i < 4; ++i)
        for (int c = 0; c < 2; ++c) {
            flush(up_[i].z1[c]);
            flush(up_[i].z2[c]);
            flush(down_[i].z1[c]);
            flush(down_[i].z2[c]);
        }
}

/* ---------------------------------------------------------------------------------------------- text --- */
struct Text {
    char *p;
    int left;
    void put(char c) {
        if (left > 1) {
            *p++ = c;
            --left;
        }
    }
    void str(const char *s) {
        while (*s) put(*s++);
    }
    void num(float x, int decimals) {
        int scale = 1;
        for (int i = 0; i < decimals; ++i) scale *= 10;
        int n = (int)(x * (float)scale + (x < 0.0f ? -0.5f : 0.5f));
        if (n < 0) {
            put('-');
            n = -n;
        }
        char d[12];
        int k = 0;
        int whole = n / scale, frac = n % scale;
        do d[k++] = (char)('0' + whole % 10); while ((whole /= 10) && k < 11);
        while (k) put(d[--k]);
        if (decimals) {
            put('.');
            for (int s = scale / 10; s; s /= 10) put((char)('0' + frac / s % 10));
        }
    }
};

static Text text(char *out, int max) {
    Text t = {out, max};
    if (max > 0) *out = 0;
    return t;
}
static void end(Text &t) {
    if (t.left > 0) *t.p = 0;
}

void fmt_hz(char *out, int max, float hz) {
    Text t = text(out, max);
    if (hz >= 10000.0f) {
        t.num(hz * 0.001f, 0);
        t.str(" kHz");
    } else if (hz >= 1000.0f) {
        t.num(hz * 0.001f, 1);
        t.str(" kHz");
    } else {
        t.num(hz, hz < 10.0f ? 2 : hz < 100.0f ? 1 : 0);
        t.str(" Hz");
    }
    end(t);
}

void fmt_time(char *out, int max, float seconds) {
    Text t = text(out, max);
    if (seconds >= 10.0f) {
        t.num(seconds, 1);
        t.str(" s");
    } else if (seconds >= 1.0f) {
        t.num(seconds, 2);
        t.str(" s");
    } else {
        t.num(seconds * 1000.0f, 0);
        t.str(" ms");
    }
    end(t);
}

void fmt_pct(char *out, int max, int v) {
    Text t = text(out, max);
    t.num(unit(v) * 100.0f, 0);
    t.str(" %");
    end(t);
}

void fmt_text(char *out, int max, const char *s) {
    Text t = text(out, max);
    t.str(s);
    end(t);
}

int shared_owner;

} // namespace bwfx

using namespace bwfx;

/* ----------------------------------------------------------------------------------------------- chain --- */
static const Effect *const CHAIN[] = {&DISTORTION, &FILTER, &CHORUS, &DELAY, &REVERB};
enum { COUNT = sizeof(CHAIN) / sizeof(CHAIN[0]) };

struct Slot {
    volatile int value[4];
    volatile uint32_t version; /* the main loop's */
    volatile int on;
    uint32_t applied;          /* the interrupt's */
    float gain;                /* 0 out .. 1 in, the fade */
    int running;               /* reset since it last went quiet */
};
static Slot slot[COUNT];
static volatile int bypassed;
static float chain_gain;

static float L[FW_BLOCK], R[FW_BLOCK];   /* the signal */
static float DL[FW_BLOCK], DR[FW_BLOCK]; /* before the chain, for the bypass fade */
static float SL[FW_BLOCK], SR[FW_BLOCK]; /* before an effect, for its fade */

static const float FADE = 1.0f / (0.01f * FW_FS_HZ); /* 10 ms */
static const float IN = 1.0f / 32768.0f;

static int valid(int fx, int p) { return fx >= 0 && fx < COUNT && p >= 0 && p < BW_FX_PARAMS; }

/* x goes to y, as g fades towards to: from the dry a to the processed b */
static float fade(float *al, float *ar, const float *bl, const float *br, int n, float g, int to) {
    const float step = to ? FADE : -FADE;
    for (int i = 0; i < n; ++i) {
        g = clampf(g + step, 0.0f, 1.0f);
        al[i] += (bl[i] - al[i]) * g;
        ar[i] += (br[i] - ar[i]) * g;
    }
    return g;
}

/* the chain's instance of an effect: the filter's 0, the others' one */
static void *st(int k) { return CHAIN[k]->state(0); }

static void block(int32_t *lr, int n) {
    if (shared_owner != OWNER_CHAIN) { /* the mixer had the shared lines: they start over, with the chain's knobs */
        for (int k = 0; k < COUNT; ++k)
            if (CHAIN[k] != &FILTER) {
                slot[k].running = 0;
                slot[k].applied = slot[k].version - 1u;
            }
        shared_owner = OWNER_CHAIN;
    }
    const int in = !bypassed;
    if (!in && chain_gain == 0.0f) {
        for (int i = 0; i < COUNT; ++i) slot[i].running = 0;
        return;
    }
    for (int i = 0; i < n; ++i) {
        L[i] = (float)lr[2 * i] * IN;
        R[i] = (float)lr[2 * i + 1] * IN;
    }
    const int fading = !in || chain_gain < 1.0f;
    if (fading)
        for (int i = 0; i < n; ++i) {
            DL[i] = L[i];
            DR[i] = R[i];
        }
    for (int k = 0; k < COUNT; ++k) {
        Slot &s = slot[k];
        const Effect &e = *CHAIN[k];
        const uint32_t version = s.version;
        if (version != s.applied) {
            int v[4];
            for (int p = 0; p < 4; ++p) v[p] = s.value[p];
            e.apply(st(k), v);
            s.applied = version;
        }
        const int on = s.on;
        if (!on && s.gain == 0.0f) {
            s.running = 0;
            continue;
        }
        if (!s.running) {
            e.reset(st(k));
            s.running = 1;
        }
        if (on && s.gain == 1.0f) {
            e.process(st(k), L, R, n);
            continue;
        }
        for (int i = 0; i < n; ++i) {
            SL[i] = L[i];
            SR[i] = R[i];
        }
        e.process(st(k), L, R, n);
        /* L, R are the processed; fade from the dry SL, SR into them and keep that */
        s.gain = fade(SL, SR, L, R, n, s.gain, on);
        for (int i = 0; i < n; ++i) {
            L[i] = SL[i];
            R[i] = SR[i];
        }
    }
    if (fading) {
        chain_gain = fade(DL, DR, L, R, n, chain_gain, in);
        for (int i = 0; i < n; ++i) {
            L[i] = DL[i];
            R[i] = DR[i];
        }
    }
    for (int i = 0; i < n; ++i) {
        lr[2 * i] = (int32_t)clampf(L[i] * 32768.0f, -32767.0f, 32767.0f);
        lr[2 * i + 1] = (int32_t)clampf(R[i] * 32768.0f, -32767.0f, 32767.0f);
    }
}

extern "C" {

void bw_fx_init(void) {
    for (int k = 0; k < COUNT; ++k) {
        Slot &s = slot[k];
        for (int p = 0; p < 4; ++p) s.value[p] = CHAIN[k]->def[p];
        s.on = 0;
        s.gain = 0.0f;
        s.running = 0;
        CHAIN[k]->apply(st(k), CHAIN[k]->def);
        CHAIN[k]->reset(st(k));
        s.version = s.applied = 1;
    }
    bypassed = 0;
    chain_gain = 1.0f;
    shared_owner = OWNER_CHAIN;
}

void bw_fx_process(int32_t *lr, uint32_t frames) {
    while (frames) {
        const int n = frames < FW_BLOCK ? (int)frames : (int)FW_BLOCK;
        block(lr, n);
        lr += 2 * n;
        frames -= (uint32_t)n;
    }
}

int bw_fx_count(void) { return COUNT; }
const char *bw_fx_name(int fx) { return valid(fx, 0) ? CHAIN[fx]->name : ""; }
void bw_fx_enable(int fx, int on) {
    if (valid(fx, 0)) slot[fx].on = on != 0;
}
int bw_fx_enabled(int fx) { return valid(fx, 0) ? slot[fx].on : 0; }

const char *bw_fx_param_name(int fx, int p) { return valid(fx, p) ? CHAIN[fx]->param[p] : ""; }
void bw_fx_set(int fx, int p, int value) {
    if (!valid(fx, p)) return;
    slot[fx].value[p] = value < 0 ? 0 : value > BW_FX_MAX ? BW_FX_MAX : value;
    slot[fx].version = slot[fx].version + 1;
}
int bw_fx_get(int fx, int p) { return valid(fx, p) ? slot[fx].value[p] : 0; }
int bw_fx_steps(int fx, int p) { return valid(fx, p) ? CHAIN[fx]->steps[p] : 0; }
void bw_fx_format(int fx, int p, char *out, int max) {
    if (!out || max <= 0) return;
    if (!valid(fx, p)) {
        *out = 0;
        return;
    }
    CHAIN[fx]->format(p, slot[fx].value[p], out, max);
}

void bw_fx_bypass(int on) { bypassed = on != 0; }
int bw_fx_bypassed(void) { return bypassed; }

} // extern "C"
