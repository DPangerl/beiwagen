/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl */
/* Fernbedienung's module pictures, part A: oscillator, wavetable, FM, string, grain, filter, EQ, compressor,
 * distortion, chorus, after Steckfeld SF1's displays. What each picture takes: docs/fernbedienung/pictures-a.md.
 * Unity build, included once by app.c after fb_proto.h and bw_ui.h. Integer maths only: log2, exp2 and sums in
 * the log domain come from small tables; curves are drawn a column at a time (y in 1/16 px). */
#ifndef FB_PICS_A_H
#define FB_PICS_A_H

#define PA_W ((int32_t)FB_PIC_W)
#define PA_H ((int32_t)FB_PIC_H)
#define PA_GRID 12                          /* the raster's pitch, in screen pixels */
#define PA_NONE (-32768)                    /* a column without a trace */
#define PA_LMIN (-64 * 4096)                /* log2 of zero, near enough */
#define PA_HATCH 56                         /* hatching's ink, /256 */
#define PA_GHOST 115                        /* the live trace over the set one, /256 (Steckfeld: 45 %) */

static int16_t pa_ya[FB_PIC_W], pa_yb[FB_PIC_W], pa_yc[FB_PIC_W];
static uint16_t pa_ink;

/* ---- tables: log2(1 + i/32), 2^(i/32), log2(1 + 2^-d) and log2((1 - 2^-d) / d) at d = i/4, tanh(i/16); Q12 / Q16 */
static const uint16_t PA_LOG2[33] = {
    0, 182, 358, 530, 696, 858, 1016, 1169, 1319, 1465, 1607, 1746, 1882, 2015, 2145, 2272, 2396, 2518, 2637, 2754,
    2869, 2982, 3092, 3200, 3307, 3412, 3514, 3615, 3715, 3812, 3908, 4003, 4096
};
static const uint32_t PA_EXP2[33] = {
    65536, 66971, 68438, 69936, 71468, 73032, 74632, 76266, 77936, 79642, 81386, 83169, 84990, 86851, 88752, 90696,
    92682, 94711, 96785, 98905, 101070, 103283, 105545, 107856, 110218, 112631, 115098, 117618, 120194, 122825,
    125515, 128263, 131072
};
static const uint16_t PA_LADD[65] = {
    4096, 3606, 3160, 2757, 2396, 2074, 1789, 1538, 1319, 1128, 962, 819, 696, 591, 501, 424, 358, 303, 256, 216,
    182, 153, 129, 109, 92, 77, 65, 55, 46, 39, 33, 27, 23, 19, 16, 14, 12, 10, 8, 7, 6, 5, 4, 3, 3, 2, 2, 2, 1, 1,
    1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};
static const int16_t PA_LSUB[65] = {
    -2166, -2670, -3160, -3635, -4096, -4542, -4974, -5392, -5796, -6187, -6564, -6929, -7281, -7621, -7950, -8267,
    -8573, -8869, -9155, -9431, -9698, -9956, -10206, -10447, -10681, -10907, -11127, -11339, -11545, -11745,
    -11939, -12128, -12311, -12489, -12663, -12831, -12996, -13156, -13312, -13464, -13612, -13757, -13899, -14037,
    -14173, -14305, -14435, -14561, -14685, -14807, -14926, -15043, -15158, -15270, -15381, -15489, -15595, -15700,
    -15803, -15904, -16003, -16100, -16197, -16291, -16384
};
static const uint16_t PA_TANH[65] = {
    0, 2045, 4075, 6073, 8025, 9919, 11742, 13486, 15142, 16706, 18173, 19541, 20812, 21986, 23065, 24053, 24955,
    25775, 26518, 27190, 27796, 28340, 28829, 29267, 29659, 30009, 30321, 30599, 30846, 31066, 31261, 31435, 31588,
    31725, 31845, 31952, 32047, 32131, 32205, 32270, 32328, 32380, 32425, 32465, 32500, 32531, 32559, 32583, 32605,
    32624, 32641, 32656, 32669, 32680, 32690, 32699, 32707, 32714, 32720, 32726, 32731, 32735, 32739, 32742, 32745
};

/* log2 constants, Q12 */
#define PA_L2_20 17702
#define PA_L2_1000 40820
#define PA_L2_10 13607

/* ---- maths */
static int32_t pa_min(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t pa_max(int32_t a, int32_t b) { return a > b ? a : b; }
static int32_t pa_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static int32_t pa_abs(int32_t v) { return v < 0 ? -v : v; }

/* log2(x), Q12; 0 gives PA_LMIN */
static int32_t pa_log2(uint32_t x)
{
    int32_t b = 31;
    uint32_t m, i, f;
    if (!x)
        return PA_LMIN;
    while (!(x >> b))
        b--;
    m = (b >= 16 ? x >> (b - 16) : x << (16 - b)) & 0xFFFFu;
    i = m >> 11;
    f = m & 2047u;
    return b * 4096 + PA_LOG2[i] + (int32_t)(((PA_LOG2[i + 1] - PA_LOG2[i]) * f) >> 11);
}

/* 2^(l / 4096), Q16; l below 13 * 4096 */
static uint32_t pa_exp2(int32_t l)
{
    const int32_t i = l >> 12;
    const uint32_t f = (uint32_t)l & 4095u, j = f >> 7, g = f & 127u;
    const uint32_t m = PA_EXP2[j] + (((PA_EXP2[j + 1] - PA_EXP2[j]) * g) >> 7);
    return i >= 0 ? m << i : i > -31 ? m >> -i : 0;
}

/* log2(2^a + 2^b) */
static int32_t pa_ladd(int32_t a, int32_t b)
{
    const int32_t m = pa_max(a, b), d = pa_abs(a - b);
    if (d >= 16 * 4096)
        return m;
    return m + PA_LADD[d >> 10] + (((PA_LADD[(d >> 10) + 1] - PA_LADD[d >> 10]) * (d & 1023)) >> 10);
}

/* log2 |2^a - 2^b| */
static int32_t pa_lsub(int32_t a, int32_t b)
{
    const int32_t m = pa_max(a, b), d = pa_abs(a - b);
    if (d >= 16 * 4096)
        return m;
    if (d < 4)
        return m - 16 * 4096;
    return m + pa_log2((uint32_t)d) - 12 * 4096 + PA_LSUB[d >> 10]
           + (((PA_LSUB[(d >> 10) + 1] - PA_LSUB[d >> 10]) * (d & 1023)) >> 10);
}

/* log2 of a magnitude in decibels, Q8 */
static int32_t pa_db8(int32_t l) { return (pa_clamp(l, -40 * 4096, 40 * 4096) * 1541) >> 12; }

/* ---- the picture's values: v14 0..16383, 0 if not sent; live -1 if not sent */
static int32_t pa_v(const fb_pic_t* p, uint32_t i) { return i < p->n ? (int32_t)(p->v[i] & 16383u) : 0; }
static int32_t pa_live(const fb_pic_t* p, uint32_t i) { return i < p->live_n ? (int32_t)(p->live[i] & 16383u) : -1; }

/* a log control: lo + n * span (Q12 log2 units) */
static int32_t pa_lpos(int32_t v, int32_t lo, int32_t span) { return lo + ((v * span) >> 14); }

/* ---- pixels */
static uint16_t pa_mix(uint16_t a, uint16_t b, int32_t t)   /* t 0..256 */
{
    const uint32_t u = (uint32_t)(256 - t), w = (uint32_t)t;
    const uint32_t r = ((uint32_t)(a >> 11) * u + (uint32_t)(b >> 11) * w) >> 8;
    const uint32_t g = ((uint32_t)((a >> 5) & 63u) * u + (uint32_t)((b >> 5) & 63u) * w) >> 8;
    const uint32_t bl = ((uint32_t)(a & 31u) * u + (uint32_t)(b & 31u) * w) >> 8;
    return (uint16_t)(r << 11 | g << 5 | bl);
}

static void pa_px(int32_t x, int32_t y, uint16_t c, int32_t a)
{
    uint16_t* p;
    if ((uint32_t)x >= cv_w || (uint32_t)y >= cv_h || a <= 0)
        return;
    p = &cv_px[(uint32_t)y * cv_w + (uint32_t)x];
    *p = swap16(a >= 256 ? c : pa_mix(swap16(*p), c, a));
}

static void pa_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c, int32_t a)
{
    int32_t i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            pa_px(i, j, c, a);
}

