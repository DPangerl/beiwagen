/* SPDX-License-Identifier: GPL-3.0-only */
/* Beiwagen: the M-VAVE FM-1 as a USB sidecar for the OP-1 Field. The OP-1 (USB host, 44.1 kHz, USB mode 8) plays
 * its tape tracks into the FM-1; one pair (the source, ALGORITHM) is heard and goes back to the OP-1 as its USB
 * input; the keys play the OP-1 over USB-MIDI. The platform's USB audio is sys/usb.c (FW_USB_PLAY in, FELUCCA_UAC
 * out; build.txt).
 * From FM1_AudioBase's app (stevexfer, GPL-3.0-only). Modified for Beiwagen, 2026 (Daniel Pangerl): pages and
 * sections (bw_ui.h), HOME (bw_home.h), the keys as MIDI (bw_keys.h), the 4-track mixdown (bw_mixui.h), the
 * sequencer and arpeggiator (bw_sequi.h), presets and patterns on the keys (bw_store.h), the keys' settings kept,
 * the backlight off on buttons without a function, REC's meaning in the footer */
#include "bw_fw.h"
#include "avis.h"

#if !FW_USB_PLAY || !FELUCCA_UAC
#error "beiwagen: build.txt defines FW_USB_PLAY=1 and FELUCCA_UAC=1"
#endif

#include "bw_ui.h"
#include "bw_keys.h"
#include "bw_home.h"
#include "bw_mixui.h"
#include "bw_sequi.h"
#include "bw_toast.h"
#include "bw_store.h"

/* the audio interrupt's (the main loop only reads them, but the gain's target) */
static volatile int32_t bw_gain_target;     /* Q15: the main loop's, from MASTER */
static int32_t bw_gain;                     /* Q15: ramped to the target over a block */
static int16_t bw_in[FW_USB_PLAY_CH * FW_BLOCK];   /* every pair, pair k at bw_in + 2 * frames * k */
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

/* ---- the sequencer in TIMER5 (build.txt: APP_TICK, APP_MIDI_RT) */
static void app_tick(void) { seq_tick(); }
static void app_midi_rt(uint32_t status) { seq_midi_rt(status); }

/* ---- what the store keeps: the mixer as a preset, the pattern being edited as a pattern */
static void preset_get(void* dst) { bw_mix_save(dst, BW_MIX_BLOB); }
static void preset_set(const void* src) { bw_mix_load(src, BW_MIX_BLOB); }
static void pattern_get(void* dst) { seq_blob_get(seq_pat_next, dst); }
static void pattern_set(const void* src)
{
    seq_blob_set(seq_pat_next, src, SEQ_BLOB);
    seq_ui_sync();
}
static int seq_busy(void) { return seq_run; }   /* a flash save stalls the clock: not while the pattern plays */
static const bw_store_prov_t PROV_PRESET = { BW_MIX_BLOB, 1, preset_get, preset_set, preset_get, 0 };
static const bw_store_prov_t PROV_PATTERN = { SEQ_BLOB, 1, pattern_get, pattern_set, pattern_get, 0 };
BW_STORE_FITS(BW_MIX_BLOB);
BW_STORE_FITS(SEQ_BLOB);

static const ui_section_t SECTIONS[] = {
    { "HOME", FW_BTN_HOME, HOME_PAGES, FW_NELEM(HOME_PAGES) },
    { "MIX", FW_BTN_GLO, MIX_PAGES, FW_NELEM(MIX_PAGES) },
    { "SEQ", FW_BTN_SEQ, SEQ_PAGES, FW_NELEM(SEQ_PAGES) },
    { "ARP", FW_BTN_ARP, ARP_PAGES, FW_NELEM(ARP_PAGES) },
};
#define SECTION_COUNT FW_NELEM(SECTIONS)
#define LIGHTS_UNUSED (FW_BIT(FW_BTN_FX) | FW_BIT(FW_BTN_LFO) | FW_BIT(FW_BTN_ENV))   /* no function yet: unlit */

/* the main loop's */
static uint32_t sec, sec_page[SECTION_COUNT];
static int full_draw = 1;
static int32_t shown[4];
static uint32_t lit_until[4];               /* a knob's tile is lit until then (ms), 0: dark */
static uint32_t tiles_ms;

/* ---- the keys' MIDI channel and velocity (HOME's Keys page), in the preset bank's ext: saved once the page is left,
   not while the pattern plays (a save stalls the clock) */
static uint8_t keys_set_saved[2];

