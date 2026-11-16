/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl */
/* Presets and patterns on the keys (unity build, included once by app.c, after bw_ui.h and bw_toast.h).
 *
 * Two banks, one per storage object of the platform (fw_store_*, sys/storage.c: each object is its own A/B sector
 * pair, so a save erases and writes one 4 KB sector and a torn save leaves the last copy in charge):
 *   bank 0 = presets (the mixer / effects state), bank 1 = patterns (the sequencer's).
 * What a bank holds comes from its provider: get / set / defaults of a blob of up to BW_STORE_BLOB_MAX bytes.
 *
 * A bank's image (the object's payload, 3764 of 3840 B):
 *   header 68 B: magic "BWS1", layout version, slot stride, a used bit per slot, per slot the provider version and
 *                the blob size it was saved with
 *   28 slots of 132 B: the 27 keys, then the session (the state at the last explicit session save)
 *
 * Versions. A provider's blob grows append-only within a version: a newer firmware reading an older slot starts from
 * its defaults and lays the stored bytes over them (new fields keep their defaults). A change that is not
 * append-only bumps the provider's version and gives a migrate(); a slot from an older version is then migrated, or,
 * without migrate(), read the append-only way. A slot from a newer version (after a downgrade) is not loaded
 * ("newer firmware"). A bank whose layout is newer than this firmware's is read-only: nothing loads from it and
 * saves are refused, so a downgrade never destroys it; an unknown or damaged object reads as an empty bank.
 *
 * Gestures (bw_store_frame, every frame, before the keys play): hold SAVE + key: the current preset (in SEQ: the
 * pattern) goes onto the key; hold SEL (SCL in fw.h) + key: it comes back. While either is held the key LEDs show the
 * filled slots, a hint is in the footer, and pressed keys are the store's (the mask it returns: keep them from
 * MIDI). Releasing without a key changes nothing. Holding SAVE alone for BW_STORE_SESSION_MS saves the session
 * (both banks' current state, restored at power-on). Saves are refused while bw_store_busy() says so (the transport
 * runs): a save stalls the audio for one sector erase. */
#ifndef BW_STORE_H
#define BW_STORE_H

#define BW_STORE_BTN_SAVE FW_BTN_SAVE
#define BW_STORE_BTN_LOAD FW_BTN_SCL        /* the panel's SEL */
#define BW_STORE_SESSION_MS 2000u           /* SAVE held alone this long: the session is saved */

#define BW_STORE_MAGIC 0x31535742u          /* "BWS1" */
#define BW_STORE_LAYOUT 1u
#define BW_STORE_SESSION FW_KEYS            /* the slot after the keys' */
#define BW_STORE_SLOTS (FW_KEYS + 1u)
#define BW_STORE_BLOB_MAX 132u              /* a provider's blob, at most */

enum { BW_BANK_PRESET, BW_BANK_PATTERN, BW_BANKS };

/* the app's check for a provider's blob: BW_STORE_FITS(sizeof(mix_preset_t)); */
#define BW_STORE_FITS(size) _Static_assert((size) <= BW_STORE_BLOB_MAX, "a stored blob is at most 132 bytes")

typedef struct {
    uint32_t size;                          /* 1..BW_STORE_BLOB_MAX */
    uint32_t version;                       /* 1..255 */
    void (*get)(void* dst);                 /* the current state -> blob */
    void (*set)(const void* src);           /* blob -> the current state (main loop; the provider ramps) */
    void (*defaults)(void* dst);            /* the defaults as a blob */
    int (*migrate)(void* dst, const void* src, uint32_t len, uint32_t from);   /* 0: ok; may be 0 (append-only) */
} bw_store_prov_t;

typedef struct {
    uint32_t magic;
    uint16_t layout, stride;
    uint32_t used;                          /* bit s: slot s holds a blob */
    uint8_t ver[BW_STORE_SLOTS];            /* the provider's version it was saved with */
    uint8_t len[BW_STORE_SLOTS];            /* its size then */
} bw_store_hdr_t;

typedef struct {
    bw_store_hdr_t h;
    uint8_t slot[BW_STORE_SLOTS][BW_STORE_BLOB_MAX];
} bw_store_img_t;

_Static_assert(sizeof(bw_store_hdr_t) == 68u, "the header's layout is the flash's");
_Static_assert(sizeof(bw_store_img_t) <= FW_STORE_MAX, "a bank fits a storage object");
_Static_assert(BW_BANKS <= FW_STORE_SLOTS, "a storage object per bank");
_Static_assert(BW_STORE_SLOTS <= 32u, "the used bits");

/* what bw_store_put / bw_store_get say */
enum { BW_ST_OK = 1, BW_ST_EMPTY = 0, BW_ST_FAIL = -1, BW_ST_NEWER = -2, BW_ST_BUSY = -3, BW_ST_NOPROV = -4 };