/* how much of pixel row r (16 per pixel) the span lo..hi covers (Q4 both), 0..16 */
static int32_t pa_cov(int32_t lo, int32_t hi, int32_t r)
{
    const int32_t a = pa_max(lo, r * 16), b = pa_min(hi, r * 16 + 16);
    return b > a ? b - a : 0;
}

/* an upright span in column x, lo..hi in Q4, with soft ends */
static void pa_vspan(int32_t x, int32_t lo, int32_t hi, uint16_t c, int32_t a)
{
    int32_t r, r1;
    if (lo > hi) {
        r = lo;
        lo = hi;
        hi = r;
    }
    r1 = pa_min((hi + 15) >> 4, PA_H);
    for (r = pa_max(lo >> 4, 0); r < r1; r++)
        pa_px(x, r, c, a * pa_cov(lo, hi, r) >> 4);
}

/* a level span in row y, x0..x1 in Q4 */
static void pa_hspan(int32_t y, int32_t x0, int32_t x1, uint16_t c, int32_t a)
{
    int32_t x, xe;
    if (x0 > x1) {
        x = x0;
        x0 = x1;
        x1 = x;
    }
    xe = pa_min((x1 + 15) >> 4, PA_W);
    for (x = pa_max(x0 >> 4, 0); x < xe; x++)
        pa_px(x, y, c, a * pa_cov(x0, x1, x) >> 4);
}

/* an upright bar centred on xc (Q4), w wide (Q4), from y0 to y1 (Q4) */
static void pa_vbar(int32_t xc, int32_t y0, int32_t y1, int32_t w, uint16_t c, int32_t a)
{
    int32_t x;
    for (x = pa_max((xc - w / 2) >> 4, 0); x <= pa_min((xc + w / 2) >> 4, PA_W - 1); x++)
        pa_vspan(x, y0, y1, c, a * pa_cov(xc - w / 2, xc + w / 2, x) >> 4);
}

/* a level bar centred on yc (Q4), w thick (Q4), from x0 to x1 (Q4) */
static void pa_hbar(int32_t yc, int32_t x0, int32_t x1, int32_t w, uint16_t c, int32_t a)
{
    int32_t y;
    for (y = pa_max((yc - w / 2) >> 4, 0); y <= pa_min((yc + w / 2) >> 4, PA_H - 1); y++)
        pa_hspan(y, x0, x1, c, a * pa_cov(yc - w / 2, yc + w / 2, y) >> 4);
}

/* dashed 3 on, 3 off, whole pixels */
static void pa_vdash(int32_t x, int32_t y0, int32_t y1, uint16_t c, int32_t a)
{
    int32_t y;
    for (y = y0; y <= y1; y++)
        if ((y - y0) % 6 < 3)
            pa_px(x, y, c, a);
}

static void pa_hdash(int32_t y, int32_t x0, int32_t x1, uint16_t c, int32_t a)
{
    int32_t x;
    for (x = x0; x <= x1; x++)
        if ((x - x0) % 6 < 3)
            pa_px(x, y, c, a);
}

/* a straight line, Q4 ends, 1 px, dashed if dash */
static void pa_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c, int32_t a, int dash)
{
    const int32_t dx = pa_abs(x1 - x0), dy = pa_abs(y1 - y0), n = pa_max(pa_max(dx, dy) >> 4, 1);
    int32_t i;
    for (i = 0; i <= n; i++)
        if (!dash || (i % 6) < 3)
            pa_px((x0 + (x1 - x0) * i / n) >> 4, (y0 + (y1 - y0) * i / n) >> 4, c, a);
}

/* text, its anti-aliasing blended onto what is there */
static int32_t pa_text(int32_t x, int32_t y, const felucca_font_t* f, const char* s, uint16_t c, int32_t a)
{
    for (; *s; s++) {
        const uint32_t gi = glyph(f, (uint8_t)*s), w = f->bw[gi], bpr = ((w >> f->sh) + 1u) / 2u;
        const uint8_t* gd = f->data + f->off[gi];
        uint32_t gx, gy;
        for (gy = 0; gy < f->h; gy++)
            for (gx = 0; gx < w; gx++) {
                const uint32_t sx = gx >> f->sh;
                uint32_t v = gd[(gy >> f->sh) * bpr + sx / 2u];
                v = (sx & 1u) ? (v & 15u) : (v >> 4);
                if (v)
                    pa_px(x - f->pad + (int32_t)gx, y + (int32_t)gy, c, a * (int32_t)v / 15);
            }
        x += f->adv[gi];
    }
    return x;
}

/* ---- the display: gradient, raster */
static void pa_ground(const fb_theme_t* th)
{
    int32_t x, y;
    cv_begin(FB_PIC_W, FB_PIC_H, th->bg_top);
    for (y = 0; y < PA_H; y++) {
        const uint16_t c = swap16(pa_mix(th->bg_top, th->bg_bottom, y * 256 / (PA_H - 1)));
        const uint16_t g = swap16(th->grid);
        uint16_t* row = &cv_px[(uint32_t)y * FB_PIC_W];
        const int line = y > 0 && (y + (int32_t)FB_PIC_Y) % PA_GRID == 0;
        for (x = 0; x < PA_W; x++)
            row[x] = line || (x > 0 && x % PA_GRID == 0) ? g : c;
    }
}

/* ---- traces: y per column (Q4), PA_NONE where there is none */

/* a curve: core hw (half width, Q4) at alpha a; glow: Steckfeld's two soft strokes around it */
static void pa_trace(const int16_t* y, int32_t x0, int32_t x1, uint16_t c, int32_t a, int32_t hw, int glow)
{
    int32_t x, r;
    x0 = pa_max(x0, 0);
    x1 = pa_min(x1, PA_W);
    for (x = x0; x < x1; x++) {
        int32_t lo, hi, ext, r1;
        if (y[x] == PA_NONE)
            continue;
        lo = hi = y[x];
        if (x > x0 && y[x - 1] != PA_NONE) {
            lo = pa_min(lo, (y[x] + y[x - 1]) / 2);
            hi = pa_max(hi, (y[x] + y[x - 1]) / 2);
        }
        if (x + 1 < x1 && y[x + 1] != PA_NONE) {
            lo = pa_min(lo, (y[x] + y[x + 1]) / 2);
            hi = pa_max(hi, (y[x] + y[x + 1]) / 2);
        }
        ext = glow ? 48 : hw;
        r1 = pa_min((hi + ext + 15) >> 4, PA_H);
        for (r = pa_max((lo - ext) >> 4, 0); r < r1; r++) {
            int32_t t = a * pa_cov(lo - hw, hi + hw, r) >> 4;
            if (glow) {
                int32_t inv = (256 - t) * (256 - pa_cov(lo - 26, hi + 26, r) * 77 / 16) >> 8;
                inv = inv * (256 - pa_cov(lo - 48, hi + 48, r) * 31 / 16) >> 8;
                t = 256 - inv;
            }
            pa_px(x, r, c, t);
        }
    }
}