static void keys_set_load(void)
{
    uint8_t s[2];
    bw_store_ext_get(BW_BANK_PRESET, s, sizeof s);
    if (s[0] >= 1u && s[0] <= 16u && s[1] >= 1u && s[1] <= 127u) {
        key_ch = s[0];
        key_vel = s[1];
    }
    keys_set_saved[0] = (uint8_t)key_ch;
    keys_set_saved[1] = (uint8_t)key_vel;
}

static void keys_set_frame(const ui_page_t* pg, uint32_t ms)
{
    const uint8_t s[2] = { (uint8_t)key_ch, (uint8_t)key_vel };
    int r;
    if (pg == &PG_SETUP || !memcmp(s, keys_set_saved, sizeof s))
        return;
    r = bw_store_ext_save(BW_BANK_PRESET, s, sizeof s);
    if (r == BW_ST_BUSY)
        return;                             /* again once the transport stops */
    if (r != BW_ST_OK)
        bw_store_say("", "", 0, r, ms);
    memcpy(keys_set_saved, s, sizeof s);    /* (a failed save is not tried again every frame) */
}

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
    fw_lights_off(LIGHTS_UNUSED | (SECTIONS[sec].button == FW_BTN_SEQ ? 0u : FW_BIT(FW_BTN_EDIT)));
}

/* the mixer's return track (the one the OP-1 records the mix onto), -1 none */
static int32_t return_track(void)
{
    int32_t t;
    for (t = 0; t < BW_MIX_TRACKS; t++)
        if (bw_mix_get(BW_MIX_P(t, BW_MIX_RETURN)) > BW_MIX_MAX / 2)
            return t;
    return -1;
}

/* the footer: what REC does here on the left (SEQ: record; else find the track the mix goes to), the status on the right (a toast over it while one shows); drawn when
   something in it changed */
static void footer(uint32_t ms, int full)
{
    static uint32_t shown_key = 0xFFFFFFFFu, cpu_ms;
    static int32_t cpu;
    const int play = usb_play_state() == 2;
    const int32_t ret = return_track();
    const int in_seq = SECTIONS[sec].button == FW_BTN_SEQ;
    uint32_t key;
    char b[24];
    int32_t x;
    if (toast_footer(ms, &full))
        return;
    if (ms - cpu_ms >= 500u) {
        cpu = (int32_t)(fw_stats.cpu_q8 * 100u >> 8);
        cpu_ms = ms;
    }
    key = (uint32_t)(ret + 1) | (uint32_t)play << 3 | (uint32_t)in_seq << 4 | (uint32_t)seq_rec << 5 | (uint32_t)cpu << 8;
    if (!full && key == shown_key)
        return;
    shown_key = key;
    cv_begin(240, UI_FOOT_H, C_BG);
    x = 4 + ui_chip(4, 3, "REC", in_seq && seq_rec ? RGB(255, 50, 40) : C_TEXT, C_BG) + 6;
    if (in_seq) {
        str_cpy(b, seq_rec ? "recording" : "record", sizeof b);
    } else if (ret < 0) {
        str_cpy(b, "find return", sizeof b);
    } else {
        str_cpy(b, "mix to ", sizeof b);
        ui_cat_int(b, ret + 1, sizeof b);
    }
    ui_text(x, 3, &FONT_T, b, C_TEXT, C_BG);
    str_cpy(b, "CPU ", sizeof b);
    ui_cat_int(b, cpu, sizeof b);
    ui_cat(b, "%", sizeof b);
    x = 236 - text_w(&FONT_T, b);
    ui_text(x, 3, &FONT_T, b, C_SUB, C_BG);
    if (!play)
        ui_text(x - 10 - text_w(&FONT_T, "No OP-1"), 3, &FONT_T, "No OP-1", C_WARN, C_BG);
    cv_blit(0, UI_FOOT_Y);
}

