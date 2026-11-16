/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl */
/* The system settings, the same in every app: hold HOME and press EDIT. SELECT picks a setting, ALGORITHM changes it,
 * HOME or EDIT closes (and saves, when something changed). While open the app runs on (audio, MIDI, the keys), but
 * its drawing is held back (main.c: lcd_muted) and it sees no buttons or knobs; buttons the settings took stay
 * hidden from it until they are let go. On closing the app gets in->redraw.
 *   LIGHTS   the buttons' backlight: off, 1..8 (an app takes it off buttons that do nothing: fw_lights_off)
 *   THEME    sync (the app on the other end decides), light, dark: fw_theme(); only where the app has a light
 *            theme (build.txt: define FW_SET_THEME=1)
 * Stored in its own sector, 0xFB000 (hal/fm1_flash.h FL_SYS_LO), outside every app's store: one copy, a torn
 * write reads as the defaults. */
#ifndef FW_SET_THEME
#define FW_SET_THEME 0
#endif
#define SET_ADDR FL_SYS_LO
#define SET_MAGIC 0x31544553u                     /* "SET1" */
#define SET_LIGHTS_MAX 8u
#define SET_LIGHTS_DEFAULT 4u
#define SET_ROWS (1u + FW_SET_THEME)
#define SET_ROW_Y(r) (52u + (r) * 80u)

typedef struct {
    uint32_t magic;
    uint8_t lights, theme, rsv[2];                /* lights: 0 off, 1..SET_LIGHTS_MAX; theme: FW_THEME_* */
    uint32_t crc;
} fw_set_t;

/* the button backlight's pulse a scan frame (ns), per level: about evenly spaced to the eye */
static const uint16_t SET_LIGHTS_NS[SET_LIGHTS_MAX + 1u] = {0, 300, 450, 700, 1000, 1500, 2100, 3000, 4000};
static const char *const SET_THEME_NAME[3] = {"SYNC", "LIGHT", "DARK"};

static fw_set_t fw_set, fw_set_stored, fw_set_shown;
static uint8_t fw_set_open, fw_set_row, fw_set_full;
static uint32_t fw_set_swallow;                     /* buttons (labels) the settings took, until let go */
static uint32_t fw_set_dark = ~0u;                  /* buttons (labels) without the backlight */

static uint32_t fw_theme(void) { return FW_SET_THEME ? fw_set.theme : FW_THEME_SYNC; }

static uint32_t fw_set_crc(const fw_set_t *s) { return st_crc32(s, sizeof *s - sizeof s->crc); }

static void fw_set_apply(void) { fm1_led_bg_ns = SET_LIGHTS_NS[fw_set.lights]; }

static void fw_settings_init(void)                  /* after fm1_input_init */
{
    fw_set.magic = SET_MAGIC;
    fw_set.lights = SET_LIGHTS_DEFAULT;
    if (flash_ok && st_read(SET_ADDR, &fw_set_stored, sizeof fw_set_stored) == 0 &&
        fw_set_stored.magic == SET_MAGIC && fw_set_stored.crc == fw_set_crc(&fw_set_stored) &&
        fw_set_stored.lights <= SET_LIGHTS_MAX && fw_set_stored.theme <= FW_THEME_DARK)
        fw_set = fw_set_stored;
    fw_lights_off(0);
    fw_set_apply();
}

static void fw_lights_off(uint32_t labels)          /* the backlight layer: every button but these */
{
    uint32_t b, p, r;
    if (labels == fw_set_dark)
        return;
    fw_set_dark = labels;
    for (p = 0; p < FM1_NCOL; p++)
        fm1_led_bg[p] = 0;
    for (b = 0; b < FW_BTN_COUNT; b++)
        if (!(labels & FW_BIT(b)))
            for (p = 0; p < FM1_NCOL; p++)
                for (r = 1; r < 5u; r++)
                    if (FM1_KEYMAP[r][p] == (int8_t)fw_matrix_of(b))
                        fm1_led_bg[p] |= (uint8_t)(1u << r);
}

static void fw_set_save(void)
{
    fw_set.crc = fw_set_crc(&fw_set);
    if (!flash_ok || !memcmp(&fw_set, &fw_set_stored, sizeof fw_set))
        return;
    if (st_erase(SET_ADDR) == 0 && st_prog(SET_ADDR, &fw_set, sizeof fw_set) == 0)
        fw_set_stored = fw_set;
}

