/* SPDX-License-Identifier: GPL-3.0-only */
/* Beiwagen: the M-VAVE FM-1 as a USB sidecar for the OP-1 Field. The OP-1 (USB host, 44.1 kHz, USB mode 8) plays
 * its tape tracks into the FM-1; one pair (the source, ALGORITHM) is heard and goes back to the OP-1 as its USB
 * input; the keys play the OP-1 over USB-MIDI. The platform's USB audio is sys/usb.c (FW_USB_PLAY in, FELUCCA_UAC
 * out; build.txt).
 * From FM1_AudioBase's app (stevexfer, GPL-3.0-only). Modified for Beiwagen, 2026 (Daniel Pangerl): pages and
 * sections (bw_ui.h), HOME (bw_home.h), the keys as MIDI (bw_keys.h), the source pair, the effects (bw_fxui.h) */
#include "bw_fw.h"
#include "avis.h"

#if !FW_USB_PLAY || !FELUCCA_UAC
#error "beiwagen: build.txt defines FW_USB_PLAY=1 and FELUCCA_UAC=1"
#endif

#include "bw_ui.h"
#include "bw_keys.h"
#include "bw_home.h"
#include "bw_fxui.h"

/* the audio interrupt's (the main loop only reads them, but the gain's target) */
static volatile int32_t bw_gain_target;     /* Q15: the main loop's, from MASTER */
static int32_t bw_gain;                     /* Q15: ramped to the target over a block */
static int16_t bw_in[2u * FW_BLOCK];
static int16_t bw_scope[BW_SCOPE];
static volatile uint32_t bw_scope_w;

/* ---- for avis.c (bw_fw.h) */
uint16_t* ui_fb(void) { return cv_px; }
void ui_lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t* px) { lcd_blit(x, y, w, h, px); }
void ui_lcd_sync(void) { lcd_sync(); }
void ui_lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { lcd_fill(x, y, w, h, c); }

void bw_scope_read(int16_t* dst, int n)
{
    const uint32_t w = bw_scope_w;
    int i;
    for (i = 0; i < n; i++)
        dst[i] = bw_scope[(w - (uint32_t)n + (uint32_t)i) & (BW_SCOPE - 1u)];
}

/* ---- sections still to be built: a page that says so */
static void planned_body(uint32_t ms, int full)
{
    (void)ms;
    if (!full)
        return;
    cv_begin(240, UI_BODY_H, C_BLACK);
    cv_text(8, 26, &FONT_S, "NOT BUILT YET", C_DIM);
    cv_blit(0, UI_BODY_Y);
}

static const ui_page_t PG_SEQ = { "PATTERN", { 0 }, planned_body, 0 };
static const ui_page_t PG_ARP = { "ARP", { 0 }, planned_body, 0 };
static const ui_page_t PG_LFO = { "LFO", { 0 }, planned_body, 0 };
static const ui_page_t PG_ENV = { "FOLLOWER", { 0 }, planned_body, 0 };
static const ui_page_t* const SEQ_PAGES[] = { &PG_SEQ };
static const ui_page_t* const ARP_PAGES[] = { &PG_ARP };
static const ui_page_t* const LFO_PAGES[] = { &PG_LFO };
static const ui_page_t* const ENV_PAGES[] = { &PG_ENV };

static const ui_section_t SECTIONS[] = {
    { "HOME", FW_BTN_HOME, HOME_PAGES, FW_NELEM(HOME_PAGES) },
    { "FX", FW_BTN_GLO, FX_PAGES, FW_NELEM(FX_PAGES) },
    { "SEQ", FW_BTN_SEQ, SEQ_PAGES, FW_NELEM(SEQ_PAGES) },
    { "ARP", FW_BTN_ARP, ARP_PAGES, FW_NELEM(ARP_PAGES) },
    { "LFO", FW_BTN_LFO, LFO_PAGES, FW_NELEM(LFO_PAGES) },
    { "ENV", FW_BTN_ENV, ENV_PAGES, FW_NELEM(ENV_PAGES) },
};
#define SECTION_COUNT FW_NELEM(SECTIONS)

/* the main loop's */
static uint32_t sec, sec_page[SECTION_COUNT];
static int full_draw = 1;
static int32_t shown[4];
static uint32_t body_ms;

/* MASTER (0..1023) as a gain, Q15: squared, so the knob's travel is near even in dB; the bottom is silence */
static int32_t master_gain(uint32_t m)
{
    return m < 4u ? 0 : (int32_t)(m * m >> 5);
}

