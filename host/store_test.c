/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl */
/* apps/beiwagen/bw_store.h on the real A/B storage (sys/storage.c) over a simulated NOR: store / recall / empty,
 * versions, power cut at every flash operation of a save, the gestures, the toast (tools/store_test.py). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "host_hal.h"
#include "fw.h"
#include "fw_tables.h"
#include "libc.c"
#include "lcd.c"
#include "gfx.c"
#include "input.c"

/* ---- the NOR: erase sets 0xFF, program only clears bits; a power cut at operation `cut` leaves that operation
   half done and every later one undone */
static uint8_t nor[0x100000];
static int ops, cut = -1, erases;

static int nor_dead(void) { return cut >= 0 && ops > cut; }

static int st_read(uint32_t off, void* dst, uint32_t n)
{
    memcpy(dst, nor + off, n);
    return 0;
}

static int st_erase(uint32_t off)
{
    assert(off >= 0xF7000u && off < 0xFB000u && !(off & 0xFFFu));
    if (nor_dead())
        return -1;
    erases++;
    memset(nor + off, 0xFF, cut == ops++ ? 0x800 : 0x1000);
    return nor_dead() ? -1 : 0;
}

static int st_prog(uint32_t off, const void* src, uint32_t n)
{
    uint32_t i;
    assert(off >= 0xF7000u && off + n <= 0xFB000u);
    if (nor_dead())
        return -1;
    if (cut == ops++)
        n /= 2u;
    for (i = 0; i < n; i++)
        nor[off + i] &= ((const uint8_t*)src)[i];
    return nor_dead() ? -1 : 0;
}

#include "storage.c"

static int fw_store_load(uint32_t slot, void* dst, uint32_t max) { return slot < FW_STORE_SLOTS ? st_load(slot, dst, max) : -1; }
static int fw_store_save(uint32_t slot, const void* src, uint32_t len)
{
    return slot < FW_STORE_SLOTS ? st_save(slot, src, len) : -1;
}

#include "bw_ui.h"
#include "bw_toast.h"
#include "bw_store.h"

/* ---- providers: a preset of 120 B, a pattern of 132 B (the limit) */
typedef struct {
    uint8_t b[120];
} preset_t;
typedef struct {
    uint8_t b[BW_STORE_BLOB_MAX];
} pattern_t;
BW_STORE_FITS(sizeof(preset_t));
BW_STORE_FITS(sizeof(pattern_t));

static preset_t cur_preset;
static pattern_t cur_pattern;
static int sets;

static void pre_get(void* d) { memcpy(d, &cur_preset, sizeof cur_preset); }
static void pre_set(const void* s) { memcpy(&cur_preset, s, sizeof cur_preset); sets++; }
static void pre_def(void* d) { memset(d, 0xD0, sizeof cur_preset); }
static void pat_get(void* d) { memcpy(d, &cur_pattern, sizeof cur_pattern); }
static void pat_set(const void* s) { memcpy(&cur_pattern, s, sizeof cur_pattern); }
static void pat_def(void* d) { memset(d, 0, sizeof cur_pattern); }

static const bw_store_prov_t PRESET = { sizeof(preset_t), 1, pre_get, pre_set, pre_def, 0 };
static const bw_store_prov_t PATTERN = { sizeof(pattern_t), 1, pat_get, pat_set, pat_def, 0 };

static void fill(preset_t* p, uint8_t v) { memset(p, v, sizeof *p); }
static int is(const preset_t* p, uint8_t v)
{
    uint32_t i;
    for (i = 0; i < sizeof p->b; i++)
        if (p->b[i] != v)
            return 0;
    return 1;
}

static void reboot(void)
{
    memset(bw_img, 0x5A, sizeof bw_img);    /* the RAM is gone */
    bw_store_init();
}

