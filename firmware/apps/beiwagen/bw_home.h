/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl */
/* Beiwagen's HOME section (unity build, included once by app.c): the overview (the source, the streams, the levels;
 * the keys' settings on the knobs), the USB diagnostics and the scope (MOOD_FM1's visualizer, avis.c). */
#ifndef BW_HOME_H
#define BW_HOME_H

#define SRC_PAIRS (FW_USB_PLAY_CH / 2)

static int32_t src_pair;                    /* the OP-1 pair heard: 0 = tape track 1 (USB mode 8) */
static volatile uint32_t out_meter[2];      /* the output's peak, 0..32767, falling ~24 dB/s (the audio ISR's) */
static uint32_t midi_seen_ms;

static void fmt_track(char* b, int32_t v, const ui_param_t* p)
{
    (void)p;
    str_cpy(b, "TRACK ", 12);
    ui_cat_int(b, v + 1, 12);
}

static void fmt_oct(char* b, int32_t v, const ui_param_t* p)
{
    (void)p;
    b[0] = 0;
    if (v > 0)
        ui_cat(b, "+", 8);
    ui_cat_int(b, v, 8);
}

static void src_apply(int32_t v, const ui_param_t* p)
{
    (void)p;
    usb_play_select((uint32_t)v);
}

static void src_step(int32_t detents)       /* ALGORITHM, on every page */
{
    int32_t v = src_pair + (detents > 0 ? 1 : detents < 0 ? -1 : 0);
    v = v < 0 ? 0 : v >= SRC_PAIRS ? SRC_PAIRS - 1 : v;
    if (v != src_pair) {
        src_pair = v;
        src_apply(v, 0);
    }
}

static const ui_param_t P_SOURCE = { "SOURCE", 0, SRC_PAIRS - 1, 1, &src_pair, fmt_track, src_apply };
static const ui_param_t P_OCTAVE = { "OCTAVE", -3, 3, 1, &key_oct, fmt_oct, 0 };
static const ui_param_t P_MIDI_CH = { "MIDI CH", 1, 16, 1, &key_ch, 0, 0 };
static const ui_param_t P_VELOCITY = { "VELOCITY", 1, 127, 1, &key_vel, 0, 0 };

/* ---- the overview */
static void overview_body(uint32_t ms, int full)
{
    static const char* const IN_TXT[3] = { "NO STREAM", "WAITING", "PLAYING" };
    static const char* const OUT_TXT[3] = { "OFF", "OPEN", "TO OP-1" };
    const int play = usb_play_state(), rec = usb_rec_state();
    char b[24];
    (void)full;
    cv_begin(240, UI_BODY_H, C_BLACK);
    str_cpy(b, "IN ", sizeof b);
    ui_cat(b, IN_TXT[play], sizeof b);
    cv_text(8, 2, &FONT_S, b, play == 2 ? C_ACCENT : C_GRAY);
    str_cpy(b, "OUT ", sizeof b);
    ui_cat(b, OUT_TXT[rec], sizeof b);
    cv_text(132, 2, &FONT_S, b, rec == 2 ? C_ACCENT : C_GRAY);
    cv_text(8, 24, &FONT_S, "L", C_DIM);
    ui_meter(26, 28, 206, 8, out_meter[0], 1);
    cv_text(8, 38, &FONT_S, "R", C_DIM);
    ui_meter(26, 42, 206, 8, out_meter[1], 1);
    str_cpy(b, "CPU ", sizeof b);
    ui_cat_int(b, (int32_t)(fw_stats.cpu_q8 * 100u >> 8), sizeof b);
    ui_cat(b, "%", sizeof b);
    cv_text(8, 54, &FONT_S, b, C_DIM);
    cv_text(132, 54, &FONT_S, "MIDI", ms - midi_seen_ms < 300u ? C_HI : C_DIM);
    cv_blit(0, UI_BODY_Y);
}

/* ---- the USB diagnostics: what the host does with the playback stream */
static void hex_put(char* b, uint32_t v, int digits)
{
    static const char H[] = "0123456789ABCDEF";
    int i;
    for (i = 0; i < digits; i++)
        b[i] = H[(v >> (4 * (digits - 1 - i))) & 0xFu];
    b[digits] = 0;
}

