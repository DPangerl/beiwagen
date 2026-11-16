/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl */
/* Beiwagen's mixer pages (unity build, included once by app.c, after bw_ui.h): the mixdown engine's (bw_mix.h)
 * parameters on the knobs. Per track two pages, "Track N" (level, pan, the sends) and "Track N tone" (the insert),
 * then the shared delay and reverb and the master. The header's corner shows the four tracks: white with signal in
 * the mix, grey in the mix and quiet, dark muted, amber the return (out of the mix); the page's track underlined;
 * "Finding" while the return is searched, "Not found" a moment after a failed search. Drawn with the page, then only
 * when one of those changes. */
#ifndef BW_MIXUI_H
#define BW_MIXUI_H

#include "bw_mix.h"

#define MIX_STEP 20                         /* a detent on a continuous parameter: 50 across its range */
#define MIX_PAGES_N (2 * BW_MIX_TRACKS + 3)
#define MIX_CORNER_W 92u
#define MIX_SIGNAL 100                      /* a track has signal over this peak (about -50 dBFS) */

static int32_t mix_val[BW_MIX_PARAMS];
static ui_param_t mix_param[BW_MIX_PARAMS];
static ui_page_t mix_page[MIX_PAGES_N];
static char mix_title[2 * BW_MIX_TRACKS][16];
static const ui_page_t* const MIX_PAGES[MIX_PAGES_N] = {
    &mix_page[0], &mix_page[1], &mix_page[2], &mix_page[3], &mix_page[4], &mix_page[5],
    &mix_page[6], &mix_page[7], &mix_page[8], &mix_page[9], &mix_page[10],
};

static void mix_fmt(char* b, int32_t v, const ui_param_t* p)
{
    (void)v;
    bw_mix_format((int)p->tag, b, 20);
}

static void mix_apply(int32_t v, const ui_param_t* p) { bw_mix_set((int)p->tag, (int)v); }

/* the track a page belongs to, -1: a shared page */
static int32_t mix_page_track(const ui_page_t* pg)
{
    int32_t i;
    for (i = 0; i < 2 * BW_MIX_TRACKS; i++)
        if (pg == &mix_page[i])
            return i >> 1;
    return -1;
}

static void mix_corner(uint32_t ms, int full)
{
    static uint32_t shown = 0xFFFFFFFFu, signal_until[BW_MIX_TRACKS], fail_ms;
    static int last_state;
    const int state = bw_mix_detect_state();
    const int32_t cur = mix_page_track(ui_cur);
    uint32_t key, t;
    int32_t p;
    for (p = 0; p < BW_MIX_PARAMS; p++)         /* the knobs follow what changed elsewhere (detection, a preset) */
        mix_val[p] = bw_mix_get((int)p);
    if (state == BW_MIX_DETECT_FAILED && last_state != BW_MIX_DETECT_FAILED)
        fail_ms = ms | 1u;
    last_state = state;
    if (fail_ms && (int32_t)(ms - fail_ms) >= 2000)
        fail_ms = 0;
    key = (uint32_t)(cur + 1) | (state == BW_MIX_DETECT_RUNNING ? 1u : 0u) << 3 | (fail_ms ? 1u : 0u) << 4;
    for (t = 0; t < BW_MIX_TRACKS; t++) {
        const uint32_t ret = mix_val[BW_MIX_P(t, BW_MIX_RETURN)] != 0;
        const uint32_t mute = mix_val[BW_MIX_P(t, BW_MIX_MUTE)] != 0;
        if (bw_mix_peak((int)t) > MIX_SIGNAL)
            signal_until[t] = ms + 300u;
        key |= (ret << 2 | mute << 1 | (uint32_t)((int32_t)(signal_until[t] - ms) > 0)) << (5u + 3u * t);
    }
    if (!full && key == shown)
        return;
    shown = key;
    cv_begin(MIX_CORNER_W, UI_HEAD_H, C_BG);
    if (state == BW_MIX_DETECT_RUNNING || fail_ms) {
        const char* s = fail_ms ? "Not found" : "Finding";
        ui_text((int32_t)MIX_CORNER_W - 2 - text_w(&FONT_T, s), 3, &FONT_T, s, C_WARN, C_BG);
    } else {
        for (t = 0; t < BW_MIX_TRACKS; t++) {
            const uint32_t bits = key >> (5u + 3u * t);
            const uint16_t bg = (bits & 4u) ? C_WARN : (bits & 2u) ? C_TILE : (bits & 1u) ? C_TEXT : C_TILE_HI;
            const uint16_t fg = (bits & 4u) || (bits & 7u) == 1u ? RGB(10, 10, 12) : (bits & 2u) ? C_SUB : C_TEXT;
            const int32_t x = 2 + 23 * (int32_t)t;
            char d[2] = { (char)('1' + t), 0 };
            cv_rect(x, 1, 20, 16, bg);
            ui_text(x + 10 - text_w(&FONT_T, d) / 2, 0, &FONT_T, d, fg, bg);
            if ((int32_t)t == cur)
                cv_rect(x, 19, 20, 2, C_TEXT);
        }
    }
    cv_blit(240u - MIX_CORNER_W, 0);
}