static void nav(const fw_input_t* in)
{
    uint32_t i;
    for (i = 0; i < SECTION_COUNT; i++)
        if ((in->pressed & FW_BIT(SECTIONS[i].button)) && i != sec) {
            sec = i;
            full_draw = 1;
        }
    if (in->knob[FW_KNOB_SELECT]) {
        const int32_t n = (int32_t)SECTIONS[sec].count;
        int32_t p = (int32_t)sec_page[sec] + (in->knob[FW_KNOB_SELECT] > 0 ? 1 : -1);
        p = p < 0 ? 0 : p >= n ? n - 1 : p;
        if ((uint32_t)p != sec_page[sec]) {
            sec_page[sec] = (uint32_t)p;
            full_draw = 1;
        }
    }
    for (i = 0; i < SECTION_COUNT; i++)
        fw_led_button(SECTIONS[i].button, i == sec);
}

static void draw(const ui_page_t* pg, const fw_input_t* in)
{
    static const uint32_t KNOB[4] = { FW_KNOB_1, FW_KNOB_2, FW_KNOB_3, FW_KNOB_4 };
    uint32_t r;
    for (r = 0; r < 4u; r++)
        ui_turn(pg->p[r], in->knob[KNOB[r]]);
    ui_cur = pg;
    if (pg->screen) {
        pg->screen(in->ms, full_draw);
        full_draw = 0;
        return;
    }
    if (full_draw) {
        lcd_fill(0, 0, 240, 240, C_BLACK);
        ui_header(&SECTIONS[sec], sec_page[sec]);
    }
    for (r = 0; r < 4u; r++)                /* a row is drawn when its value moved, by a knob or elsewhere */
        if (full_draw || (pg->p[r] && *pg->p[r]->val != shown[r])) {
            ui_row(r, pg->p[r], pg->p[r] != 0);
            shown[r] = pg->p[r] ? *pg->p[r]->val : 0;
        }
    if (pg->body && (full_draw || in->ms - body_ms >= 66u)) {
        pg->body(in->ms, full_draw);
        body_ms = in->ms;
    }
    full_draw = 0;
}

static void app_init(void)
{
    keys_init();
    fx_ui_init();
    usb_play_enable(1);
    usb_play_select((uint32_t)src_pair);
}

static void app_frame(const fw_input_t* in)
{
    fw_midi_t m;
    bw_gain_target = master_gain(in->master);
    while (fw_midi_read(&m))   /* (drained: a full ring would hold the host's SysEx back too) */
        midi_seen_ms = in->ms;
    keys_frame(in);
    src_step(in->knob[FW_KNOB_ALGO]);
    if (in->pressed & FW_BIT(FW_BTN_FX)) {
        fx_bypass = !fx_bypass;
        bw_fx_bypass(fx_bypass);
    }
    if ((in->pressed & FW_BIT(FW_BTN_GLO)) && SECTIONS[sec].button == FW_BTN_GLO)
        fx_toggle(SECTIONS[sec].pages[sec_page[sec]]);
    fw_led_button(FW_BTN_FX, fx_bypass);
    fw_led_button(FW_BTN_PLAY, usb_play_state() == 2);
    fw_led_button(FW_BTN_REC, usb_rec_state() == 2);
    nav(in);
    draw(SECTIONS[sec].pages[sec_page[sec]], in);
}

/* the OP-1's pair (or silence) through the effects, its scope, then the volume and the output's meters */
static void app_audio(int32_t* lr, uint32_t frames)
{
    const int32_t g1 = bw_gain_target, step = (g1 - bw_gain) / (int32_t)frames;
    int32_t g = bw_gain;
    uint32_t i, pk[2] = { 0, 0 }, w = bw_scope_w;
    if (!(usb_play_begin() && usb_play_read(bw_in, frames)))
        memset(bw_in, 0, frames * 2u * sizeof bw_in[0]);
    for (i = 0; i < 2u * frames; i++)
        lr[i] = bw_in[i];
    bw_fx_process(lr, frames);
    for (i = 0; i < frames; i++) {
        const int32_t l = lr[2u * i], r = lr[2u * i + 1u];
        int32_t ol, or_;
        bw_scope[w++ & (BW_SCOPE - 1u)] = (int16_t)((l + r) >> 1);
        g += step;
        ol = l * g >> 15;
        or_ = r * g >> 15;
        lr[2u * i] = ol;
        lr[2u * i + 1u] = or_;
        ol = ol < 0 ? -ol : ol;
        or_ = or_ < 0 ? -or_ : or_;
        pk[0] = (uint32_t)ol > pk[0] ? (uint32_t)ol : pk[0];
        pk[1] = (uint32_t)or_ > pk[1] ? (uint32_t)or_ : pk[1];
    }
    bw_gain = g1;
    bw_scope_w = w;
    for (i = 0; i < 2u; i++) {
        const uint32_t m = out_meter[i] - (out_meter[i] >> 6);
        out_meter[i] = pk[i] > m ? pk[i] : m;
    }
}