static bw_store_img_t bw_img[BW_BANKS] FW_POOL;
static uint8_t bw_img_ro[BW_BANKS];         /* the bank is a newer layout's: read-only */
static const bw_store_prov_t* bw_prov[BW_BANKS];
static uint8_t bw_blob[BW_STORE_BLOB_MAX];
static int (*bw_store_busy)(void);          /* the app's: 1 while a save would hurt (the transport runs) */

static void bw_store_blank(uint32_t b)
{
    memset(&bw_img[b], 0, sizeof bw_img[b]);
    bw_img[b].h.magic = BW_STORE_MAGIC;
    bw_img[b].h.layout = BW_STORE_LAYOUT;
    bw_img[b].h.stride = BW_STORE_BLOB_MAX;
}

/* both banks from the flash (app_init; the flash is up then, the audio not yet) */
static void bw_store_init(void)
{
    uint32_t b;
    for (b = 0; b < BW_BANKS; b++) {
        const int n = fw_store_load(b, &bw_img[b], sizeof bw_img[b]);
        const bw_store_hdr_t* h = &bw_img[b].h;
        bw_img_ro[b] = 0;
        if (n >= (int)sizeof *h && h->magic == BW_STORE_MAGIC && h->layout > BW_STORE_LAYOUT) {
            bw_img_ro[b] = 1;
            continue;
        }
        if (n != (int)sizeof bw_img[b] || h->magic != BW_STORE_MAGIC || h->layout != BW_STORE_LAYOUT ||
            h->stride != BW_STORE_BLOB_MAX)
            bw_store_blank(b);
    }
}

static int bw_store_register(uint32_t bank, const bw_store_prov_t* p)
{
    if (bank >= BW_BANKS || !p || !p->size || p->size > BW_STORE_BLOB_MAX || !p->version || p->version > 255u ||
        !p->get || !p->set || !p->defaults)
        return -1;
    bw_prov[bank] = p;
    return 0;
}

static uint32_t bw_store_used(uint32_t bank)
{
    return bank < BW_BANKS && !bw_img_ro[bank] ? bw_img[bank].h.used & ((1u << FW_KEYS) - 1u) : 0;
}

/* a slot as the provider's current blob, into dst (size bytes): BW_ST_OK, _EMPTY, _NEWER, _FAIL, _NOPROV */
static int bw_store_read(uint32_t bank, uint32_t slot, void* dst)
{
    const bw_store_prov_t* p;
    const bw_store_hdr_t* h;
    uint32_t n;
    if (bank >= BW_BANKS || slot >= BW_STORE_SLOTS)
        return BW_ST_FAIL;
    p = bw_prov[bank];
    h = &bw_img[bank].h;
    if (!p)
        return BW_ST_NOPROV;
    if (bw_img_ro[bank] || !(h->used >> slot & 1u))
        return BW_ST_EMPTY;
    if (h->ver[slot] > p->version)
        return BW_ST_NEWER;
    p->defaults(dst);
    if (h->ver[slot] < p->version && p->migrate)
        return p->migrate(dst, bw_img[bank].slot[slot], h->len[slot], h->ver[slot]) ? BW_ST_FAIL : BW_ST_OK;
    n = h->len[slot] < p->size ? h->len[slot] : p->size;
    memcpy(dst, bw_img[bank].slot[slot], n);
    return BW_ST_OK;
}

/* recall: the slot into the current state (main loop) */
static int bw_store_load(uint32_t bank, uint32_t slot)
{
    const int r = bw_store_read(bank, slot, bw_blob);
    if (r == BW_ST_OK)
        bw_prov[bank]->set(bw_blob);
    return r;
}

/* the current state into a slot and onto the flash (one sector: the audio stalls meanwhile). On a failed write the
   image is as before, so RAM and flash agree */
static int bw_store_save(uint32_t bank, uint32_t slot)
{
    static bw_store_hdr_t old_h;
    static uint8_t old_s[BW_STORE_BLOB_MAX];
    const bw_store_prov_t* p;
    bw_store_img_t* m;
    if (bank >= BW_BANKS || slot >= BW_STORE_SLOTS)
        return BW_ST_FAIL;
    p = bw_prov[bank];
    m = &bw_img[bank];
    if (!p)
        return BW_ST_NOPROV;
    if (bw_img_ro[bank])
        return BW_ST_NEWER;
    if (bw_store_busy && bw_store_busy())
        return BW_ST_BUSY;
    old_h = m->h;
    memcpy(old_s, m->slot[slot], sizeof old_s);
    memset(m->slot[slot], 0, sizeof m->slot[slot]);
    p->get(m->slot[slot]);
    m->h.used |= 1u << slot;
    m->h.ver[slot] = (uint8_t)p->version;
    m->h.len[slot] = (uint8_t)p->size;
    if (fw_store_save(bank, m, sizeof *m) != 0) {
        m->h = old_h;
        memcpy(m->slot[slot], old_s, sizeof old_s);
        return BW_ST_FAIL;
    }
    return BW_ST_OK;
}