static void pa_glow(const int16_t* y, int32_t x0, int32_t x1) { pa_trace(y, x0, x1, pa_ink, 256, 12, 1); }
static void pa_dim(const int16_t* y, int32_t x0, int32_t x1, int32_t a) { pa_trace(y, x0, x1, pa_ink, a, 8, 0); }

/* upright hatching between two traces (or a trace and a level, b == 0: base), every `step` px */
static void pa_hatch(const int16_t* a, const int16_t* b, int32_t base, int32_t x0, int32_t x1, int32_t step)
{
    int32_t x;
    for (x = pa_max(x0, 0); x < pa_min(x1, PA_W); x++) {
        if (x % step || a[x] == PA_NONE || (b && b[x] == PA_NONE))
            continue;
        pa_vspan(x, a[x], b ? b[x] : base, pa_ink, PA_HATCH);
    }
}

/* a white node, as Steckfeld marks a band */
static void pa_node(int32_t xq, int32_t yq)
{
    pa_rect((xq >> 4) - 2, (yq >> 4) - 2, 5, 5, 0xFFFFu, 256);
}

/* a polyline (Q4 points, x rising) into a per-column trace */
static void pa_polyline(int16_t* y, const int32_t* px, const int32_t* py, int32_t n)
{
    int32_t i, x;
    for (x = 0; x < PA_W; x++)
        y[x] = PA_NONE;
    for (i = 0; i + 1 < n; i++) {
        const int32_t xa = px[i], xb = px[i + 1];
        for (x = pa_max((xa + 8) >> 4, 0); x <= pa_min((xb + 7) >> 4, PA_W - 1); x++) {
            const int32_t xc = x * 16 + 8;
            if (xc < xa || xc > xb)
                continue;
            y[x] = (int16_t)(xb > xa ? py[i] + (py[i + 1] - py[i]) * (xc - xa) / (xb - xa) : py[i]);
        }
    }
}

static int32_t pa_isqrt(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return (int32_t)r;
}

static void pa_fmt_fix2(char* b, int32_t hundredths)   /* 1.25 */
{
    char d[4];
    b[0] = 0;
    ui_cat_int(b, hundredths / 100, 12);
    ui_cat(b, ".", 12);
    d[0] = (char)('0' + hundredths / 10 % 10);
    d[1] = (char)('0' + hundredths % 10);
    d[2] = 0;
    ui_cat(b, d, 12);
}

/* ==== oscillator: v0 wave, v1 tune, v2 level, v3 width; live: tune, level, width */
static const int16_t PA_STAIRS[8] = { 9830, -22938, 27853, -6554, 18022, -29491, 3277, 22938 };

static int32_t pa_wave(int32_t wave, uint32_t t, uint32_t width)   /* t Q16 in the cycle, out Q15 */
{
    switch (wave) {
    case 0: return FW_SINE[t >> 6];
    case 1: return pa_max(32767 - 2 * pa_abs((int32_t)t - 32768), -32767);
    case 2: return (int32_t)t - 32768;
    case 3: return t < width ? 32767 : -32767;
    case 4: return PA_STAIRS[(t * 8u) >> 16];
    case 5: return ((int32_t)((((t * 48u) >> 16) * 2654435761u >> 16) % 2000u) - 1000) * 32767 / 1000;
    default: return 0;
    }
}

static void pa_osc_trace(int16_t* y, int32_t wave, int32_t tune, int32_t level, int32_t width)
{
    const uint32_t cycles = pa_exp2(tune - 4096);   /* 2 * 2^(tune / 12), tune -24..24 over the sweep */
    const uint32_t w16 = 3277u + (uint32_t)((width * 58982) >> 14);
    const int32_t mid = PA_H * 8, amp = (level * (PA_H / 2 - 6) * 16) >> 14;
    int32_t x;
    for (x = 0; x < PA_W; x++)
        y[x] = (int16_t)(mid - (amp * pa_wave(wave, ((uint32_t)x * cycles / (uint32_t)PA_W) & 0xFFFFu, w16) >> 15));
}

static void pa_oscillator(const fb_pic_t* p)
{
    const int32_t wave = pa_v(p, 0);
    pa_osc_trace(pa_ya, wave, pa_v(p, 1), pa_v(p, 2), pa_v(p, 3));
    pa_hatch(pa_ya, 0, PA_H * 8, 0, PA_W, 3);
    pa_glow(pa_ya, 0, PA_W);
    if (p->live_n >= 3) {
        pa_osc_trace(pa_yb, wave, pa_live(p, 0), pa_live(p, 1), pa_live(p, 2));
        pa_dim(pa_yb, 0, PA_W, PA_GHOST);
    }
}

/* ==== wavetable: v1 position, v2 warp, v3 level; arr: 8 frames x 64 samples (64 + 63 * s); live: position, warp */
static int32_t pa_wt_sample(const uint8_t* arr, int32_t pos, uint32_t ph)   /* pos Q14, ph Q16 -> Q15 */
{
    const int32_t sw = pos * 7, f = pa_min(sw >> 14, 6), blend = sw - f * 16384;
    const uint32_t at = ph * 64u, i = (at >> 16) & 63u, j = (i + 1u) & 63u;
    const int32_t fr = (int32_t)((at & 0xFFFFu) >> 2);   /* Q14 */
    const uint8_t* a = arr + f * 64;
    const uint8_t* b = a + 64;
    const int32_t sa = (a[i] - 64) * 256 + (((a[j] - a[i]) * fr) >> 6);   /* Q8 of a step */
    const int32_t sb = (b[i] - 64) * 256 + (((b[j] - b[i]) * fr) >> 6);
    return ((sa + (((sb - sa) * blend) >> 14)) * 520) >> 8;
}

/* a frame's baseline: further back, higher (0.7 - 0.4 back of the height), Q4 */
static int32_t pa_wt_base(int32_t pos) { return PA_H * 16 * (11469 - ((6554 * (16384 - pos)) >> 14)) >> 14; }

/* the table at pos as one frame of the waterfall: further back, further right; its columns l..r */
static void pa_wt_trace(int16_t* y, const uint8_t* arr, int32_t pos, int32_t warp, int32_t level, int32_t* l, int32_t* r)
{
    const int32_t base = pa_wt_base(pos), amp = (PA_H * 16 * 3 / 10) * level >> 14;
    const int32_t left = (PA_W * ((2949 * (16384 - pos)) >> 14)) >> 14, width = PA_W * 82 / 100;   /* 0.18 back, 0.82 */
    int32_t x;
    for (x = 0; x < PA_W; x++) {
        uint32_t ph;
        int32_t w;
        if (x < left || x > left + width) {
            y[x] = PA_NONE;
            continue;
        }
        ph = (uint32_t)((x - left) * 65535 / width);
        w = (int32_t)ph + ((((warp * FW_SINE[(ph >> 6) & 1023u]) >> 13) * 9387) >> 16);   /* + 0.9 warp sin / 2pi */
        y[x] = (int16_t)(base - (amp * pa_wt_sample(arr, pos, (uint32_t)pa_clamp(w, 0, 65535)) >> 15));
    }
    *l = left;
    *r = left + width + 1;
}

static void pa_wt_lit(const fb_pic_t* p, int32_t lit)
{
    int32_t l, r;
    pa_wt_trace(pa_ya, p->arr, lit, pa_v(p, 2), pa_v(p, 3), &l, &r);
    pa_hatch(pa_ya, 0, pa_wt_base(lit), l, r, 3);
    pa_glow(pa_ya, l, r);
}

