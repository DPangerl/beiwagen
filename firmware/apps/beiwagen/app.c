/* SPDX-License-Identifier: GPL-3.0-only */
/* Beiwagen: the M-VAVE FM-1 as a USB sidecar for the OP-1 Field. The OP-1 (USB host, 44.1 kHz, USB mode 8) plays
 * its tape tracks into the FM-1; one pair is heard, and goes back to the OP-1 as its USB input. The platform's USB
 * audio is sys/usb.c (FW_USB_PLAY in, FELUCCA_UAC out; build.txt).
 * From FM1_AudioBase's app (stevexfer, GPL-3.0-only). Modified for Beiwagen, 2026 (Daniel Pangerl): a diagnostics
 * page (the USB playback's counters, its channels' meters, the host's last requests), the pair heard (KNOB1) */
#include "bw_fw.h"
#include "avis.h"

#if !FW_USB_PLAY || !FELUCCA_UAC
#error "beiwagen: build.txt defines FW_USB_PLAY=1 and FELUCCA_UAC=1"
#endif

enum { PAGE_MAIN, PAGE_VIS, PAGE_DIAG, PAGE_COUNT }; /* GLO steps through them */

#define METER_W 180
#define METER_DB_MIN (-600)                 /* the meters' left end, tenths of a dB */

/* the audio interrupt's (the main loop only reads them, but the gain's target) */
static volatile int32_t bw_gain_target;     /* Q15: the main loop's, from MASTER */
static int32_t bw_gain;                     /* Q15: ramped to the target over a block */
static int16_t bw_in[2u * FW_BLOCK];
static int16_t bw_scope[BW_SCOPE];
static volatile uint32_t bw_scope_w;
static volatile uint32_t bw_meter[2];       /* the peak before the volume, 0..32767, falling ~24 dB/s */

/* the main loop's */
static int page = PAGE_MAIN;
static uint32_t dirty = 1;
static int shown_play = -1, shown_rec = -1;
static int32_t shown_gain = -1;
static uint32_t midi_seen_ms, status_ms;
static int play_pair;                       /* the USB channel pair heard (KNOB1) */

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

/* MASTER (0..1023) as a gain, Q15: squared, so the knob's travel is near even in dB; the bottom is silence */
static int32_t master_gain(uint32_t m)
{
    return m < 4u ? 0 : (int32_t)(m * m >> 5);
}

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

/* ---- the main page */
static void draw_row(uint32_t y, const char* label, const char* value, uint16_t vc)
{
    cv_begin(240, 18, C_BLACK);
    cv_text(12, 1, &FONT_S, label, C_GRAY);
    cv_text(228 - text_w(&FONT_S, value), 1, &FONT_S, value, vc);
    cv_blit(0, y);
}

static uint16_t meter_colour(int32_t db)
{
    return db > -30 ? RGB(255, 60, 40) : db > -120 ? RGB(250, 200, 40) : RGB(30, 204, 112);
}

static void draw_meters(void)
{
    uint32_t ch;
    cv_begin(240, 26, C_BLACK);
    for (ch = 0; ch < 2u; ch++) {
        const int32_t db = db10(bw_meter[ch]);
        const int32_t y = 3 + 12 * (int32_t)ch;
        int32_t w = (db - METER_DB_MIN) * METER_W / -METER_DB_MIN;
        w = w < 0 ? 0 : w > METER_W ? METER_W : w;
        cv_text(12, y - 3, &FONT_S, ch ? "R" : "L", C_DIM);
        cv_rect(36, y, METER_W, 8, RGB(24, 24, 30));
        cv_rect(36, y, w, 8, meter_colour(db));
    }
    cv_blit(0, 62);
}