/* the session: every bank's current state (a sector per bank), and back at power-on */
static int bw_store_session_save(void)
{
    uint32_t b;
    int r = BW_ST_NOPROV;
    for (b = 0; b < BW_BANKS; b++)
        if (bw_prov[b]) {
            const int rb = bw_store_save(b, BW_STORE_SESSION);
            r = r == BW_ST_NOPROV || rb < r ? rb : r;
            if (rb == BW_ST_BUSY)
                break;
        }
    return r;
}

static void bw_store_session_load(void)
{
    uint32_t b;
    for (b = 0; b < BW_BANKS; b++)
        bw_store_load(b, BW_STORE_SESSION);
}

/* ---- the gestures */
enum { BW_HOLD_NONE, BW_HOLD_SAVE, BW_HOLD_LOAD };

static uint8_t bw_hold;                     /* BW_HOLD_* */
static uint8_t bw_hold_bank, bw_hold_acted;
static uint32_t bw_hold_ms;

static void bw_store_say(const char* verb, const char* what, int key, int r, uint32_t ms)
{
    char b[40];
    uint16_t c = C_TEXT;
    if (r == BW_ST_OK) {
        str_cpy(b, verb, sizeof b);
        if (key) {
            ui_cat(b, " key ", sizeof b);
            ui_cat_int(b, key, sizeof b);
        }
    } else {
        c = C_WARN;
        if (r == BW_ST_EMPTY) {
            str_cpy(b, "Key ", sizeof b);
            ui_cat_int(b, key, sizeof b);
            ui_cat(b, " is empty", sizeof b);
        } else {
            str_cpy(b, r == BW_ST_BUSY ? "Stop playback to save" : r == BW_ST_NEWER ? "From newer firmware"
                    : r == BW_ST_NOPROV ? "No " : "Flash error, not saved", sizeof b);
            if (r == BW_ST_NOPROV)
                ui_cat(b, what, sizeof b);
        }
    }
    toast_show(b, c, ms, TOAST_MS);
}

/* every frame, before the keys play; bank: BW_BANK_PATTERN in SEQ, else BW_BANK_PRESET. Returns the keys it took
   (keep them from MIDI: keys_pressed & ~mask). */
static uint32_t bw_store_frame(const fw_input_t* in, uint32_t bank)
{
    static const char* const WHAT[BW_BANKS] = { "preset", "pattern" };
    const uint32_t mode = (in->held & FW_BIT(BW_STORE_BTN_SAVE)) ? BW_HOLD_SAVE
                          : (in->held & FW_BIT(BW_STORE_BTN_LOAD)) ? BW_HOLD_LOAD : BW_HOLD_NONE;
    uint32_t k;
    if (mode != bw_hold) {
        if (bw_hold && !bw_hold_acted)      /* released without a key: the hint goes */
            toast_hide();
        bw_hold = (uint8_t)mode;
        bw_hold_bank = (uint8_t)(bank < BW_BANKS ? bank : BW_BANK_PRESET);
        bw_hold_acted = 0;
        bw_hold_ms = in->ms;
        if (mode) {
            char b[40];
            str_cpy(b, mode == BW_HOLD_SAVE ? "Save " : "Load ", sizeof b);
            ui_cat(b, WHAT[bw_hold_bank], sizeof b);
            ui_cat(b, ": press a key", sizeof b);
            toast_show(b, C_TEXT, in->ms, 0);
        }
    }
    if (!mode)
        return 0;
    if (in->keys_pressed) {
        for (k = 0; !(in->keys_pressed >> k & 1u); k++)
            ;
        bw_hold_acted = 1;
        if (mode == BW_HOLD_SAVE)
            bw_store_say("Saved to", WHAT[bw_hold_bank], (int)k + 1, bw_store_save(bw_hold_bank, k), in->ms);
        else
            bw_store_say("Loaded", WHAT[bw_hold_bank], (int)k + 1, bw_store_load(bw_hold_bank, k), in->ms);
    } else if (mode == BW_HOLD_SAVE && !bw_hold_acted && in->ms - bw_hold_ms >= BW_STORE_SESSION_MS) {
        bw_hold_acted = 1;
        bw_store_say("Session saved", "state to save", 0, bw_store_session_save(), in->ms);
    }
    return in->keys_pressed;
}

/* after the keys' LEDs: while SAVE / SEL is held, the filled slots of the bank instead */
static void bw_store_leds(void)
{
    if (bw_hold)
        fw_leds_keys(bw_store_used(bw_hold_bank));
}

#endif
