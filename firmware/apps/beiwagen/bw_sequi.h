/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl */
/* Beiwagen's SEQ and ARP sections (unity build, included once by app.c, after bw_ui.h and bw_seq.h).
 *
 * SEQ: Steps (the pattern as a roll: 16 columns, pitch upwards, the gate as the block's width, the playhead along the
 * top, the selected step marked; the knobs edit the selected step), Step (the same on knob tiles), Pattern (pattern,
 * length, channel, swing), Clock (tempo, sync, clock out). ARP: Arp (mode, rate, gate, octaves), Arp keys
 * (on, latch). The header's corner shows the pattern, the transport, the tempo and REC.
 *
 * Keys (seq_keys): arp on: they feed the arp (and do not sound themselves); REC while it plays: they go into the
 * pattern, quantised; in SEQ otherwise: step entry (the selected step takes the note, the next is selected). The UI
 * edits the selected pattern (seq_pat_next: the one that plays, or follows at the bar). */
#ifndef BW_SEQUI_H
#define BW_SEQUI_H

#include "bw_seq.h"

/* the knobs' values: the selected step's and pattern's, refreshed every frame (seq_ui_frame) */
static int32_t sq_sel, sq_note = 60, sq_vel = 100, sq_gate = 50, sq_patn, sq_len = 16, sq_ch = 1, sq_swing = 50;
static uint8_t sq_key_note[FW_KEYS];        /* what a key pressed for the arp / recording, for its release */

static seq_pattern_t* sq_edit(void) { return &seq_pat[seq_pat_next]; }

