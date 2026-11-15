/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl */
/* Beiwagen's effects section (GLO; unity build, included once by app.c): a page per effect of the chain (bw_fx.h),
 * its four parameters on the knobs. GLO again switches the page's effect on or off, FX bypasses the whole chain. */
#ifndef BW_FXUI_H
#define BW_FXUI_H

#include "bw_fx.h"

#define FX_N 5                              /* the chain's effects (bw_fx_count, checked in fx_ui_init) */
#define FX_STEP 20                          /* a detent on a continuous parameter: 50 across its range */

static int32_t fx_val[FX_N][BW_FX_PARAMS];
static ui_param_t fx_param[FX_N][BW_FX_PARAMS];
static ui_page_t fx_page[FX_N];
static int fx_bypass;
static const ui_page_t* const FX_PAGES[FX_N] = { &fx_page[0], &fx_page[1], &fx_page[2], &fx_page[3], &fx_page[4] };

static void fx_fmt(char* b, int32_t v, const ui_param_t* p)
{
    (void)v;
    bw_fx_format(p->tag / BW_FX_PARAMS, p->tag % BW_FX_PARAMS, b, 20);
}

static void fx_apply(int32_t v, const ui_param_t* p) { bw_fx_set(p->tag / BW_FX_PARAMS, p->tag % BW_FX_PARAMS, (int)v); }

/* the chain, each effect lit when on, the page's underlined; then the output's level */
static void fx_body(uint32_t ms, int full)
{
    static const char* const SHORT[FX_N] = { "DIST", "FILT", "CHOR", "DLY", "VERB" };
    const ui_page_t* pg = ui_cur;
    int32_t i;
    (void)ms;
    (void)full;
    cv_begin(240, UI_BODY_H, C_BLACK);
    for (i = 0; i < FX_N; i++) {
        const int32_t x = 4 + 47 * i;
        const int on = bw_fx_enabled((int)i) && !fx_bypass;
        cv_rect(x, 4, 44, 26, on ? C_ACCENT : RGB(24, 24, 30));
        cv_text(x + (44 - text_w(&FONT_S, SHORT[i])) / 2, 9, &FONT_S, SHORT[i], on ? C_WHITE : C_GRAY);
        if (pg == &fx_page[i])
            cv_rect(x, 33, 44, 3, C_HI);
    }
    cv_text(8, 42, &FONT_S, fx_bypass ? "BYPASS" : "OUT", fx_bypass ? RGB(250, 200, 40) : C_DIM);
    ui_meter(70, 46, 162, 8, out_meter[0] > out_meter[1] ? out_meter[0] : out_meter[1], 1);
    cv_blit(0, UI_BODY_Y);
}

static void fx_ui_init(void)
{
    int fx, p;
    bw_fx_init();
    for (fx = 0; fx < FX_N && fx < bw_fx_count(); fx++) {
        fx_page[fx].title = bw_fx_name(fx);
        fx_page[fx].body = fx_body;
        for (p = 0; p < BW_FX_PARAMS; p++) {
            ui_param_t* q = &fx_param[fx][p];
            const int steps = bw_fx_steps(fx, p);
            fx_val[fx][p] = bw_fx_get(fx, p);
            q->name = bw_fx_param_name(fx, p);
            q->min = 0;
            q->max = BW_FX_MAX;
            q->step = steps > 1 ? BW_FX_MAX / (steps - 1) : FX_STEP;
            q->val = &fx_val[fx][p];
            q->fmt = fx_fmt;
            q->apply = fx_apply;
            q->tag = fx * BW_FX_PARAMS + p;
            fx_page[fx].p[p] = q;
        }
    }
}

/* GLO while an effect's page shows: that effect on / off */
static void fx_toggle(const ui_page_t* pg)
{
    int fx;
    for (fx = 0; fx < FX_N; fx++)
        if (pg == &fx_page[fx])
            bw_fx_enable(fx, !bw_fx_enabled(fx));
}

#endif