/* seven frames from the first (at the back) to the last, the position's trace hatched and glowing among them */
static void pa_wavetable(const fb_pic_t* p)
{
    const int32_t lit = pa_v(p, 1);
    int32_t k, drawn = 0, l, r;
    if (p->arr_n < 512u)
        return;
    for (k = 0; k < 7; k++) {
        const int32_t pos = k * 16384 / 6;
        if (!drawn && lit <= pos) {
            pa_wt_lit(p, lit);
            drawn = 1;
        }
        pa_wt_trace(pa_yb, p->arr, pos, pa_v(p, 2), pa_v(p, 3), &l, &r);
        pa_dim(pa_yb, l, r, (102 - ((64 * (16384 - pos)) >> 14)) * 2 / 3);   /* 0.4 - 0.25 back, 0.7 px */
    }
    if (!drawn)
        pa_wt_lit(p, lit);
    if (p->live_n >= 2) {
        pa_wt_trace(pa_yb, p->arr, pa_live(p, 0), pa_live(p, 1), pa_v(p, 3), &l, &r);
        pa_dim(pa_yb, l, r, PA_GHOST);
    }
}

/* ==== FM: v0 algorithm 0..7, v1 index, v2 feedback, v3 level, v4..7 ratio x4 (1..64), v8..11 amounts;
   live: index, feedback, level */
static const uint8_t PA_FM_MOD[8][4] = {
    { 2, 4, 8, 0 }, { 2, 12, 0, 0 }, { 6, 0, 8, 0 }, { 6, 8, 8, 0 }, { 2, 0, 8, 0 }, { 8, 8, 8, 0 }, { 0, 0, 8, 0 },
    { 0, 0, 0, 0 }
};
static const uint8_t PA_FM_CAR[8] = { 1, 1, 1, 1, 5, 7, 7, 15 };

#define PA_FM_BW 52                         /* an operator's box */

/* Where the wave goes: with one or two rows of operators the diagram spans the top and the wave runs under it, as
   in Steckfeld; with three or four the diagram takes the left and the wave the right. Q4 except x0, w (px). */
typedef struct {
    int32_t dw, top, bottom, bus;           /* the diagram */
    int32_t x0, w, mid, amp;                /* the wave */
} pa_fm_layout_t;

static void pa_fm_place(pa_fm_layout_t* l, int32_t rows)
{
    if (rows >= 3) {
        l->dw = 118;
        l->top = 5 * 16;
        l->bottom = 80 * 16;
        l->bus = 82 * 16;
        l->x0 = 134;
        l->w = PA_W - 136;
        l->mid = 38 * 16;
        l->amp = 30 * 16;
    } else {
        l->dw = PA_W - 10;                  /* room for the feedback loop right of operator 4 */
        l->top = 6 * 16;
        l->bottom = 54 * 16;
        l->bus = 54 * 16;
        l->x0 = 0;
        l->w = PA_W;
        l->mid = 71 * 16;
        l->amp = 13 * 16;
    }
}

/* FmVoice::process with every operator at full level, two cycles, into the wave's columns */
static void pa_fm_trace(int16_t* y, const pa_fm_layout_t* l, int32_t algo, int32_t index, int32_t fb,
                        const fb_pic_t* p, int32_t level)
{
    const uint8_t* mod = PA_FM_MOD[algo];
    const int32_t car = PA_FM_CAR[algo], carriers = (car & 1) + (car >> 1 & 1) + (car >> 2 & 1) + (car >> 3);
    const int32_t amp = (l->amp * 92 / 100) * level >> 14;
    uint32_t ph[4] = { 0, 0, 0, 0 }, inc[4];
    int32_t out[4], prev = 0, before = 0, i, op, src, next = 0, col = 0;
    for (op = 0; op < 4; op++)
        inc[op] = (uint32_t)pa_clamp(pa_v(p, 4u + (uint32_t)op), 1, 64) << 21;   /* ratio / 512 of a cycle */
    for (i = 0; i < 1024 && col < l->w; i++) {
        int32_t mix = 0;
        for (op = 3; op >= 0; op--) {
            int32_t m = 0;   /* phase modulation in cycles, Q16 */
            for (src = op + 1; src < 4; src++)
                if (mod[op] & (1 << src))
                    m += (out[src] * index) >> 13;
            if (op == 3)
                m += ((prev + before) * fb) >> 15;
            out[op] = (FW_SINE[(ph[op] + ((uint32_t)m << 16)) >> 22] * pa_v(p, 8u + (uint32_t)op)) >> 14;
            if (car & (1 << op))
                mix += out[op];
            ph[op] += inc[op];
        }
        before = prev;
        prev = out[3];
        if (i >= next) {
            y[l->x0 + col] = (int16_t)(l->mid - (amp * (mix / carriers) >> 15));
            col++;
            next = col * 1023 / (l->w - 1);
        }
    }
}

/* where an operator sits: over the middle of what it drives (Q4, whole pixels) */
static int32_t pa_fm_over(const uint8_t* mod, const int32_t* xs, int32_t op)
{
    int32_t t, sum = 0, cnt = 0;
    for (t = 0; t < op; t++)
        if (mod[t] & (1 << op)) {
            sum += xs[t];
            cnt++;
        }
    return cnt ? sum / cnt / 16 * 16 : 0;
}

/* a box's outline, t px thick */
static void pa_frame(int32_t x, int32_t y, int32_t w, int32_t h, int32_t t, uint16_t c, int32_t a)
{
    pa_rect(x, y, w, t, c, a);
    pa_rect(x, y + h - t, w, t, c, a);
    pa_rect(x, y + t, t, h - 2 * t, c, a);
    pa_rect(x + w - t, y + t, t, h - 2 * t, c, a);
}

