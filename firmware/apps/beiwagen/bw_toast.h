/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl */
/* A short message in the footer strip (bw_ui.h: y 218..239), over whatever the footer shows (unity build, included
 * once by app.c, after bw_ui.h). The footer asks first: while a toast is up it owns the strip, and the frame it goes
 * the footer is told to draw itself whole again. Only the strip is drawn, and only when the message changed. */
#ifndef BW_TOAST_H
#define BW_TOAST_H

#define TOAST_MS 1500u                      /* a confirmation's time on the screen */
#define C_TOAST RGB(40, 40, 50)

static char toast_txt[40];
static uint16_t toast_fg;
static uint32_t toast_until;                /* ms; 0 with toast_on: until toast_hide */
static uint8_t toast_on, toast_dirty;

/* dur 0: shown until toast_hide (a hint while a button is held) */
static void toast_show(const char* s, uint16_t fg, uint32_t now, uint32_t dur)
{
    str_cpy(toast_txt, s, sizeof toast_txt);
    toast_fg = fg;
    toast_until = dur ? (now + dur) | 1u : 0;
    toast_on = toast_dirty = 1;
}

static void toast_hide(void) { toast_until = 1u; }   /* gone at the next toast_footer */

/* the footer's first call: 1 = the toast has the strip (drawn when it changed); 0 = the footer's own, *full set on
   the frame the toast went */
static int toast_footer(uint32_t ms, int* full)
{
    const felucca_font_t* f = &FONT_M;
    if (!toast_on)
        return 0;
    if (toast_until && (int32_t)(ms - toast_until) >= 0) {
        toast_on = 0;
        *full = 1;
        return 0;
    }
    if (!toast_dirty && !*full)
        return 1;
    toast_dirty = 0;
    if (text_w(f, toast_txt) > 232)
        f = &FONT_T;
    cv_begin(240, UI_FOOT_H, C_TOAST);
    ui_text(4, ((int32_t)UI_FOOT_H - (int32_t)f->h) / 2, f, toast_txt, toast_fg, C_TOAST);
    cv_blit(0, UI_FOOT_Y);
    return 1;
}

#endif