static void draw(const ui_page_t* pg, const fw_input_t* in)
{
    static const uint32_t KNOB[4] = { FW_KNOB_1, FW_KNOB_2, FW_KNOB_3, FW_KNOB_4 };
    const uint32_t ms = in->ms | 1u;
    uint32_t i;
    int lit_change[4] = { 0, 0, 0, 0 };
    if (full_draw)                          /* a new page starts dark */
        for (i = 0; i < 4u; i++)
            lit_until[i] = 0;
    for (i = 0; i < 4u; i++)
        if (ui_turn(pg->p[i], in->knob[KNOB[i]])) {
            lit_change[i] = !lit_until[i];
            lit_until[i] = ms + UI_LIT_MS;
        } else if (lit_until[i] && (int32_t)(ms - lit_until[i]) >= 0) {
            lit_until[i] = 0;
            lit_change[i] = 1;
        }
    ui_cur = pg;
    if (pg->screen) {
        pg->screen(in->ms, full_draw);
        full_draw = 0;
        return;
    }
    if (full_draw) {                        /* every region over what was there, top to bottom: no clear, no flash */
        ui_header(&SECTIONS[sec], sec_page[sec]);
        if (pg->corner)
            pg->corner(in->ms, 1);
        ui_gaps();
    } else if (pg->corner) {
        pg->corner(in->ms, 0);
    }
    if (pg->tiles) {
        if (full_draw || in->ms - tiles_ms >= 33u) {
            pg->tiles(in->ms, full_draw);
            tiles_ms = in->ms;
        }
    } else {
        for (i = 0; i < 4u; i++) {          /* a tile: all of it with the page or when it lights / dims, else its value */
            const ui_param_t* p = pg->p[i];
            if (full_draw || lit_change[i] || (p && *p->val != shown[i])) {
                ui_knob_tile(i, p, lit_until[i] != 0, full_draw || lit_change[i]);
                shown[i] = p ? *p->val : 0;
            }
        }
    }
    footer(in->ms, full_draw);
    full_draw = 0;
}

static void app_init(void)
{
    keys_init();
    mix_ui_init();
    seq_ui_init();
    usb_play_enable(1);
    bw_store_init();
    bw_store_register(BW_BANK_PRESET, &PROV_PRESET);
    bw_store_register(BW_BANK_PATTERN, &PROV_PATTERN);
    bw_store_busy = seq_busy;
    bw_store_session_load();
    keys_set_load();
}

static void app_frame(const fw_input_t* in)
{
    fw_midi_t m;
    bw_gain_target = master_gain(in->master);
    if (in->redraw)
        full_draw = 1;
    while (fw_midi_read(&m))   /* (drained: a full ring would hold the host's SysEx back too) */
        ;
    {
        const uint32_t in_seq = SECTIONS[sec].button == FW_BTN_SEQ;
        fw_input_t k = *in;                 /* keys the store or the arp take do not sound themselves */
        k.keys_pressed &= ~bw_store_frame(in, in_seq ? BW_BANK_PATTERN : BW_BANK_PRESET);
        k.keys_pressed &= ~seq_keys(&k, key_oct, key_vel, key_ch, (int)in_seq);
        keys_frame(&k);
        fw_leds_keys(in->keys | seq_key_leds(key_oct));
        bw_store_leds();
        if (in->pressed & FW_BIT(FW_BTN_REC)) {   /* REC: record in SEQ, elsewhere find the return track */
            if (in_seq)
                seq_rec_toggle();
            else
                mix_detect();
        }
        if ((in->pressed & FW_BIT(FW_BTN_EDIT)) && in_seq)
            seq_ui_edit();
    }
    if (in->pressed & FW_BIT(FW_BTN_PLAY))
        seq_play();
    if ((in->pressed & FW_BIT(FW_BTN_GLO)) && SECTIONS[sec].button == FW_BTN_GLO)
        mix_toggle_mute(SECTIONS[sec].pages[sec_page[sec]]);
    if ((in->pressed & FW_BIT(FW_BTN_ARP)) && SECTIONS[sec].button == FW_BTN_ARP)
        seq_arp_toggle();
    seq_ui_frame();
    seq_leds(in->ms);
    nav(in);
    keys_set_frame(SECTIONS[sec].pages[sec_page[sec]], in->ms);
    draw(SECTIONS[sec].pages[sec_page[sec]], in);
}

/* the OP-1's four tracks (or silence) through the mixer, its scope, then the volume and the output's meters; the
   mixer sees the final output last (its return detection) */
static void app_audio(int32_t* lr, uint32_t frames)
{
    const int32_t g1 = bw_gain_target, step = (g1 - bw_gain) / (int32_t)frames;
    int32_t g = bw_gain;
    uint32_t i, pk[2] = { 0, 0 }, w = bw_scope_w;
    bw_mix_process(usb_play_begin() && usb_play_read_pairs(bw_in, frames) ? bw_in : 0, FW_USB_PLAY_CH / 2, lr, frames);
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
    bw_mix_output(lr, frames);
    bw_scope_w = w;
    for (i = 0; i < 2u; i++) {
        const uint32_t m = out_meter[i] - (out_meter[i] >> 6);
        out_meter[i] = pk[i] > m ? pk[i] : m;
    }
}