static void pa_fm(const fb_pic_t* p, const fb_theme_t* th)
{
    static const char* const FRAC[4] = { "", ".25", ".5", ".75" };
    const int32_t algo = pa_clamp(pa_v(p, 0), 0, 7), index = pa_v(p, 1), fb = pa_v(p, 2), car = PA_FM_CAR[algo];
    const uint8_t* mod = PA_FM_MOD[algo];
    const int32_t wire = 10 + ((26 * index) >> 14);
    pa_fm_layout_t l;
    int32_t depth[4], over[4], xs[4], ys[4], rows = 1, pitch, bh, op, t, r, wc, bl, br;
    for (op = 0; op < 4; op++) {   /* rows up from the carriers */
        depth[op] = 0;
        for (t = 0; t < op; t++)
            if (mod[t] & (1 << op))
                depth[op] = pa_max(depth[op], depth[t] + 1);
        rows = pa_max(rows, depth[op] + 1);
    }
    pa_fm_place(&l, rows);
    wc = (l.x0 + l.w / 2) * 16;
    pitch = (l.bottom - l.top) / rows;
    bh = pa_min(22 * 16, pitch - 3 * 16);
    for (r = 0; r < rows; r++) {   /* each row over what it drives; those sharing a spot spread round it */
        int32_t n = 0, i = 0;
        for (op = 0; op < 4; op++)
            if (depth[op] == r) {
                over[op] = pa_fm_over(mod, xs, op);
                ys[op] = l.bottom - pitch * (2 * r + 1) / 2;
                n++;
            }
        for (op = 0; op < 4; op++) {
            int32_t g = 0, k = 0, j;
            if (depth[op] != r)
                continue;
            if (!r) {
                xs[op] = l.dw * 16 * (2 * i++ + 1) / (2 * n);
                continue;
            }
            for (j = 0; j < 4; j++)
                if (depth[j] == r && over[j] == over[op]) {
                    g++;
                    k += j < op;
                }
            xs[op] = over[op] + (2 * k - (g - 1)) * (PA_FM_BW + 6) * 8;
        }
    }
    for (op = 0; op < 4; op++)   /* wires, thicker with Index */
        for (t = 0; t < op; t++)
            if (mod[t] & (1 << op)) {
                const int32_t y0 = ys[op] + bh / 2, y1 = ys[t] - bh / 2, bend = (y0 + y1) / 2;
                pa_vbar(xs[op], y0, bend + wire / 2, wire, pa_ink, 256);
                pa_hbar(bend, pa_min(xs[op], xs[t]) - wire / 2, pa_max(xs[op], xs[t]) + wire / 2, wire, pa_ink, 256);
                pa_vbar(xs[t], bend - wire / 2, y1, wire, pa_ink, 256);
            }
    bl = br = wc;   /* the carriers' bus, out to the wave */
    for (op = 0; op < 4; op++)
        if (car & (1 << op)) {
            pa_vbar(xs[op], ys[op] + bh / 2, l.bus + 12, 24, pa_ink, 256);
            bl = pa_min(bl, xs[op]);
            br = pa_max(br, xs[op]);
        }
    pa_hbar(l.bus, bl - 12, br + 12, 24, pa_ink, 256);
    if (rows >= 3)
        pa_vbar(wc, l.mid + l.amp + 2 * 16, l.bus, 24, pa_ink, 256);
    else
        pa_vbar(wc, l.bus, l.mid - l.amp, 24, pa_ink, 256);
    {   /* only operator 4 feeds back on itself */
        const int32_t rx = xs[3] + PA_FM_BW * 8, lx = rx + 7 * 16, ty = ys[3] - bh / 2 - 5 * 16;
        if (fb > 0) {
            const int32_t w = 10 + ((26 * fb) >> 14);
            pa_hbar(ys[3], rx, lx + w / 2, w, pa_ink, 256);
            pa_vbar(lx, ty - w / 2, ys[3] + w / 2, w, pa_ink, 256);
            pa_hbar(ty, xs[3] - w / 2, lx + w / 2, w, pa_ink, 256);
            pa_vbar(xs[3], ty - w / 2, ys[3] - bh / 2, w, pa_ink, 256);
        } else {
            pa_hdash(ys[3] >> 4, rx >> 4, lx >> 4, pa_ink, 102);
            pa_vdash(lx >> 4, ty >> 4, ys[3] >> 4, pa_ink, 102);
            pa_hdash(ty >> 4, xs[3] >> 4, lx >> 4, pa_ink, 102);
            pa_vdash(xs[3] >> 4, ty >> 4, (ys[3] - bh / 2) >> 4, pa_ink, 102);
        }
    }
    for (op = 0; op < 4; op++) {   /* the operators: number, ratio, a strip for the amount */
        const int32_t bx = (xs[op] >> 4) - PA_FM_BW / 2, by = (ys[op] - bh / 2 + 8) >> 4, h = bh >> 4;
        const int32_t r4 = pa_clamp(pa_v(p, 4u + (uint32_t)op), 1, 64), strip = h < 18 ? 2 : 3;
        const int32_t ty = by + (h - strip - 13) / 2;   /* the figures' 11 px between frame and strip */
        char b[12], num[2];
        num[0] = (char)('1' + op);
        num[1] = 0;
        b[0] = 0;
        ui_cat_int(b, r4 / 4, 12);
        ui_cat(b, FRAC[r4 & 3], 12);
        pa_rect(bx, by, PA_FM_BW, h, th->bg_bottom, 256);
        if ((car >> op) & 1)
            pa_frame(bx, by, PA_FM_BW, h, 2, pa_ink, 256);
        else
            pa_frame(bx, by, PA_FM_BW, h, 1, pa_ink, 205);
        pa_text(bx + 5, ty, &FONT_T, num, pa_ink, 256);
        pa_text(bx + PA_FM_BW - 5 - text_w(&FONT_T, b), ty, &FONT_T, b, pa_ink, 180);
        pa_rect(bx, by + h - strip, (PA_FM_BW * pa_v(p, 8u + (uint32_t)op)) >> 14, strip, pa_ink, 256);
    }
    for (t = 0; t < PA_W; t++)   /* two cycles of the voice */
        pa_ya[t] = pa_yb[t] = PA_NONE;
    pa_fm_trace(pa_ya, &l, algo, index, fb, p, pa_v(p, 3));
    pa_hatch(pa_ya, 0, l.mid, l.x0, l.x0 + l.w, 3);
    pa_glow(pa_ya, l.x0, l.x0 + l.w);
    if (p->live_n >= 3) {
        pa_fm_trace(pa_yb, &l, algo, pa_live(p, 0), pa_live(p, 1), p, pa_live(p, 2));
        pa_dim(pa_yb, l.x0, l.x0 + l.w, PA_GHOST);
    }
}

/* ==== string: v0 pluck position (0..0.5), v1 pick, v2 level; live: level */

/* the string pulled out at the pluck to side (+1 up, -1 down), its corner rounded by the pick */
static void pa_pulled(int16_t* y, int32_t pos, int32_t pick, int32_t level, int32_t side)
{
    const int32_t mid = PA_H * 8, lx = 6 * 16, rx = (PA_W - 6) * 16, span = rx - lx;
    const int32_t reach = (level * (PA_H / 2 - 6) * 16) >> 14;
    const int32_t px = lx + span * pa_max(pos >> 1, 328) / 16384, py = mid - side * reach;   /* max(position, 0.02) */
    const int32_t rad = span * (16384 - pick) / 5 >> 14;
    const int32_t ax = lx - px, ay = mid - py, bx = rx - px, by = mid - py;
    const int32_t la = pa_isqrt((uint32_t)(ax * ax + ay * ay)), lb = pa_isqrt((uint32_t)(bx * bx + by * by));
    const int32_t cross = pa_abs(ax * by - ay * bx) >> 6;
    int32_t xs[12], ys[12], n = 0, d, k;
    /* the arc's tangent points sit r / tan(angle / 2) = r (|a| |b| + a.b) / |a x b| from the corner */
    d = cross ? rad * ((la * lb + ax * bx + ay * by) >> 6) / cross : 0;
    d = pa_min(d, pa_min(la, lb) * 9 / 10);
    xs[n] = lx;
    ys[n++] = mid;
    if (d > 8 && la && lb) {
        const int32_t x1 = px + ax * d / la, y1 = py + ay * d / la, x2 = px + bx * d / lb, y2 = py + by * d / lb;
        for (k = 0; k <= 8; k++) {   /* the corner as a quadratic curve through the tangent points */
            const int32_t u = 8 - k;
            xs[n] = (x1 * u * u + 2 * px * u * k + x2 * k * k) / 64;
            ys[n++] = (y1 * u * u + 2 * py * u * k + y2 * k * k) / 64;
        }
    } else {
        xs[n] = px;
        ys[n++] = py;
    }
    xs[n] = rx;
    ys[n++] = mid;
    pa_polyline(y, xs, ys, n);
}