static void draw_volume(int32_t gain)
{
    char b[16];
    const int32_t db = db10((uint32_t)gain);
    if (gain)
        fmt_fix(b, db, 1);
    else
        str_cpy(b, "OFF", sizeof b);
    if (gain)
        str_cpy(b + str_len(b), " dB", sizeof b - str_len(b));
    cv_begin(240, 34, C_BLACK);
    cv_text(12, 1, &FONT_S, "VOLUME", C_GRAY);
    cv_text(228 - text_w(&FONT_S, b), 1, &FONT_S, b, C_HI);
    cv_rect(12, 24, 216, 6, C_DIM);
    cv_rect(12, 24, gain * 216 / 32768, 6, C_ACCENT);
    cv_blit(0, 140);
}

static void draw_status(void)
{
    char b[16], n[8];
    const int midi = fm1_ms - midi_seen_ms < 300u;
    cv_begin(240, 18, C_BLACK);
    fmt_int(n, (int32_t)(fw_stats.cpu_q8 * 100u >> 8));
    str_cpy(b, "CPU ", sizeof b);
    str_cpy(b + 4, n, sizeof b - 4);
    cv_text(12, 1, &FONT_S, b, C_GRAY);
    cv_text(110, 1, &FONT_S, fw_stats.usb_up ? "USB" : "---", fw_stats.usb_up ? C_ACCENT : C_DIM);
    cv_text(170, 1, &FONT_S, "MIDI", midi ? C_HI : C_DIM);
    cv_blit(0, 222);
}

static void draw_main(int play, int rec, int32_t gain)
{
    static const char* const PLAY_TXT[3] = { "OFF", "WAITING", "PLAYING" };
    static const char* const REC_TXT[3] = { "OFF", "OPEN", "RECORDING" };
    if (dirty) {
        lcd_fill(0, 0, 240, 240, C_BLACK);
        draw_text_box(0, 10, 240, &FONT_S, FW_NAME, C_WHITE, 1);
        draw_text_box(0, 196, 240, &FONT_S, "GLO: VISUALIZER", C_DIM, 1);
        shown_play = shown_rec = -1;
        shown_gain = -1;
        status_ms = 0;
        dirty = 0;
    }
    if (play != shown_play) {
        draw_row(40, "USB IN", PLAY_TXT[play], play == 2 ? C_ACCENT : C_HI);
        shown_play = play;
    }
    if (rec != shown_rec) {
        draw_row(104, "USB OUT", REC_TXT[rec], rec == 2 ? C_ACCENT : C_HI);
        shown_rec = rec;
    }
    if (gain != shown_gain) {
        draw_volume(gain);
        shown_gain = gain;
    }
    draw_meters();
    if (fm1_ms - status_ms >= 250u || !status_ms) {
        draw_status();
        status_ms = fm1_ms | 1u;
    }
}

/* ---- the diagnostics page: what a host does with the playback stream */
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
    char n[12];
    str_cpy(b, a, 40);
    fmt_int(n, (int32_t)va);
    str_cpy(b + str_len(b), n, 40 - str_len(b));
    str_cpy(b + str_len(b), z, 40 - str_len(b));
    fmt_int(n, (int32_t)vz);
    str_cpy(b + str_len(b), n, 40 - str_len(b));
}

static void draw_ch_meters(uint32_t y, const uint16_t* pk, uint32_t n)
{
    static uint16_t hold[10];
    uint32_t c;
    cv_begin(240, 10u * 6u, C_BLACK);
    for (c = 0; c < n && c < 10u; c++) {
        const uint32_t m = hold[c] - (hold[c] >> 2);
        int32_t db, w;
        hold[c] = (uint16_t)(pk[c] > m ? pk[c] : m);
        db = db10(hold[c]);
        w = (db - METER_DB_MIN) * 200 / -METER_DB_MIN;
        w = w < 0 ? 0 : w > 200 ? 200 : w;
        cv_rect(30, (int32_t)(6u * c), 200, 4, RGB(24, 24, 30));
        cv_rect(30, (int32_t)(6u * c), w, 4, (c >> 1) == (uint32_t)play_pair ? C_ACCENT : C_GRAY);
        cv_rect(6 + (int32_t)(c & 1u) * 8, (int32_t)(6u * c), 6, 4, (c >> 1) == (uint32_t)play_pair ? C_HI : C_DIM);
    }
    cv_blit(0, y);
}