static void test_basic(void)
{
    memset(nor, 0xFF, sizeof nor);
    bw_store_init();
    assert(bw_store_register(BW_BANK_PRESET, &PRESET) == 0);
    assert(bw_store_register(BW_BANK_PATTERN, &PATTERN) == 0);
    assert(bw_store_used(BW_BANK_PRESET) == 0);
    assert(bw_store_load(BW_BANK_PRESET, 4) == BW_ST_EMPTY);
    fill(&cur_preset, 0x11);
    erases = 0;
    assert(bw_store_save(BW_BANK_PRESET, 4) == BW_ST_OK);
    assert(erases == 1);                    /* one sector a save */
    fill(&cur_preset, 0x22);
    assert(bw_store_save(BW_BANK_PRESET, 26) == BW_ST_OK);
    memset(&cur_pattern, 0x77, sizeof cur_pattern);
    assert(bw_store_save(BW_BANK_PATTERN, 0) == BW_ST_OK);
    reboot();
    assert(bw_store_used(BW_BANK_PRESET) == (1u << 4 | 1u << 26));
    assert(bw_store_used(BW_BANK_PATTERN) == 1u);
    assert(bw_store_load(BW_BANK_PRESET, 4) == BW_ST_OK && is(&cur_preset, 0x11));
    assert(bw_store_load(BW_BANK_PRESET, 26) == BW_ST_OK && is(&cur_preset, 0x22));
    assert(bw_store_load(BW_BANK_PRESET, 5) == BW_ST_EMPTY && is(&cur_preset, 0x22));
    memset(&cur_pattern, 0, sizeof cur_pattern);
    assert(bw_store_load(BW_BANK_PATTERN, 0) == BW_ST_OK && cur_pattern.b[BW_STORE_BLOB_MAX - 1u] == 0x77);
    /* all 27 keys and the session at the largest blob */
    {
        uint32_t k;
        for (k = 0; k < BW_STORE_SLOTS; k++) {
            memset(&cur_pattern, (int)k + 1, sizeof cur_pattern);
            assert(bw_store_save(BW_BANK_PATTERN, k) == BW_ST_OK);
        }
        reboot();
        for (k = 0; k < BW_STORE_SLOTS; k++)
            assert(bw_store_load(BW_BANK_PATTERN, k) == BW_ST_OK && cur_pattern.b[0] == k + 1u &&
                   cur_pattern.b[BW_STORE_BLOB_MAX - 1u] == k + 1u);
    }
    puts("store_test: store / recall / empty / 28 x 132 B ok");
}

static int playing_now;
static int playing(void) { return playing_now; }

static void test_busy(void)
{
    bw_store_busy = playing;
    playing_now = 1;
    erases = 0;
    assert(bw_store_save(BW_BANK_PRESET, 3) == BW_ST_BUSY && erases == 0);
    assert(!(bw_store_used(BW_BANK_PRESET) & 1u << 3));
    playing_now = 0;
    bw_store_busy = 0;
    puts("store_test: busy hook ok");
}

/* a newer firmware: the preset grew from 120 to 124 B (append-only, version 1), then became version 2 */
static uint8_t big[124];
static void big_get(void* d) { memcpy(d, big, sizeof big); }
static void big_set(const void* s) { memcpy(big, s, sizeof big); }
static void big_def(void* d) { memset(d, 0xEE, sizeof big); }
static int big_mig(void* d, const void* s, uint32_t len, uint32_t from)
{
    uint32_t i;
    assert(from == 1u && len == 120u);
    for (i = 0; i < len; i++)
        ((uint8_t*)d)[i] = (uint8_t)(((const uint8_t*)s)[i] + 1u);
    return 0;
}