static void fw_set_draw(void)
{
    static const char *const LABEL[2] = {"LIGHTS", "THEME"};
    char v[4] = {0, 0, 0, 0};
    uint32_t r, i;
    if (fw_set_full) {
        lcd_fill(0, 0, 240, 240, C_BLACK);
        draw_text_box(0, 14, 240, &FONT_S, "SETTINGS", C_GRAY, 1);
        draw_text_box(0, 206, 240, &FONT_S, SET_ROWS > 1u ? "SELECT pick  ALGO change" : "ALGO change  HOME close",
                      C_GRAY, 1);
    }
    for (r = 0; r < SET_ROWS; r++)
        draw_text_box(16, SET_ROW_Y(r), 112, &FONT_L, LABEL[r], r == fw_set_row && SET_ROWS > 1u ? C_ACCENT : C_HI, 0);
    if (fw_set.lights)
        v[0] = (char)('0' + fw_set.lights);
    else
        str_cpy(v, "OFF", sizeof v);
    draw_text_box(144, SET_ROW_Y(0), 80, &FONT_L, v, C_WHITE, 2);
    cv_begin(208, 12, C_BLACK);                      /* the level as segments */
    for (i = 0; i < SET_LIGHTS_MAX; i++)
        cv_rect((int32_t)i * 26, 0, 22, 12, i < fw_set.lights ? C_ACCENT : C_DIM);
    cv_blit(16, SET_ROW_Y(0) + 46u);
    if (FW_SET_THEME)
        draw_text_box(128, SET_ROW_Y(1), 96, &FONT_L, SET_THEME_NAME[fw_set.theme], C_WHITE, 2);
    lcd_sync();
    fw_set_shown = fw_set;
    fw_set_full = 0;
}

static void fw_set_turn(int32_t d)                  /* ALGORITHM: the picked setting, one step */
{
    if (fw_set_row == 0u) {
        const int32_t l = (int32_t)fw_set.lights + d;
        fw_set.lights = (uint8_t)(l < 0 ? 0 : l > (int32_t)SET_LIGHTS_MAX ? SET_LIGHTS_MAX : l);
        fw_set_apply();
    } else {
        const int32_t t = (int32_t)fw_set.theme + d;
        fw_set.theme = (uint8_t)(t < 0 ? 0 : t > FW_THEME_DARK ? FW_THEME_DARK : t);
    }
}

/* before the app's frame: opens, runs and closes the settings, and takes from in what the app must not see */
static void fw_settings_frame(fw_input_t *in)
{
    const uint32_t held = in->held;
    const uint8_t row = fw_set_row;
    if (!fw_set_open && (held & FW_BIT(FW_BTN_HOME)) && (in->pressed & FW_BIT(FW_BTN_EDIT))) {
        fw_set_open = 1;
        fw_set_full = 1;
    } else if (fw_set_open && (in->pressed & (FW_BIT(FW_BTN_HOME) | FW_BIT(FW_BTN_EDIT)))) {
        fw_set_open = 0;
        fw_set_save();
        lcd_fill(0, 0, 240, 240, C_BLACK);
        in->redraw = 1;
    } else if (fw_set_open) {
        if (in->knob[FW_KNOB_SELECT] && SET_ROWS > 1u)
            fw_set_row = in->knob[FW_KNOB_SELECT] > 0 ? (uint8_t)(SET_ROWS - 1u) : 0u;
        if (in->knob[FW_KNOB_ALGO])
            fw_set_turn(in->knob[FW_KNOB_ALGO] > 0 ? 1 : -1);
    }
    if (fw_set_open && (fw_set_full || row != fw_set_row || memcmp(&fw_set, &fw_set_shown, sizeof fw_set)))
        fw_set_draw();
    if (fw_set_open || in->redraw)
        fw_set_swallow |= held | in->pressed;
    in->held &= ~fw_set_swallow;
    in->pressed &= ~fw_set_swallow;
    in->released &= ~fw_set_swallow;
    fw_set_swallow &= held;
    if (fw_set_open) {
        uint32_t k;
        for (k = 0; k < FW_KNOB_COUNT; k++)
            in->knob[k] = 0;
    }
}
