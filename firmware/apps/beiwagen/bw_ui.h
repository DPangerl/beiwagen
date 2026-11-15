/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl */
/* Beiwagen's screen kit (unity build, included once by app.c): a page is a header, a content strip and four
 * parameter rows, one per knob (KNOB1..4). Sections (HOME, GLO, SEQ...) hold pages; SELECT steps through them.
 *
 *   0..23    header: the section, the page's title, its place among the section's pages
 *   26..97   content: the page's own drawing (meters, a grid...), redrawn ~15 times a second
 *   100..239 four parameter rows of 35 px: name, value, a bar */
#ifndef BW_UI_H
#define BW_UI_H

#define UI_HEAD_H 24u
#define UI_BODY_Y 26u
#define UI_BODY_H 72u
#define UI_ROWS_Y 100u
#define UI_ROW_H 35u
#define METER_DB_MIN (-600)                 /* meters' left end, tenths of a dB */

/* a parameter: an integer in [min, max], one knob detent = step; apply() hands a new value on (may be 0); tag is
   the owner's (an effect's index, a parameter's) */
typedef struct ui_param ui_param_t;
struct ui_param {
    const char* name;
    int32_t min, max, step;
    int32_t* val;
    void (*fmt)(char* b, int32_t v, const ui_param_t* p);   /* 0: the plain number */
    void (*apply)(int32_t v, const ui_param_t* p);
    int32_t tag;
};

typedef struct {
    const char* title;
    const ui_param_t* p[4];                 /* 0: an empty row */
    void (*body)(uint32_t ms, int full);    /* the content strip; full: draw everything, else what changes */
    void (*screen)(uint32_t ms, int full);  /* set: the page draws the whole screen itself (no header, no rows) */
} ui_page_t;

typedef struct {
    const char* name;
    uint32_t button;                        /* FW_BTN_*: pressing it opens the section */
    const ui_page_t* const* pages;
    uint32_t count;
} ui_section_t;

static const ui_page_t* ui_cur;            /* the page on the screen (a body may need to know which it is) */

/* 0..32768 (32768 = 0 dB) in tenths of a dB, from -960; within about 0.1 dB */
static int32_t db10(uint32_t v)
{
    uint32_t b = 0, f;
    int32_t l;
    if (!v)
        return -960;
    while (b < 31u && (v >> (b + 1u)))
        b++;
    f = b >= 16u ? (v >> (b - 16u)) & 0xFFFFu : (v << (16u - b)) & 0xFFFFu;   /* the mantissa past 1, Q16 */
    f += ((f * (65536u - f)) >> 16) * 22u >> 6;                                /* log2(1 + f), near enough */
    l = ((int32_t)b - 15) * 65536 + (int32_t)f;                                 /* log2(v / 32768), Q16 */
    l = (l * 602 >> 16) / 10;
    return l < -960 ? -960 : l;
}

static uint16_t meter_colour(int32_t db)
{
    return db > -30 ? RGB(255, 60, 40) : db > -120 ? RGB(250, 200, 40) : RGB(30, 204, 112);
}

/* a horizontal level bar on the open canvas: peak 0..32768, w px wide at x, y */
static void ui_meter(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t peak, uint16_t on)
{
    const int32_t db = db10(peak);
    int32_t n = (db - METER_DB_MIN) * w / -METER_DB_MIN;
    n = n < 0 ? 0 : n > w ? w : n;
    cv_rect(x, y, w, h, RGB(24, 24, 30));
    cv_rect(x, y, n, h, on ? meter_colour(db) : C_GRAY);
}

static void ui_cat(char* b, const char* s, unsigned max) { str_cpy(b + str_len(b), s, max - str_len(b)); }

static void ui_cat_int(char* b, int32_t v, unsigned max)
{
    char n[12];
    fmt_int(n, v);
    ui_cat(b, n, max);
}

static void ui_header(const ui_section_t* s, uint32_t page)
{
    uint32_t i;
    cv_begin(240, UI_HEAD_H, RGB(16, 16, 22));
    cv_text(8, 4, &FONT_S, s->name, C_ACCENT);
    cv_text(8 + text_w(&FONT_S, s->name) + 10, 4, &FONT_S, s->pages[page]->title, C_WHITE);
    if (s->count > 1u)                      /* where SELECT is: a dot per page */
        for (i = 0; i < s->count; i++)
            cv_rect(232 - 8 * (int32_t)(s->count - i), 9, 5, 5, i == page ? C_HI : C_DIM);
    cv_blit(0, 0);
}

static void ui_row(uint32_t r, const ui_param_t* p, int lit)
{
    char b[20];
    const uint32_t y = UI_ROWS_Y + r * UI_ROW_H;
    static const char* const KNOB[4] = { "1", "2", "3", "4" };
    cv_begin(240, UI_ROW_H, C_BLACK);
    cv_text(8, 3, &FONT_S, KNOB[r], C_DIM);
    if (p) {
        int32_t span = p->max - p->min, w;
        if (p->fmt)
            p->fmt(b, *p->val, p);
        else
            fmt_int(b, *p->val);
        cv_text(26, 3, &FONT_S, p->name, lit ? C_WHITE : C_GRAY);
        cv_text(232 - text_w(&FONT_S, b), 3, &FONT_S, b, C_HI);
        w = span > 0 ? (*p->val - p->min) * 206 / span : 0;
        cv_rect(26, 25, 206, 4, C_DIM);
        cv_rect(26, 25, w, 4, lit ? C_ACCENT : C_GRAY);
    }
    cv_blit(0, y);
}

/* one knob's detents onto its parameter; 1 when the value moved */
static int ui_turn(const ui_param_t* p, int32_t detents)
{
    int32_t v;
    if (!p || !detents)
        return 0;
    v = *p->val + detents * p->step;
    v = v < p->min ? p->min : v > p->max ? p->max : v;
    if (v == *p->val)
        return 0;
    *p->val = v;
    if (p->apply)
        p->apply(v, p);
    return 1;
}

#endif