static void pa_string(const fb_pic_t* p)
{
    const int32_t pos = pa_v(p, 0), pick = pa_v(p, 1), level = pa_v(p, 2), mid = PA_H * 8;
    const int32_t reach = (level * (PA_H / 2 - 6) * 16) >> 14;
    int32_t x;
    pa_pulled(pa_ya, pos, pick, level, 1);
    pa_pulled(pa_yb, pos, pick, level, -1);
    pa_hatch(pa_ya, pa_yb, 0, 0, PA_W, 3);
    for (x = 0; x < PA_W; x++) {   /* the fundamental it settles into, dashed */
        const int32_t t = (x - 6) * 65536 / (PA_W - 12);
        pa_yc[x] = (int16_t)(x < 6 || x > PA_W - 6 ? PA_NONE : mid - ((reach * 45 / 100) * FW_SINE[(t >> 7) & 1023] >> 15));
        if ((x / 3) & 1)
            pa_yc[x] = PA_NONE;
    }
    pa_dim(pa_yc, 0, PA_W, 102);
    pa_vbar(6 * 16 + 8, mid - 9 * 16, mid + 9 * 16, 32, pa_ink, 256);   /* the bridges */
    pa_vbar((PA_W - 6) * 16 - 8, mid - 9 * 16, mid + 9 * 16, 32, pa_ink, 256);
    pa_dim(pa_yb, 0, PA_W, 102);
    pa_glow(pa_ya, 0, PA_W);
    if (p->live_n >= 1) {
        pa_pulled(pa_yc, pos, pick, pa_live(p, 0), 1);
        pa_dim(pa_yc, 0, PA_W, PA_GHOST);
    }
}

/* ==== grain: v0 position, v1 spray, v2 freeze, v3 size, v4 pitch, v5 mix; arr: 128 peaks, then per grain
   place (2 bytes, 14 bits) and gain; live: position */
#define PA_GR_TOP 15
#define PA_GR_BOT 68

static void pa_disc(int32_t cx, int32_t cy, int fill)   /* a 7 px dot, filled or a ring */
{
    static const uint8_t DOT[7] = { 0x1C, 0x3E, 0x7F, 0x7F, 0x7F, 0x3E, 0x1C };
    static const uint8_t RING[7] = { 0x1C, 0x22, 0x41, 0x41, 0x41, 0x22, 0x1C };
    int32_t i, j;
    for (j = 0; j < 7; j++)
        for (i = 0; i < 7; i++)
            if (((fill ? DOT : RING)[j] >> i) & 1)
                pa_px(cx - 3 + i, cy - 3 + j, pa_ink, 256);
}

static void pa_grain(const fb_pic_t* p)
{
    const int32_t top = PA_GR_TOP * 16, bot = PA_GR_BOT * 16, mid = (top + bot) / 2, half = (bot - top) / 2;
    const int32_t pos = pa_v(p, 0), freeze = pa_v(p, 2) != 0;
    const int32_t x = (PA_W * pos) >> 14, reach = (PA_W * pa_v(p, 1) / 6) >> 14;
    const uint32_t peaks = p->arr_n < 128u ? p->arr_n : 128u;
    int32_t i, ms, st, mix;
    char b[40], n[12];
    if (reach > 1)   /* how far Spray scatters round Position */
        for (i = pa_max(x - reach, 0); i <= pa_min(x + reach, PA_W - 1); i++)
            if (!(i % 4))
                pa_vspan(i, top, bot, pa_ink, PA_HATCH);
    pa_vdash(x, PA_GR_TOP, PA_GR_BOT - 1, pa_ink, 102);
    pa_hspan(mid >> 4, 0, PA_W * 16, pa_ink, 60);
    for (i = 0; i < (int32_t)peaks; i++) {   /* the buffer, oldest on the left */
        const int32_t h = half * pa_min(p->arr[i], 127) / 127;
        if (h >= 4)
            pa_vspan((2 * i + 1) * PA_W / 256, mid - h, mid + h, pa_ink, freeze ? 230 : 140);
    }
    for (i = 128; i + 2 < (int32_t)p->arr_n; i += 3) {   /* the grains playing, as tall as their window is open */
        const int32_t place = (p->arr[i] & 127) << 7 | (p->arr[i + 1] & 127), gain = p->arr[i + 2] & 127;
        const int32_t gx = pa_min((PA_W * place) >> 14, PA_W - 1), r = half * (19 + 108 * gain / 127) / 127;
        pa_vspan(gx - 2, mid - r, mid + r, pa_ink, 31);
        pa_vspan(gx + 2, mid - r, mid + r, pa_ink, 31);
        pa_vspan(gx - 1, mid - r, mid + r, pa_ink, 100);
        pa_vspan(gx + 1, mid - r, mid + r, pa_ink, 100);
        pa_vspan(gx, mid - r - 8, mid + r + 8, pa_ink, 256);
    }
    if (p->live_n >= 1)
        pa_vspan((PA_W * pa_live(p, 0)) >> 14, top, bot, pa_ink, PA_GHOST);
    pa_text(2, -2, &FONT_T, "OLD", pa_ink, 102);
    {
        const char* state = freeze ? "FREEZE" : "REC";
        const int32_t w = text_w(&FONT_T, state);
        pa_text(PA_W - 2 - w, -2, &FONT_T, state, pa_ink, 256);
        pa_disc(PA_W - 2 - w - 7, 6, freeze || (fm1_ms / 500u) % 2u == 0);
    }
    ms = (int32_t)((pa_exp2((pa_v(p, 3) * 27213) >> 14) * 10u + 32768u) >> 16);   /* 0.01 .. 1 s, log */
    st = (pa_v(p, 4) * 48 + 8191) / 16383 - 24;
    mix = (pa_v(p, 5) * 100 + 8191) / 16383;
    b[0] = 0;
    if (ms >= 1000) {
        pa_fmt_fix2(b, (ms + 5) / 10);
        ui_cat(b, " s", 40);
    } else {
        ui_cat_int(b, ms, 40);
        ui_cat(b, " ms", 40);
    }
    ui_cat(b, "   ", 40);
    n[0] = 0;
    if (st >= 0)
        ui_cat(n, "+", 12);
    ui_cat_int(n, st, 12);
    ui_cat(b, n, 40);
    ui_cat(b, " st   MIX ", 40);
    ui_cat_int(b, mix, 40);
    pa_text(2, PA_H - 15, &FONT_T, b, pa_ink, 256);
}

/* ==== filter: v0 mode (LP HP BP notch morph), v1 cutoff (20..18000 Hz, log), v2 resonance, v3 24 dB, v4 morph;
   live: cutoff, resonance, morph */
#define PA_L2_900 40197

/* the engine's SVF at log2(f / cutoff) = l (Q12): log2 |H|, Q12 */
static int32_t pa_svf(int32_t mode, int32_t l, int32_t res, int32_t steep, int32_t morph)
{
    const int32_t lk = pa_log2((uint32_t)pa_max(2 * (16384 - res), 328)) - 14 * 4096;   /* max(2 (1 - res), 0.02) */
    const int32_t lgain = pa_log2((uint32_t)(16384 - res / 2)) - 14 * 4096;              /* 1 - res / 2 */
    const int32_t la = pa_lsub(0, 2 * l);                                                /* |1 - w^2| */
    const int32_t lden = pa_ladd(2 * la, 2 * lk + 2 * l) / 2;
    int32_t m;
    switch (mode) {
    case 1: m = 2 * l - lden; break;
    case 2: m = lk + l - lden; break;
    case 3: m = la - lden; break;
    case 4: {   /* lowpass into bandpass into highpass */
        const int32_t t = pa_min(2 * morph, 32768);
        const int32_t lband = pa_log2((uint32_t)(t < 16384 ? t : 32768 - t)) - 14 * 4096;
        const int32_t num = t < 16384
            ? pa_ladd(2 * (lgain + pa_log2((uint32_t)(16384 - t)) - 14 * 4096), 2 * lband + 2 * l)
            : pa_ladd(2 * (pa_log2((uint32_t)(t - 16384)) - 14 * 4096) + 4 * l, 2 * lband + 2 * l);
        return num / 2 - lden;
    }
    default: m = lgain - lden; break;
    }
    if (steep) {   /* the Butterworth second stage */
        const int32_t lbw = pa_ladd(2 * la, 4096 + 2 * l) / 2;
        m += mode == 1 ? 2 * l - lbw : mode == 2 ? 2048 + l - lbw : mode == 3 ? la - lbw : -lbw;
    }
    return m;
}