static void mix_page_set(uint32_t i, const char* title, int p0, int p1, int p2, int p3)
{
    const int ps[4] = { p0, p1, p2, p3 };
    int k;
    mix_page[i].title = title;
    mix_page[i].corner = mix_corner;
    mix_page[i].corner_w = MIX_CORNER_W;
    for (k = 0; k < 4; k++)
        mix_page[i].p[k] = ps[k] >= 0 ? &mix_param[ps[k]] : 0;
}

static void mix_ui_init(void)
{
    int p, t;
    bw_mix_init();
    for (p = 0; p < BW_MIX_PARAMS; p++) {
        ui_param_t* q = &mix_param[p];
        const int steps = bw_mix_steps(p);
        mix_val[p] = bw_mix_get(p);
        q->name = bw_mix_param_name(p);
        q->min = 0;
        q->max = BW_MIX_MAX;
        q->step = steps > 1 ? BW_MIX_MAX / (steps - 1) : MIX_STEP;
        q->val = &mix_val[p];
        q->fmt = mix_fmt;
        q->apply = mix_apply;
        q->tag = p;
    }
    for (t = 0; t < BW_MIX_TRACKS; t++) {
        char* a = mix_title[2 * t];
        char* b = mix_title[2 * t + 1];
        str_cpy(a, "Track ", 16);
        ui_cat_int(a, t + 1, 16);
        str_cpy(b, a, 16);
        ui_cat(b, " tone", 16);
        mix_page_set(2u * (uint32_t)t, a, BW_MIX_P(t, BW_MIX_LEVEL), BW_MIX_P(t, BW_MIX_PAN),
                     BW_MIX_P(t, BW_MIX_SEND_DELAY), BW_MIX_P(t, BW_MIX_SEND_REVERB));
        mix_page_set(2u * (uint32_t)t + 1u, b, BW_MIX_P(t, BW_MIX_CUTOFF), BW_MIX_P(t, BW_MIX_RESO),
                     BW_MIX_P(t, BW_MIX_DRIVE), BW_MIX_P(t, BW_MIX_MODE));
    }
    mix_page_set(8, "Delay", BW_MIX_DELAY_TIME, BW_MIX_DELAY_FEEDBACK, BW_MIX_DELAY_TONE, BW_MIX_DELAY_RETURN);
    mix_page_set(9, "Reverb", BW_MIX_REVERB_SIZE, BW_MIX_REVERB_DECAY, BW_MIX_REVERB_DAMPING, BW_MIX_REVERB_RETURN);
    mix_page_set(10, "Master", BW_MIX_MASTER, BW_MIX_INSERTS, -1, -1);
}

/* the track on the page muted / heard again (a shared page: nothing); 1 when a track was switched */
static int mix_toggle_mute(const ui_page_t* pg)
{
    const int32_t t = mix_page_track(pg);
    if (t < 0)
        return 0;
    bw_mix_set(BW_MIX_P(t, BW_MIX_MUTE), bw_mix_get(BW_MIX_P(t, BW_MIX_MUTE)) ? 0 : BW_MIX_MAX);
    return 1;
}

/* find the return track (the corner shows it; bw_mix_detect_state() for the result) */
static void mix_detect(void) { bw_mix_detect(); }

#endif