static void diag_line(uint32_t y, const char* s, uint16_t c)
{
    cv_begin(240, 17, C_BLACK);
    cv_text(6, 0, &FONT_S, s, c);
    cv_blit(0, y);
}

static void diag_pair(char* b, const char* a, uint32_t va, const char* z, uint32_t vz)
{
    str_cpy(b, a, 40);
    ui_cat_int(b, (int32_t)va, 40);
    ui_cat(b, z, 40);
    ui_cat_int(b, (int32_t)vz, 40);
}

static void diag_meters(uint32_t y, const uint16_t* pk, uint32_t n)
{
    static uint16_t hold[10];
    uint32_t c;
    cv_begin(240, 10u * 6u, C_BLACK);
    for (c = 0; c < n && c < 10u; c++) {
        const uint32_t m = hold[c] - (hold[c] >> 2);
        const int heard = (c >> 1) == (uint32_t)src_pair;
        hold[c] = (uint16_t)(pk[c] > m ? pk[c] : m);
        ui_meter(30, (int32_t)(6u * c), 200, 4, hold[c], heard);
        cv_rect(6 + (int32_t)(c & 1u) * 8, (int32_t)(6u * c), 6, 4, heard ? C_HI : C_DIM);
    }
    cv_blit(0, y);
}

static void diag_screen(uint32_t ms, int full)
{
    static uint32_t last_ms, last_pkts;
    fw_usb_diag_t d;
    uint16_t pk[10];
    char b[40];
    uint32_t i, n, pps, y = 2u;
    if (full) {
        lcd_fill(0, 0, 240, 240, C_BLACK);
        last_ms = ms - 100u;
    }
    if (ms - last_ms < 100u)
        return;
    usb_diag(&d);
    n = usb_play_peaks(pk);
    pps = (d.pkts - last_pkts) * 1000u / (ms - last_ms);
    last_pkts = d.pkts;
    last_ms = ms;
    diag_pair(b, "RATE ", d.rate, "  CH ", d.ch);
    diag_line(y, b, C_WHITE); y += 17u;
    diag_pair(b, "PKT/S ", pps, "  BYTES ", d.bytes);
    diag_line(y, b, pps ? C_ACCENT : C_HI); y += 17u;
    diag_pair(b, "START ", d.starts, "  DROP ", d.drops);
    diag_line(y, b, C_HI); y += 17u;
    diag_pair(b, "OVR ", d.ovr, "  DRY ", d.dry);
    diag_line(y, b, C_HI); y += 17u;
    diag_pair(b, "SETUP ", d.setups, "  ALT ", d.alt);
    diag_line(y, b, C_HI); y += 17u;
    fmt_track(b, src_pair, 0);
    diag_line(y, b, C_WHITE); y += 18u;
    diag_meters(y, pk, n);
    y += 62u;
    for (i = 0; i < 3u; i++) {                       /* newest first: type req value index, tenths of a s ago */
        const fw_usb_req_t* e = &d.req[i];
        char t[12];
        if (i >= d.nreq) {
            diag_line(y, "", C_DIM);
            y += 17u;
            continue;
        }
        hex_put(b, e->type, 2);
        b[2] = ' ';
        hex_put(b + 3, e->req, 2);
        b[5] = ' ';
        hex_put(b + 6, e->value, 4);
        b[10] = ' ';
        hex_put(b + 11, e->index, 4);
        b[15] = ' ';
        b[16] = 0;
        fmt_int(t, (int32_t)((ms - e->ms) / 100u));
        ui_cat(b, t, sizeof b);
        diag_line(y, b, i ? C_GRAY : C_WHITE);
        y += 17u;
    }
}

/* ---- the scope */
static void scope_screen(uint32_t ms, int full)
{
    if (full)
        avis_start(ms);
    else
        avis_frame(ms);
}

static const ui_page_t PG_OVERVIEW = { "OVERVIEW", { &P_SOURCE, &P_OCTAVE, &P_MIDI_CH, &P_VELOCITY }, overview_body, 0 };
static const ui_page_t PG_USB = { "USB", { 0 }, 0, diag_screen };
static const ui_page_t PG_SCOPE = { "SCOPE", { 0 }, 0, scope_screen };
static const ui_page_t* const HOME_PAGES[] = { &PG_OVERVIEW, &PG_USB, &PG_SCOPE };

#endif