static int32_t pa_filter_y(int32_t m) { return 16 * PA_H * (18 * 256 - pa_db8(m)) / (54 * 256); }   /* +18 .. -36 dB */

/* log2 f at column x's middle, 20 Hz .. 20 kHz */
static int32_t pa_col_l2f(int32_t x) { return PA_L2_20 + (2 * x + 1) * PA_L2_1000 / (2 * PA_W); }

static void pa_filter_curve(int16_t* y, int32_t mode, int32_t lfc, int32_t res, int32_t steep, int32_t morph)
{
    int32_t x;
    for (x = 0; x < PA_W; x++)
        y[x] = (int16_t)pa_clamp(pa_filter_y(pa_svf(mode, pa_col_l2f(x) - lfc, res, steep, morph)), -32, PA_H * 16 + 32);
}

static void pa_freq_ticks(void)
{
    static const int32_t DEC[3] = { 27213, 40820, 54426 };   /* log2 100, 1k, 10k */
    int32_t i;
    for (i = 0; i < 3; i++)
        pa_vspan((DEC[i] - PA_L2_20) * PA_W / PA_L2_1000, (PA_H - 5) * 16, PA_H * 16, pa_ink, 102);
}

static void pa_filter(const fb_pic_t* p)
{
    const int32_t mode = pa_v(p, 0), lfc = pa_lpos(pa_v(p, 1), PA_L2_20, PA_L2_900), res = pa_v(p, 2);
    const int32_t steep = mode != 4 && pa_v(p, 3), morph = pa_v(p, 4);
    const int32_t cx = (lfc - PA_L2_20) * PA_W / PA_L2_1000;
    pa_hspan(PA_H * 18 / 54, 0, PA_W * 16, pa_ink, 46);   /* unity */
    pa_freq_ticks();
    pa_filter_curve(pa_ya, mode, lfc, res, steep, morph);
    pa_hatch(pa_ya, 0, PA_H * 16, 0, PA_W, 3);
    pa_vdash(cx, 0, PA_H - 1, pa_ink, 102);
    pa_glow(pa_ya, 0, PA_W);
    if (p->live_n >= 3) {
        pa_filter_curve(pa_yb, mode, pa_lpos(pa_live(p, 0), PA_L2_20, PA_L2_900), pa_live(p, 1), steep, pa_live(p, 2));
        pa_dim(pa_yb, 0, PA_W, PA_GHOST);
    }
    pa_node(cx * 16 + 8, pa_filter_y(pa_svf(mode, 0, res, steep, morph)));
}

/* ==== EQ: v0..2 gain low, mid, high (-18..18 dB), v3 low (20..500 Hz), v4 mid (100..8000), v5 high (1k..18k),
   v6 Q (0.3..8); all frequencies and Q log; live: mid frequency */

/* log2 |s^2 a + s b + c| at s = jw, every term log2 (Q12), l = log2 w */
static int32_t pa_lsize(int32_t la, int32_t lb, int32_t lc, int32_t l)
{
    return pa_ladd(2 * pa_lsub(lc, la + 2 * l), 2 * (lb + l)) / 2;
}

static int32_t pa_eq_l2(const fb_pic_t* p, int32_t lf, int32_t lmid)   /* log2 of the three bands' gain at f */
{
    int32_t m = 0, i;
    for (i = 0; i < 3; i++) {
        const int32_t g8 = ((pa_v(p, (uint32_t)i) * 9216) >> 14) - 4608;   /* dB, Q8 */
        const int32_t la = (g8 * 5443) >> 12, lb = 2048 + la / 2;          /* 10^(g / 40); sqrt 2 * sqrt a */
        if (i == 0) {
            const int32_t l = lf - pa_lpos(pa_v(p, 3), PA_L2_20, 19021);
            m += la + pa_lsize(0, lb, la, l) - pa_lsize(la, lb, 0, l);
        } else if (i == 1) {
            const int32_t l = lf - lmid, lq = pa_lpos(pa_v(p, 6), -7115, 19402);
            m += pa_lsize(0, la - lq, 0, l) - pa_lsize(0, -la - lq, 0, l);
        } else {
            const int32_t l = lf - pa_lpos(pa_v(p, 5), PA_L2_1000, 17080);
            m += la + pa_lsize(la, lb, 0, l) - pa_lsize(0, lb, la, l);
        }
    }
    return m;
}

static int32_t pa_eq_y(int32_t m)   /* +-24 dB */
{
    return PA_H * 8 - 16 * PA_H * pa_clamp(pa_db8(m), -24 * 256, 24 * 256) / (48 * 256);
}

static void pa_eq_curve(int16_t* y, const fb_pic_t* p, int32_t lmid)
{
    int32_t x;
    for (x = 0; x < PA_W; x++)
        y[x] = (int16_t)pa_eq_y(pa_eq_l2(p, pa_col_l2f(x), lmid));
}

static void pa_eq(const fb_pic_t* p)
{
    const int32_t lmid = pa_lpos(pa_v(p, 4), 27213, 25894);
    const int32_t lf[3] = { pa_lpos(pa_v(p, 3), PA_L2_20, 19021), lmid, pa_lpos(pa_v(p, 5), PA_L2_1000, 17080) };
    int32_t i;
    pa_hspan(PA_H / 2, 0, PA_W * 16, pa_ink, 46);
    pa_eq_curve(pa_ya, p, lmid);
    pa_hatch(pa_ya, 0, PA_H * 8, 0, PA_W, 3);
    for (i = 0; i < 3; i++)
        pa_vdash((lf[i] - PA_L2_20) * PA_W / PA_L2_1000, 0, PA_H - 1, pa_ink, 102);
    pa_glow(pa_ya, 0, PA_W);
    if (p->live_n >= 1) {
        pa_eq_curve(pa_yb, p, pa_lpos(pa_live(p, 0), 27213, 25894));
        pa_dim(pa_yb, 0, PA_W, PA_GHOST);
    }
    for (i = 0; i < 3; i++)
        pa_node((lf[i] - PA_L2_20) * PA_W * 16 / PA_L2_1000, pa_eq_y(pa_eq_l2(p, lf[i], lmid)));
}

/* ==== compressor: v0 threshold (-48..0 dB), v1 ratio (1..20, log), v2 makeup (0..24 dB) */
static void pa_compressor(const fb_pic_t* p)
{
    const int32_t th = ((pa_v(p, 0) * 12288) >> 14) - 12288;                   /* dB, Q8 */
    const int32_t slope = 65536 - (int32_t)pa_exp2(-((pa_v(p, 1) * 17702) >> 14));   /* 1 - 1 / ratio, Q16 */
    const int32_t makeup = (pa_v(p, 2) * 6144) >> 14;
    int32_t x;
    pa_line(0, PA_H * 16 - 8, PA_W * 16 - 8, 0, pa_ink, 102, 1);
    pa_vdash((th + 60 * 256) * (PA_W - 1) / (60 * 256), 0, PA_H - 1, pa_ink, 102);
    for (x = 0; x < PA_W; x++) {
        const int32_t in = -60 * 256 + x * 60 * 256 / (PA_W - 1), over = in - th;
        int32_t red = 0, out;
        if (over >= 3 * 256)
            red = (over * (slope >> 4)) >> 12;
        else if (over > -3 * 256)
            red = ((over + 768) * (over + 768) / (12 * 256) * (slope >> 4)) >> 12;   /* the 6 dB knee */
        out = pa_clamp(in - red + makeup, -60 * 256, 0);
        pa_ya[x] = (int16_t)(16 * PA_H * -out / (60 * 256));
    }
    pa_hatch(pa_ya, 0, PA_H * 16, 0, PA_W, 3);
    pa_glow(pa_ya, 0, PA_W);
}