static void test_versions(void)
{
    static const bw_store_prov_t BIG1 = { sizeof big, 1, big_get, big_set, big_def, 0 };
    static const bw_store_prov_t BIG2 = { sizeof big, 2, big_get, big_set, big_def, big_mig };
    static const bw_store_prov_t OLD = { 120, 1, pre_get, pre_set, pre_def, 0 };
    fill(&cur_preset, 0x33);
    assert(bw_store_save(BW_BANK_PRESET, 7) == BW_ST_OK);
    assert(bw_store_register(BW_BANK_PRESET, &BIG1) == 0);
    assert(bw_store_load(BW_BANK_PRESET, 7) == BW_ST_OK);
    assert(big[0] == 0x33 && big[119] == 0x33 && big[120] == 0xEE && big[123] == 0xEE);   /* defaults extend it */
    assert(bw_store_register(BW_BANK_PRESET, &BIG2) == 0);
    assert(bw_store_load(BW_BANK_PRESET, 7) == BW_ST_OK && big[0] == 0x34 && big[120] == 0xEE);   /* migrated */
    big[0] = 0x99;
    assert(bw_store_save(BW_BANK_PRESET, 8) == BW_ST_OK);
    assert(bw_store_register(BW_BANK_PRESET, &OLD) == 0);   /* back to the older firmware */
    assert(bw_store_load(BW_BANK_PRESET, 8) == BW_ST_NEWER);
    assert(bw_store_load(BW_BANK_PRESET, 7) == BW_ST_OK && is(&cur_preset, 0x33));
    /* a bank written by a newer layout: read-only here */
    bw_img[BW_BANK_PRESET].h.layout = BW_STORE_LAYOUT + 1u;
    assert(fw_store_save(BW_BANK_PRESET, &bw_img[BW_BANK_PRESET], sizeof bw_img[0]) == 0);
    reboot();
    assert(bw_store_used(BW_BANK_PRESET) == 0 && bw_store_load(BW_BANK_PRESET, 7) == BW_ST_EMPTY);
    erases = 0;
    assert(bw_store_save(BW_BANK_PRESET, 7) == BW_ST_NEWER && erases == 0);
    /* a foreign object (another firmware's): an empty bank, saving takes it over */
    memset(nor + 0xF7000, 0xFF, 0x2000);
    st_save(0, "not ours", 9);
    reboot();
    assert(bw_store_used(BW_BANK_PRESET) == 0);
    assert(bw_store_save(BW_BANK_PRESET, 1) == BW_ST_OK);
    /* sizes the registry refuses */
    {
        static const bw_store_prov_t TOO_BIG = { BW_STORE_BLOB_MAX + 1u, 1, pre_get, pre_set, pre_def, 0 };
        static const bw_store_prov_t NO_VER = { 4, 0, pre_get, pre_set, pre_def, 0 };
        assert(bw_store_register(BW_BANK_PRESET, &TOO_BIG) < 0);
        assert(bw_store_register(BW_BANK_PRESET, &NO_VER) < 0);
        assert(bw_store_register(BW_BANKS, &OLD) < 0);
    }
    puts("store_test: versions (append-only, migrate, newer slot, newer layout, foreign) ok");
}

/* power cut at each flash operation of a save, then a restart: the slot is the old or the new, never lost, and the
   other slots are untouched */
static void test_power_cut(void)
{
    int n, total, saw_old = 0, saw_new = 0;
    memset(nor, 0xFF, sizeof nor);
    bw_store_init();
    fill(&cur_preset, 0x41);
    assert(bw_store_save(BW_BANK_PRESET, 2) == BW_ST_OK);
    fill(&cur_preset, 0x42);
    assert(bw_store_save(BW_BANK_PRESET, 9) == BW_ST_OK);
    for (n = 0;; n++) {
        static uint8_t snap[0x4000];
        int r;
        memcpy(snap, nor + 0xF7000, sizeof snap);
        fill(&cur_preset, 0x43);
        ops = 0;
        cut = n;
        r = bw_store_save(BW_BANK_PRESET, 2);
        total = ops;
        cut = -1;
        if (r == BW_ST_OK) {
            assert(n >= total);
            reboot();
            assert(bw_store_load(BW_BANK_PRESET, 2) == BW_ST_OK && is(&cur_preset, 0x43));
            break;
        }
        assert(bw_img[0].h.used == (1u << 2 | 1u << 9));   /* a failed save leaves the image as it was */
        reboot();
        assert(bw_store_load(BW_BANK_PRESET, 9) == BW_ST_OK && is(&cur_preset, 0x42));
        assert(bw_store_load(BW_BANK_PRESET, 2) == BW_ST_OK);
        if (is(&cur_preset, 0x41))
            saw_old++;
        else if (is(&cur_preset, 0x43))
            saw_new++;
        else
            assert(!"slot 2 is neither the old nor the new");
        memcpy(nor + 0xF7000, snap, sizeof snap);   /* the next run cuts the same save one operation later */
        reboot();
    }
    assert(saw_old > 0);
    printf("store_test: power cut at each of %d flash operations of a save: kept the old copy %d x, the new %d x ok\n",
           total, saw_old, saw_new);
}

