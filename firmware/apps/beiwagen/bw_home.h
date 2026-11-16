/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl */
/* Beiwagen's HOME section (unity build, included once by app.c): the OP-1's tracks as the mixer has them (in the mix,
 * muted, the return) with their levels, the keys' settings, the USB diagnostics and the scope (MOOD_FM1's visualizer). */
#ifndef BW_HOME_H
#define BW_HOME_H

#include "bw_mix.h"

static volatile uint32_t out_meter[2];      /* the output's peak, 0..32767, falling ~24 dB/s (the audio ISR's) */

static void fmt_track(char* b, int32_t v, const ui_param_t* p)
{
    (void)p;
    str_cpy(b, "Track ", 12);
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

static const ui_param_t P_OCTAVE = { "Octave", -3, 3, 1, &key_oct, fmt_oct, 0 };
static const ui_param_t P_MIDI_CH = { "MIDI ch", 1, 16, 1, &key_ch, 0, 0 };
static const ui_param_t P_VELOCITY = { "Velocity", 1, 127, 1, &key_vel, 0, 0 };

/* ---- the overview: the OP-1's four tracks as tiles, the source lit, a level along each tile's foot. Once drawn,
   only the levels (102 x 6 each) go out again */
#define TR_LVL_Y (UI_TILE_H - 14u)

static void track_level(uint32_t t, uint32_t peak, uint16_t bg)
{
    cv_begin(UI_TILE_W - 16u, 6, bg);
    ui_meter(0, 0, (int32_t)UI_TILE_W - 16, 6, peak, ui_mix(bg, C_TEXT, 3));
    cv_blit(ui_tile_x(t) + 8u, ui_tile_y(t) + TR_LVL_Y);
}

static void track_tile(uint32_t t, uint16_t bg, uint16_t fg)
{
    char b[4] = { (char)('1' + t), 0 };
    const uint32_t x = ui_tile_x(t), y = ui_tile_y(t);
    cv_begin(UI_TILE_W, TR_LVL_Y, bg);
    ui_text(8, 6, &FONT_M, "Track", fg, bg);
    ui_text(8, 28, &FONT_V, b, fg, bg);
    cv_blit(x, y);
    lcd_fill(x, y + TR_LVL_Y, 8, UI_TILE_H - TR_LVL_Y, bg);
    lcd_fill(x + UI_TILE_W - 8u, y + TR_LVL_Y, 8, UI_TILE_H - TR_LVL_Y, bg);
    lcd_fill(x + 8u, y + TR_LVL_Y + 6u, UI_TILE_W - 16u, UI_TILE_H - TR_LVL_Y - 6u, bg);
}

/* a track's state in the mixer: lit (in the mix), dark (muted), amber (the return: the mix goes onto it) */
static uint32_t track_state(uint32_t t)
{
    if (bw_mix_get(BW_MIX_P((int)t, BW_MIX_RETURN)) > BW_MIX_MAX / 2)
        return 2u;
    return bw_mix_get(BW_MIX_P((int)t, BW_MIX_MUTE)) > BW_MIX_MAX / 2 ? 0u : 1u;
}

static void tracks_tiles(uint32_t ms, int full)
{
    static uint16_t hold[4];
    static uint32_t shown = 0xFFFFFFFFu;
    uint32_t t, state = 0;
    (void)ms;
    for (t = 0; t < 4u; t++)
        state |= track_state(t) << (2u * t);
    if (state != shown)
        full = 1;
    for (t = 0; t < 4u; t++) {
        const uint32_t st = track_state(t);
        const uint16_t bg = st == 2u ? C_WARN : st == 1u ? C_TILE_HI : C_TILE;
        const uint32_t p = (uint32_t)bw_mix_peak((int)t), m = hold[t] - (hold[t] >> 3);
        hold[t] = (uint16_t)(p > m ? p : m);
        if (full)
            track_tile(t, bg, st == 2u ? RGB(10, 10, 12) : st ? C_TEXT : C_SUB);
        track_level(t, hold[t], bg);
    }
    shown = state;
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
        const int heard = 1;
        hold[c] = (uint16_t)(pk[c] > m ? pk[c] : m);
        ui_meter(30, (int32_t)(6u * c), 200, 4, hold[c], heard ? C_TILE_HI : C_TILE);
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
    str_cpy(b, "PAIRS ", sizeof b);
    ui_cat_int(b, (int32_t)(d.ch / 2u), sizeof b);
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

static const ui_page_t PG_TRACKS = { "Tracks", { 0 }, tracks_tiles, 0, 0 };
static const ui_page_t PG_SETUP = { "Keys", { &P_OCTAVE, &P_MIDI_CH, &P_VELOCITY, 0 }, 0, 0, 0 };
static const ui_page_t PG_USB = { "USB", { 0 }, 0, 0, diag_screen };
static const ui_page_t PG_SCOPE = { "Scope", { 0 }, 0, 0, scope_screen };
static const ui_page_t* const HOME_PAGES[] = { &PG_TRACKS, &PG_SETUP, &PG_USB, &PG_SCOPE };

#endif