static void draw_diag(uint32_t ms)
{
    static uint32_t last_ms, last_pkts, pps;
    fw_usb_diag_t d;
    uint16_t pk[10];
    char b[40];
    uint32_t i, n, y = 2u;
    if (dirty) {
        lcd_fill(0, 0, 240, 240, C_BLACK);
        dirty = 0;
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
    diag_pair(b, "HEARD CH ", 2u * (uint32_t)play_pair + 1u, "-", 2u * (uint32_t)play_pair + 2u);
    str_cpy(b + str_len(b), "  KNOB1", sizeof b - str_len(b));
    diag_line(y, b, C_WHITE); y += 18u;
    draw_ch_meters(y, pk, n);
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
        str_cpy(b + 16, t, sizeof b - 16);
        diag_line(y, b, i ? C_GRAY : C_WHITE);
        y += 17u;
    }
}

static void app_init(void)
{
    usb_play_enable(1);
}

static void app_frame(const fw_input_t* in)
{
    const int play = usb_play_state(), rec = usb_rec_state();
    const int32_t gain = master_gain(in->master);
    fw_midi_t m;
    bw_gain_target = gain;
    if (in->knob[FW_KNOB_1]) {
        play_pair += in->knob[FW_KNOB_1] > 0 ? 1 : -1;
        play_pair = play_pair < 0 ? 0 : play_pair > 4 ? 4 : play_pair;
        usb_play_select((uint32_t)play_pair);
    }
    while (fw_midi_read(&m))   /* (drained: a full ring would hold the host's SysEx back too) */
        midi_seen_ms = in->ms;
    if (in->pressed & FW_BIT(FW_BTN_GLO)) {
        page = (page + 1) % PAGE_COUNT;
        if (page == PAGE_VIS)
            avis_start(in->ms);
        else
            dirty = 1;
    }
    fw_led_button(FW_BTN_GLO, page == PAGE_VIS);
    fw_led_button(FW_BTN_PLAY, play == 2);
    fw_led_button(FW_BTN_REC, rec == 2);
    if (page == PAGE_VIS)
        avis_frame(in->ms);
    else if (page == PAGE_DIAG)
        draw_diag(in->ms);
    else
        draw_main(play, rec, gain);
}

/* the computer's audio (or silence), its scope and meters, then the volume */
static void app_audio(int32_t* lr, uint32_t frames)
{
    const int32_t g1 = bw_gain_target, step = (g1 - bw_gain) / (int32_t)frames;
    int32_t g = bw_gain;
    uint32_t i, pk[2] = { 0, 0 }, w = bw_scope_w;
    if (!(usb_play_begin() && usb_play_read(bw_in, frames)))
        memset(bw_in, 0, frames * 2u * sizeof bw_in[0]);
    for (i = 0; i < frames; i++) {
        const int32_t l = bw_in[2u * i], r = bw_in[2u * i + 1u];
        const uint32_t al = (uint32_t)(l < 0 ? -l : l), ar = (uint32_t)(r < 0 ? -r : r);
        pk[0] = al > pk[0] ? al : pk[0];
        pk[1] = ar > pk[1] ? ar : pk[1];
        bw_scope[w++ & (BW_SCOPE - 1u)] = (int16_t)((l + r) >> 1);
        g += step;
        lr[2u * i] = l * g >> 15;
        lr[2u * i + 1u] = r * g >> 15;
    }
    bw_gain = g1;
    bw_scope_w = w;
    for (i = 0; i < 2u; i++) {
        const uint32_t m = bw_meter[i] - (bw_meter[i] >> 6);
        bw_meter[i] = pk[i] > m ? pk[i] : m;
    }
}