/* ==== distortion: v0 mode (clip, fold, crush), v1 drive; live: drive */
static int32_t pa_shape(int32_t mode, int32_t drive, int32_t x)   /* x Q15 -> Q15 */
{
    const int32_t ax = pa_abs(x), sg = x < 0 ? -1 : 1;
    if (mode == 1) {   /* fold: sin(x 10^drive pi / 2) * 0.6 */
        const int32_t g8 = (int32_t)(pa_exp2((drive * PA_L2_10) >> 14) >> 8);
        return (FW_SINE[(uint32_t)((x * g8) >> 15) & 1023u] * 19661) >> 15;
    }
    if (mode == 2) {   /* crush to 2^(1 + 15 (1 - drive)^2) levels */
        const int32_t a = 16384 - drive, e = 4096 + ((15 * ((a * a) >> 14)) >> 2);
        int32_t step, n;
        if (e >= 15 * 4096)
            return x;
        step = (int32_t)pa_exp2(15 * 4096 - e - 8 * 4096);   /* Q8 */
        if (step < 1)
            return x;
        n = (ax * 256 * 2 + step) / (2 * step);
        return sg * pa_min(n * step >> 8, 32767);
    }
    {   /* clip: tanh(x g) / g^0.25, g = 10^(1.5 drive) */
        const int32_t lg = (drive * 20410) >> 14;
        const uint32_t g = pa_exp2(lg), g4 = pa_exp2(lg / 4);
        const int32_t z = (int32_t)(((uint32_t)ax * (g >> 8)) >> 15);   /* Q8 */
        const int32_t i = z >> 4, t = i >= 64 ? 32767 : PA_TANH[i] + (((PA_TANH[i + 1] - PA_TANH[i]) * (z & 15)) >> 4);
        return sg * (int32_t)((uint32_t)t * 4096u / (g4 >> 4));
    }
}

static void pa_dist_curve(int16_t* y, int32_t mode, int32_t drive)
{
    const int32_t mid = PA_H * 8, amp = (PA_H / 2 - 6) * 16;
    int32_t x;
    for (x = 0; x < PA_W; x++)
        y[x] = (int16_t)(mid - (amp * pa_shape(mode, drive, FW_SINE[(x * 2048 / PA_W) & 1023]) >> 15));
}

static void pa_distortion(const fb_pic_t* p)
{
    const int32_t mode = pa_v(p, 0), mid = PA_H * 8, amp = (PA_H / 2 - 6) * 16;
    int32_t x;
    pa_hdash(PA_H / 2, 0, PA_W - 1, pa_ink, 102);
    for (x = 0; x < PA_W; x++)
        pa_yb[x] = (int16_t)(mid - (amp * FW_SINE[(x * 2048 / PA_W) & 1023] >> 15));
    pa_dist_curve(pa_ya, mode, pa_v(p, 1));
    pa_hatch(pa_ya, 0, mid, 0, PA_W, 3);
    pa_dim(pa_yb, 0, PA_W, 102);
    pa_glow(pa_ya, 0, PA_W);
    if (p->live_n >= 1) {
        pa_dist_curve(pa_yb, mode, pa_live(p, 0));
        pa_dim(pa_yb, 0, PA_W, PA_GHOST);
    }
}

/* ==== chorus: v0 rate (0.05..10 Hz, log), v1 depth; live: depth. It sweeps by itself, at its rate. */
static uint32_t pa_ch_phase, pa_ch_ms;

static void pa_chorus_tap(int16_t* y, uint32_t cycles, uint32_t phase, int32_t depth)
{
    const int32_t mid = PA_H * 8, amp = (depth * 13926) >> 14;   /* 0.85 depth */
    int32_t x;
    for (x = 0; x < PA_W; x++) {
        const uint32_t t = (uint32_t)x * cycles / (uint32_t)PA_W + phase;
        y[x] = (int16_t)(mid - ((mid * amp >> 14) * FW_SINE[(t >> 6) & 1023u] >> 15));
    }
}

static void pa_chorus(const fb_pic_t* p)
{
    const int32_t rv = pa_v(p, 0), depth = pa_v(p, 1);
    const uint32_t cycles = 32768u + (uint32_t)rv * 14u;                 /* 0.5 + 3.5 n, Q16 */
    const uint32_t rate = pa_exp2(-17702 + ((rv * 31310) >> 14));      /* Hz, Q16 */
    const uint32_t now = fm1_ms, dt = now - pa_ch_ms < 200u ? now - pa_ch_ms : 0;
    pa_ch_ms = now;
    pa_ch_phase = (pa_ch_phase + dt * rate / 1000u) & 0xFFFFu;
    pa_chorus_tap(pa_ya, cycles, pa_ch_phase, depth);
    pa_chorus_tap(pa_yb, cycles, pa_ch_phase + 16384u, depth);
    pa_hatch(pa_ya, pa_yb, 0, 0, PA_W, 4);
    pa_dim(pa_yb, 0, PA_W, 102);
    pa_glow(pa_ya, 0, PA_W);
    if (p->live_n >= 1) {
        pa_chorus_tap(pa_yb, cycles, pa_ch_phase, pa_live(p, 0));
        pa_dim(pa_yb, 0, PA_W, PA_GHOST);
    }
}

/* ---- the drawer: 1 if pic is one of part A's kinds (drawn and blitted), else 0 */
static int fb_pics_a_draw(const fb_pic_t* pic, const fb_theme_t* th)
{
    switch (pic->kind) {
    case FB_K_OSCILLATOR: case FB_K_WAVETABLE: case FB_K_FM: case FB_K_STRING: case FB_K_GRAIN:
    case FB_K_FILTER: case FB_K_EQ: case FB_K_COMPRESSOR: case FB_K_DISTORTION: case FB_K_CHORUS:
        break;
    default:
        return 0;
    }
    pa_ink = th->ink;
    pa_ground(th);
    switch (pic->kind) {
    case FB_K_OSCILLATOR: pa_oscillator(pic); break;
    case FB_K_WAVETABLE: pa_wavetable(pic); break;
    case FB_K_FM: pa_fm(pic, th); break;
    case FB_K_STRING: pa_string(pic); break;
    case FB_K_GRAIN: pa_grain(pic); break;
    case FB_K_FILTER: pa_filter(pic); break;
    case FB_K_EQ: pa_eq(pic); break;
    case FB_K_COMPRESSOR: pa_compressor(pic); break;
    case FB_K_DISTORTION: pa_distortion(pic); break;
    default: pa_chorus(pic); break;
    }
    cv_blit(FB_PIC_X, FB_PIC_Y);
    return 1;
}

/* 1 while pic moves by itself (the chorus sweeps, the grain's REC blinks): the core redraws it every frame */
static int fb_pics_a_moves(const fb_pic_t* pic)
{
    return pic->kind == FB_K_CHORUS || (pic->kind == FB_K_GRAIN && !pa_v(pic, 2));
}

#endif