/* ---- the gestures */
static fw_input_t in;
static void frame(uint32_t held, uint32_t keys_pressed)
{
    in.ms += 15u;
    in.pressed = held & ~in.held;
    in.released = in.held & ~held;
    in.held = held;
    in.keys_pressed = keys_pressed;
    in.keys |= keys_pressed;
}

static uint64_t key_leds(void) { return host_leds >> 14 & ((1u << FW_KEYS) - 1u); }

static void write_ppm(const char* path)
{
    FILE* f = fopen(path, "wb");
    uint32_t i;
    assert(f);
    fprintf(f, "P6\n240 240\n255\n");
    for (i = 0; i < 240u * 240u; i++) {
        const uint16_t c = host_fb[i];
        const uint8_t rgb[3] = { (uint8_t)((c >> 11) * 255u / 31u), (uint8_t)(((c >> 5) & 63u) * 255u / 63u),
                                 (uint8_t)((c & 31u) * 255u / 31u) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("screen %s\n", path);
}

static int footer_draws;
static void footer(int full)
{
    if (toast_footer(in.ms, &full))
        return;
    if (full)
        footer_draws++;
}

static void test_gestures(void)
{
    const uint32_t SAVE = FW_BIT(BW_STORE_BTN_SAVE), SEL = FW_BIT(BW_STORE_BTN_LOAD);
    int full_before;
    memset(nor, 0xFF, sizeof nor);
    bw_store_init();
    assert(bw_store_register(BW_BANK_PRESET, &PRESET) == 0);
    bw_prov[BW_BANK_PATTERN] = 0;           /* no sequencer yet */
    lcd_init();
    lcd_fill(0, 0, 240, 240, C_BG);
    /* a plain key: not the store's */
    frame(0, 1u << 3);
    assert(bw_store_frame(&in, BW_BANK_PRESET) == 0);
    /* SAVE held: a hint, the filled slots on the LEDs (none), the key taken and stored */
    frame(SAVE, 0);
    assert(bw_store_frame(&in, BW_BANK_PRESET) == 0);
    bw_store_leds();
    assert(key_leds() == 0 && toast_on && !strcmp(toast_txt, "Save preset: press a key"));
    fill(&cur_preset, 0x51);
    frame(SAVE, 1u << 4);
    assert(bw_store_frame(&in, BW_BANK_PRESET) == 1u << 4);
    bw_store_leds();
    assert(key_leds() == 1u << 4 && !strcmp(toast_txt, "Saved to key 5"));
    footer(0);
    write_ppm("build/host/out/store_saved.ppm");
    frame(0, 0);
    assert(bw_store_frame(&in, BW_BANK_PRESET) == 0);
    assert(toast_on && !strcmp(toast_txt, "Saved to key 5"));   /* the confirmation stays its time */
    footer_draws = 0;
    in.ms += TOAST_MS;
    footer(0);
    assert(!toast_on && footer_draws == 1);  /* gone: the footer draws itself whole */
    /* SEL held, released without a key: nothing happens, the hint goes */
    fill(&cur_preset, 0x52);
    frame(SEL, 0);
    bw_store_frame(&in, BW_BANK_PRESET);
    bw_store_leds();
    assert(key_leds() == 1u << 4 && toast_on);
    footer(0);
    write_ppm("build/host/out/store_hint.ppm");
    printf("leds %016llx\n", (unsigned long long)host_leds);
    frame(0, 0);
    bw_store_frame(&in, BW_BANK_PRESET);
    footer_draws = 0;
    full_before = 0;
    in.ms += 1u;
    footer(full_before);
    assert(!toast_on && footer_draws == 1 && is(&cur_preset, 0x52));
    /* SEL + key: recalled from the main loop; an empty key says so */
    sets = 0;
    frame(SEL, 0);
    bw_store_frame(&in, BW_BANK_PRESET);
    frame(SEL, 1u << 4);
    assert(bw_store_frame(&in, BW_BANK_PRESET) == 1u << 4);
    assert(sets == 1 && is(&cur_preset, 0x51) && !strcmp(toast_txt, "Loaded key 5"));
    frame(SEL, 1u << 11 | 1u << 20);
    assert(bw_store_frame(&in, BW_BANK_PRESET) == (1u << 11 | 1u << 20));   /* both taken, the lower acts */
    assert(sets == 1 && !strcmp(toast_txt, "Key 12 is empty"));
    footer(0);
    write_ppm("build/host/out/store_empty.ppm");
    frame(0, 0);
    bw_store_frame(&in, BW_BANK_PRESET);
    /* SEQ: the pattern bank, and no provider there */
    frame(SAVE, 0);
    bw_store_frame(&in, BW_BANK_PATTERN);
    assert(!strcmp(toast_txt, "Save pattern: press a key"));
    frame(SAVE, 1u);
    assert(bw_store_frame(&in, BW_BANK_PATTERN) == 1u && !strcmp(toast_txt, "No pattern"));
    frame(0, 0);
    bw_store_frame(&in, BW_BANK_PATTERN);
    /* SAVE held alone: the session after BW_STORE_SESSION_MS, once; restored at the next start */
    fill(&cur_preset, 0x61);
    frame(SAVE, 0);
    bw_store_frame(&in, BW_BANK_PRESET);
    in.ms += BW_STORE_SESSION_MS - 30u;
    frame(SAVE, 0);
    bw_store_frame(&in, BW_BANK_PRESET);
    assert(!(bw_img[0].h.used >> BW_STORE_SESSION & 1u));
    in.ms += 30u;
    erases = 0;
    frame(SAVE, 0);
    bw_store_frame(&in, BW_BANK_PRESET);
    frame(SAVE, 0);
    bw_store_frame(&in, BW_BANK_PRESET);
    assert(erases == 1 && !strcmp(toast_txt, "Session saved"));
    frame(0, 0);
    bw_store_frame(&in, BW_BANK_PRESET);
    fill(&cur_preset, 0);
    reboot();
    bw_store_session_load();
    assert(is(&cur_preset, 0x61));
    /* playing: the save refused, said */
    bw_store_busy = playing;
    playing_now = 1;
    frame(SAVE, 0);
    bw_store_frame(&in, BW_BANK_PRESET);
    frame(SAVE, 1u << 6);
    assert(bw_store_frame(&in, BW_BANK_PRESET) == 1u << 6 && !strcmp(toast_txt, "Stop playback to save"));
    assert(!(bw_store_used(BW_BANK_PRESET) & 1u << 6));
    footer(0);
    write_ppm("build/host/out/store_busy.ppm");
    bw_store_busy = 0;
    puts("store_test: gestures (save, recall, empty, cancel, keys taken, LEDs, session, busy) ok");
}

int main(void)
{
    test_basic();
    test_busy();
    test_versions();
    test_power_cut();
    test_gestures();
    printf("store_test: a bank is %u of %u B: header %u B, %u slots of %u B\n", (unsigned)sizeof(bw_store_img_t),
           (unsigned)FW_STORE_MAX, (unsigned)sizeof(bw_store_hdr_t), (unsigned)BW_STORE_SLOTS, (unsigned)BW_STORE_BLOB_MAX);
    puts("store_test: ok");
    return 0;
}