static void fmt_note(char* b, int32_t v)
{
    static const char* const N[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    str_cpy(b, N[v % 12], 4);
    ui_cat_int(b, v / 12 - 1, 8);
}

static void sq_fmt_note(char* b, int32_t v, const ui_param_t* p) { (void)p; fmt_note(b, v); }
static void sq_fmt_1(char* b, int32_t v, const ui_param_t* p) { (void)p; fmt_int(b, v + 1); }
static void sq_fmt_pct(char* b, int32_t v, const ui_param_t* p)
{
    (void)p;
    fmt_int(b, v);
    ui_cat(b, " %", 12);
}
static void sq_fmt_gate(char* b, int32_t v, const ui_param_t* p)
{
    if (v > 100)
        str_cpy(b, "Tie", 8);
    else
        sq_fmt_pct(b, v, p);
}
static void sq_fmt_onoff(char* b, int32_t v, const ui_param_t* p) { (void)p; str_cpy(b, v ? "On" : "Off", 8); }
static void sq_fmt_sync(char* b, int32_t v, const ui_param_t* p) { (void)p; str_cpy(b, v ? "Internal" : "Auto", 12); }
static void sq_fmt_mode(char* b, int32_t v, const ui_param_t* p)
{
    static const char* const M[ARP_MODES] = { "Up", "Down", "Up-down", "Random", "Played" };
    (void)p;
    str_cpy(b, M[v], 12);
}
static void sq_fmt_rate(char* b, int32_t v, const ui_param_t* p)
{
    static const char* const R[ARP_RATES] = { "1/4", "1/8", "1/8 T", "1/16", "1/16 T", "1/32", "1/32 T" };
    (void)p;
    str_cpy(b, R[v], 12);
}

/* edits: into the selected step / pattern; a note or a velocity turns the step on */
static void sq_set_step(int32_t v, const ui_param_t* p)
{
    seq_step_t* s = &sq_edit()->s[sq_sel];
    (void)v;
    if (p->tag == 1)
        s->note = (uint8_t)(sq_note | SEQ_ON);
    if (p->tag == 2) {
        s->vel = (uint8_t)(sq_vel | (s->vel & SEQ_TIE));
        s->note |= SEQ_ON;
    }
    if (p->tag == 3) {
        s->vel = (uint8_t)((s->vel & 0x7Fu) | (sq_gate > 100 ? SEQ_TIE : 0u));
        s->gate = (uint8_t)(sq_gate > 100 ? 100 : sq_gate);
    }
}

static void sq_set_pattern(int32_t v, const ui_param_t* p)
{
    seq_pattern_t* q = sq_edit();
    if (p->tag == 0)
        seq_select((uint32_t)v);
    if (p->tag == 1)
        q->len = (uint8_t)v;
    if (p->tag == 2)
        q->ch = (uint8_t)(v - 1);
    if (p->tag == 3)
        q->swing = (uint8_t)v;
}

static void sq_arp_apply(int32_t v, const ui_param_t* p) { (void)v; (void)p; seq_arp_changed(); }

static const ui_param_t P_SQ_STEP = { "Step", 0, (int32_t)SEQ_STEPS - 1, 1, &sq_sel, sq_fmt_1, 0, 0 };
static const ui_param_t P_SQ_NOTE = { "Note", 0, 127, 1, &sq_note, sq_fmt_note, sq_set_step, 1 };
static const ui_param_t P_SQ_VEL = { "Velocity", 1, 127, 1, &sq_vel, 0, sq_set_step, 2 };
static const ui_param_t P_SQ_GATE = { "Gate", 1, 101, 1, &sq_gate, sq_fmt_gate, sq_set_step, 3 };
static const ui_param_t P_SQ_PAT = { "Pattern", 0, (int32_t)SEQ_PATTERNS - 1, 1, &sq_patn, sq_fmt_1, sq_set_pattern, 0 };
static const ui_param_t P_SQ_LEN = { "Length", 1, (int32_t)SEQ_STEPS, 1, &sq_len, 0, sq_set_pattern, 1 };
static const ui_param_t P_SQ_CH = { "MIDI ch", 1, 16, 1, &sq_ch, 0, sq_set_pattern, 2 };
static const ui_param_t P_SQ_SWING = { "Swing", 50, 75, 1, &sq_swing, sq_fmt_pct, sq_set_pattern, 3 };
static const ui_param_t P_SQ_TEMPO = { "BPM", 30, 300, 1, &seq_bpm, 0, 0, 0 };
static const ui_param_t P_SQ_SYNC = { "Sync", 0, 1, 1, &seq_sync, sq_fmt_sync, 0, 0 };
static const ui_param_t P_SQ_CLKOUT = { "Clock out", 0, 1, 1, &seq_clk_out, sq_fmt_onoff, 0, 0 };
static const ui_param_t P_ARP_MODE = { "Mode", 0, ARP_MODES - 1, 1, &arp_mode, sq_fmt_mode, 0, 0 };
static const ui_param_t P_ARP_RATE = { "Rate", 0, ARP_RATES - 1, 1, &arp_rate, sq_fmt_rate, 0, 0 };
static const ui_param_t P_ARP_GATE = { "Gate", 5, 100, 5, &arp_gate, sq_fmt_pct, 0, 0 };
static const ui_param_t P_ARP_OCT = { "Octaves", 1, 4, 1, &arp_oct, 0, 0, 0 };
static const ui_param_t P_ARP_ON = { "Arp", 0, 1, 1, &arp_on, sq_fmt_onoff, sq_arp_apply, 0 };
static const ui_param_t P_ARP_LATCH = { "Latch", 0, 1, 1, &arp_latch, sq_fmt_onoff, sq_arp_apply, 0 };

/* the knobs' values from the pattern (it changes under them: step entry, recording, a pattern loaded) */
static void seq_ui_sync(void)
{
    const seq_pattern_t* q = sq_edit();
    const seq_step_t* s;
    if (sq_sel >= (int32_t)q->len)
        sq_sel = q->len - 1;
    s = &q->s[sq_sel];
    sq_note = s->note & 0x7F;
    sq_vel = s->vel & 0x7F;
    sq_gate = s->vel & SEQ_TIE ? 101 : s->gate;
    sq_patn = seq_pat_next;
    sq_len = q->len;
    sq_ch = q->ch + 1;
    sq_swing = q->swing;
}

/* ---- the header's corner: P<n>, the transport (a triangle playing, two bars armed, a square stopped), the tempo
   (EXT: from the clock), REC, ARP. Drawn when one of them changes */
#define SQ_CORNER_W 132u

static void seq_corner(uint32_t ms, int full)
{
    static uint32_t shown = 0xFFFFFFFFu;
    const int32_t t = seq_tempo10();
    const uint32_t key = (uint32_t)seq_pat_next | (uint32_t)seq_run << 5 | (uint32_t)seq_armed << 6 |
                         (uint32_t)seq_ext << 7 | (uint32_t)seq_rec << 8 | (uint32_t)(arp_on != 0) << 9 |
                         (uint32_t)((t + 5) / 10) << 10;
    char b[12];
    int32_t x = 0, i;
    (void)ms;
    if (!full && key == shown)
        return;
    shown = key;
    cv_begin(SQ_CORNER_W, UI_HEAD_H, C_BG);
    str_cpy(b, "P", sizeof b);
    ui_cat_int(b, seq_pat_next + 1, sizeof b);
    x = ui_text(x, 0, &FONT_M, b, C_TEXT, C_BG) + 8;
    if (seq_run) {
        for (i = 0; i < 10; i++)            /* a triangle */
            cv_rect(x + i, 5 + i / 2, 1, 12 - (i / 2) * 2, UI_KNOB_C[1]);
    } else if (seq_armed) {
        cv_rect(x, 5, 4, 12, C_WARN);
        cv_rect(x + 7, 5, 4, 12, C_WARN);
    } else {
        cv_rect(x, 6, 10, 10, C_SUB);
    }
    x += 16;
    if (seq_ext)
        x = ui_text(x, 3, &FONT_T, "EXT", C_WARN, C_BG) + 4;
    if (t)
        fmt_int(b, (t + 5) / 10);
    else
        str_cpy(b, "--", sizeof b);
    x = ui_text(x, 0, &FONT_M, b, C_TEXT, C_BG) + 6;
    if (seq_rec)
        cv_rect(x, 6, 10, 10, RGB(255, 50, 40));
    if (arp_on)
        ui_text((int32_t)SQ_CORNER_W - 28, 3, &FONT_T, "ARP", UI_KNOB_C[3], C_BG);
    cv_blit(240u - SQ_CORNER_W, 0);
}

/* ---- the roll: 16 columns of 15 px over the grid's area; a column redrawn when what it shows changed */
#define SQ_COL_W 15u
#define SQ_COL_H 160u                       /* the columns: y 24..183 */
#define SQ_NOTE_Y 12                        /* the notes' area in a column */
#define SQ_NOTE_H 138
#define SQ_INFO_Y (UI_GRID_Y + SQ_COL_H + 2u)   /* the selected step: y 186..215 */
#define SQ_INFO_H (UI_FOOT_Y - 2u - SQ_INFO_Y)

static uint32_t sq_col_shown[SEQ_PAGE];
static int32_t sq_lo_shown = -1, sq_span_shown;

/* the notes shown: the page's notes, at least an octave, at most what still leaves 3 px a note */
static void sq_range(const seq_pattern_t* q, uint32_t base, int32_t* lo, int32_t* span)
{
    int32_t a = 127, z = 0, n;
    uint32_t i;
    for (i = base; i < base + SEQ_PAGE && i < q->len; i++)
        if (q->s[i].note & SEQ_ON) {
            n = q->s[i].note & 0x7F;
            a = n < a ? n : a;
            z = n > z ? n : z;
        }
    if (a > z)
        a = z = 60;
    n = z - a + 1;
    if (n < 13) {
        a -= (13 - n) / 2;
        n = 13;
    }
    if (n > SQ_NOTE_H / 3)
        n = SQ_NOTE_H / 3;
    a = a < 0 ? 0 : a + n > 128 ? 128 - n : a;
    *lo = a;
    *span = n;
}

static void sq_column(uint32_t c, const seq_step_t* s, int out, int sel, int play, int32_t lo, int32_t span)
{
    const uint16_t bg = out ? C_BG : sel ? RGB(52, 52, 66) : (c >> 2) & 1u ? RGB(30, 30, 38) : C_TILE;
    const int32_t rh = SQ_NOTE_H / span;
    int32_t n;
    cv_begin(SQ_COL_W, SQ_COL_H, C_BG);
    cv_rect(0, 0, SQ_COL_W - 1, SQ_COL_H, bg);
    if (!out) {
        cv_rect(0, 0, SQ_COL_W - 1, 7, play ? UI_KNOB_C[3] : (c & 3u) == 0u ? C_SUB : ui_mix(bg, C_TEXT, 3));
        for (n = lo; n < lo + span; n++)    /* the Cs */
            if (n % 12 == 0)
                cv_rect(0, SQ_NOTE_Y + (lo + span - 1 - n) * rh + rh - 1, SQ_COL_W - 1, 1, ui_mix(bg, C_TEXT, 3));
        if (s->note & SEQ_ON) {
            const int32_t note = s->note & 0x7F, v = s->vel & 0x7F;
            const int32_t w = s->vel & SEQ_TIE ? (int32_t)SQ_COL_W : 2 + (s->gate * 12 + 50) / 100;
            int32_t y = SQ_NOTE_Y + (lo + span - 1 - note) * rh;
            y = y < SQ_NOTE_Y ? SQ_NOTE_Y : y > SQ_NOTE_Y + SQ_NOTE_H - rh ? SQ_NOTE_Y + SQ_NOTE_H - rh : y;
            cv_rect(0, y, w, rh > 3 ? rh - 1 : rh, play ? UI_KNOB_C[3] : ui_mix(bg, C_TEXT, (uint32_t)(5 + v * 10 / 127)));
        }
        if (sel)
            cv_rect(0, SQ_COL_H - 6, SQ_COL_W - 1, 6, UI_KNOB_C[0]);
    }
    cv_blit(c * SQ_COL_W, UI_GRID_Y);
}

/* the selected step under the roll, in the knobs' colours: step, note, velocity, gate */
static void sq_info(const seq_pattern_t* q, int full)
{
    static char shown[4][12];
    const seq_step_t* s = &q->s[sq_sel];
    char b[4][12];
    uint32_t i;
    int changed = full;
    str_cpy(b[0], "", 12);
    ui_cat_int(b[0], sq_sel + 1, 12);
    ui_cat(b[0], "/", 12);
    ui_cat_int(b[0], q->len, 12);
    if (s->note & SEQ_ON)
        fmt_note(b[1], s->note & 0x7F);
    else
        str_cpy(b[1], "Off", 12);
    fmt_int(b[2], s->vel & 0x7F);
    sq_fmt_gate(b[3], s->vel & SEQ_TIE ? 101 : s->gate, 0);
    for (i = 0; i < 4u; i++)
        changed |= !str_eq(b[i], shown[i]);
    if (!changed)
        return;
    cv_begin(240, SQ_INFO_H, C_BG);
    for (i = 0; i < 4u; i++) {
        const int32_t x = 60 * (int32_t)i;
        cv_rect(x + 1, 0, 58, 3, UI_KNOB_C[i]);
        ui_text(x + 30 - text_w(&FONT_M, b[i]) / 2, 6, &FONT_M, b[i], i == 1u && !(s->note & SEQ_ON) ? C_SUB : C_TEXT,
                C_BG);
        str_cpy(shown[i], b[i], 12);
    }
    cv_blit(0, SQ_INFO_Y);
}

static void seq_roll_tiles(uint32_t ms, int full)
{
    const seq_pattern_t* q = sq_edit();
    const uint32_t base = (uint32_t)sq_sel / SEQ_PAGE * SEQ_PAGE;
    const int32_t cur = seq_run && seq_pat_i == seq_pat_next ? seq_cur : -1;
    int32_t lo, span;
    uint32_t c;
    (void)ms;
    sq_range(q, base, &lo, &span);
    if (full || lo != sq_lo_shown || span != sq_span_shown) {
        full = 1;
        sq_lo_shown = lo;
        sq_span_shown = span;
    }
    if (full)
        lcd_fill(0, UI_GRID_Y + SQ_COL_H, 240, SQ_INFO_Y - UI_GRID_Y - SQ_COL_H, C_BG);
    for (c = 0; c < SEQ_PAGE; c++) {
        const uint32_t k = base + c;
        const seq_step_t* s = &q->s[k];
        const int out = k >= q->len, sel = (int32_t)k == sq_sel, play = (int32_t)k == cur;
        const uint32_t key = 1u << 31 | (uint32_t)s->note | (uint32_t)s->vel << 8 | (uint32_t)s->gate << 16 |
                             (uint32_t)out << 24 | (uint32_t)sel << 25 | (uint32_t)play << 26;
        if (!full && key == sq_col_shown[c])
            continue;
        sq_col_shown[c] = key;
        sq_column(c, s, out, sel, play, lo, span);
    }
    sq_info(q, full);
}

static const ui_page_t PG_SQ_ROLL = { "Steps", { &P_SQ_STEP, &P_SQ_NOTE, &P_SQ_VEL, &P_SQ_GATE }, seq_roll_tiles, seq_corner, 0, SQ_CORNER_W };
static const ui_page_t PG_SQ_STEP = { "Step", { &P_SQ_STEP, &P_SQ_NOTE, &P_SQ_VEL, &P_SQ_GATE }, 0, seq_corner, 0, SQ_CORNER_W };
static const ui_page_t PG_SQ_PAT = { "Pattern", { &P_SQ_PAT, &P_SQ_LEN, &P_SQ_CH, &P_SQ_SWING }, 0, seq_corner, 0, SQ_CORNER_W };
static const ui_page_t PG_SQ_CLOCK = { "Clock", { &P_SQ_TEMPO, &P_SQ_SYNC, &P_SQ_CLKOUT, 0 }, 0, seq_corner, 0, SQ_CORNER_W };
static const ui_page_t PG_ARP_MAIN = { "Arp", { &P_ARP_MODE, &P_ARP_RATE, &P_ARP_GATE, &P_ARP_OCT }, 0, seq_corner, 0, SQ_CORNER_W };
static const ui_page_t PG_ARP_KEYS = { "Arp keys", { &P_ARP_ON, &P_ARP_LATCH, 0, 0 }, 0, seq_corner, 0, SQ_CORNER_W };
static const ui_page_t* const SEQ_PAGES[] = { &PG_SQ_ROLL, &PG_SQ_STEP, &PG_SQ_PAT, &PG_SQ_CLOCK };
static const ui_page_t* const ARP_PAGES[] = { &PG_ARP_MAIN, &PG_ARP_KEYS };

/* ---- the app's calls (main loop) */

static void seq_ui_init(void)
{
    seq_init();
    seq_ui_sync();
}

static void seq_ui_frame(void) { seq_ui_sync(); }

/* the keys: arp on: to the arp; REC while playing: recorded; in SEQ (seq_shown): step entry. oct, vel, ch: the
   keyboard's (bw_keys.h: key_oct, key_vel, key_ch 1..16). Returns the keys taken: they must not sound themselves */
static uint32_t seq_keys(const fw_input_t* in, int32_t oct, int32_t vel, int32_t ch, int seq_shown)
{
    uint32_t k, taken = 0;
    for (k = 0; k < FW_KEYS; k++) {
        const uint32_t bit = FW_BIT(k);
        const int32_t n = (int32_t)FW_KEY_BASE_NOTE + (int32_t)k + 12 * oct;
        if (in->keys_released & bit) {
            if (arp_on)
                seq_arp_key(sq_key_note[k], 0, (uint32_t)(ch - 1), 0);
            seq_rec_key(sq_key_note[k], 0, 0);
            taken |= arp_on ? bit : 0u;
        }
        if (!(in->keys_pressed & bit) || n < 0 || n > 127)
            continue;
        sq_key_note[k] = (uint8_t)n;
        if (arp_on) {
            seq_arp_key((uint32_t)n, (uint32_t)vel, (uint32_t)(ch - 1), 1);
            taken |= bit;
        } else if (!(seq_rec && seq_rec_key((uint32_t)n, (uint32_t)vel, 1)) && seq_shown) {
            seq_step_t* s = &sq_edit()->s[sq_sel];  /* step entry */
            s->note = (uint8_t)(n | SEQ_ON);
            s->vel = (uint8_t)((uint32_t)vel | (s->vel & SEQ_TIE));
            sq_sel = sq_sel + 1 >= (int32_t)sq_edit()->len ? 0 : sq_sel + 1;
        }
    }
    return taken;
}

/* EDIT: the selected step on / off */
static void seq_ui_edit(void) { sq_edit()->s[sq_sel].note ^= SEQ_ON; }

static void seq_rec_toggle(void) { seq_rec = !seq_rec; }

/* ARP in the ARP section: the arp on / off */
static void seq_arp_toggle(void)
{
    arp_on = !arp_on;
    seq_arp_changed();
}

/* PLAY: lit playing, blinking armed (waiting for the clock's start); REC: lit recording */
static void seq_leds(uint32_t ms)
{
    fw_led_button(FW_BTN_PLAY, seq_run || (seq_armed && (ms >> 7 & 1u)));
    fw_led_button(FW_BTN_REC, seq_rec);
}

/* the key of the note the playing step plays (at the keyboard's octave), as a mask for fw_leds_keys */
static uint32_t seq_key_leds(int32_t oct)
{
    const int32_t c = seq_cur;
    int32_t k;
    if (!seq_run || c < 0 || !(seq_pat[seq_pat_i].s[c].note & SEQ_ON))
        return 0;
    k = (int32_t)(seq_pat[seq_pat_i].s[c].note & 0x7F) - (int32_t)FW_KEY_BASE_NOTE - 12 * oct;
    return k >= 0 && k < (int32_t)FW_KEYS ? FW_BIT((uint32_t)k) : 0u;
}

#endif
