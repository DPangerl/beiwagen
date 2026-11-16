/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl */
/* The loader's screen: the Elektro Kabelmaier sign, the name of the firmware being installed and a spinner.
 * Polled LCD DMA from RAM (sys/lcd.c), so it works with interrupts off. */
#ifndef FW_NAME
#define FW_NAME "FM-1 PLAYGROUND"
#endif
#define FW_POOL
#define CV_MAX (240u * 36u)
#include "fm1_lcd_hw.h"
#include "../sys/lcd.c"
#include "../sys/gfx.c"

#define KM_NAVY RGB(0, 0, 128)
#define KM_YELLOW RGB(255, 255, 0)
#define KM_RED RGB(255, 0, 0)
#define KM_SILVER RGB(192, 192, 192)
#define KM_SHADE RGB(128, 128, 128)
#define KM_SPIN_X 20u
#define KM_SPIN_Y 184u

static void km_line(uint32_t y, uint32_t h, uint16_t bg, const felucca_font_t *f, const char *s, uint16_t c,
                    uint32_t shadow)
{
    const int32_t x = (240 - text_w(f, s)) / 2;
    cv_begin(240, h, bg);
    if (shadow)
        cv_text(x + 3, 3, f, s, KM_RED);
    cv_text(x, 0, f, s, c);
    cv_blit(0, y);
}

static void ldr_screen_init(void)
{
    const felucca_font_t *f = text_w(&FONT_L, FW_NAME) <= 216 ? &FONT_L : &FONT_S;
    lcd_init();
    lcd_fill(0, 0, 240, 88, KM_NAVY);
    km_line(10, 35, KM_NAVY, &FONT_L, "ELEKTRO", KM_YELLOW, 1);
    km_line(46, 35, KM_NAVY, &FONT_L, "KABELMAIER", KM_YELLOW, 1);
    lcd_fill(0, 88, 240, 2, C_BLACK);
    lcd_fill(0, 90, 240, 20, KM_YELLOW);
    lcd_fill(0, 110, 240, 2, C_BLACK);
    km_line(92, 16, KM_YELLOW, &FONT_S, "+++ FIRMWARE-UPDATE +++", KM_RED, 0);
    lcd_fill(0, 112, 240, 128, KM_SILVER);
    lcd_fill(8, 128, 224, 2, KM_SHADE);             /* sunken field, Windows 95 */
    lcd_fill(8, 128, 2, 44, KM_SHADE);
    lcd_fill(8, 170, 224, 2, C_WHITE);
    lcd_fill(230, 128, 2, 44, C_WHITE);
    lcd_fill(10, 130, 220, 40, C_WHITE);
    cv_begin(220, f->h, C_WHITE);
    cv_text((220 - text_w(f, FW_NAME)) / 2, 0, f, FW_NAME, C_BLACK);
    cv_blit(10, 150u - f->h / 2u);
    cv_begin(188, FONT_S.h, KM_SILVER);
    cv_text(0, 0, &FONT_S, "Bitte nicht ausstecken!", C_BLACK);
    cv_blit(KM_SPIN_X + 32u, KM_SPIN_Y + 8u);
    lcd_sync();
}

/* a rotating bar, | / - \ as on a terminal, one step every 150 ms */
static void ldr_screen_tick(void)
{
    static const int8_t END[4][4] = {{16, 4, 16, 28}, {28, 4, 4, 28}, {4, 16, 28, 16}, {4, 4, 28, 28}};
    static uint32_t last, step;
    const uint32_t now = fm1_ticks();
    const int8_t *e;
    int32_t d;
    if (fm1_lcd_timeouts || now - last < 150000u * FM1_TICKS_PER_US)   /* a panel that does not answer: no screen */
        return;
    last = now;
    e = END[step++ & 3u];
    cv_begin(32, 32, KM_SILVER);
    for (d = -1; d <= 1; d++) {
        cv_line(e[0] + d, e[1], e[2] + d, e[3], C_BLACK);
        cv_line(e[0], e[1] + d, e[2], e[3] + d, C_BLACK);
    }
    cv_blit(KM_SPIN_X, KM_SPIN_Y);
    lcd_sync();
}
