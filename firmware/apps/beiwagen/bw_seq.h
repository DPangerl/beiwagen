/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl */
/* Beiwagen's step sequencer and arpeggiator (unity build, included once by app.c; host/seq_test.c runs it alone).
 * Both play the OP-1 over USB-MIDI.
 *
 * Time: seq_tick() runs in TIMER5 every 0.1 ms (sys/main.c, APP_TICK) and moves a timeline of SEQ_PPQ units per
 * quarter, at the tempo (seq_bpm) or, when MIDI clock comes in (24 per quarter, seq_midi_rt from sys/usb.c,
 * APP_MIDI_RT), clock by clock, interpolated between them. What falls due is queued as USB-MIDI packets straight into
 * the platform's out ring (midi_out_event), which TIMER5's USB poll sends within 0.5 ms. Nothing here waits for the
 * main loop (~66 Hz).
 *
 * Notes: every note-on is remembered with its channel until its note-off has gone out (a stop, a new pattern, another
 * channel, the arp let go: the note-off still goes, on the note's own channel). A note played again while it sounds
 * is ended first. When the ring has no room, the events wait for the next tick: nothing is dropped.
 *
 * Concurrency: the engine is TIMER5's. The main loop changes it only through the seq_* calls marked "main loop",
 * which hold interrupts off for a few microseconds; the patterns' bytes it writes directly. Integer maths only. */
#ifndef BW_SEQ_H
#define BW_SEQ_H

#define SEQ_PATTERNS 27u                    /* one per key */
#define SEQ_STEPS 32u                       /* per pattern at most (pages of SEQ_PAGE); a step is 3 bytes */
#define SEQ_PAGE 16u
#define SEQ_PPQ 960u                        /* timeline units per quarter */
#define SEQ_CLK (SEQ_PPQ / 24u)             /* per MIDI clock */
#define SEQ_STEP (SEQ_PPQ / 4u)             /* a step: a 16th */
#define SEQ_TICK_HZ 10000u                  /* seq_tick's rate (TIMER5) */
#define SEQ_EXT_LOST (SEQ_TICK_HZ / 2u)     /* no clock for 0.5 s: it is gone */

/* a step: note | SEQ_ON, vel (1..127) | SEQ_TIE, gate 1..100 (% of the step). A pattern is its own blob */
#define SEQ_ON 0x80u
#define SEQ_TIE 0x80u
typedef struct {
    uint8_t note, vel, gate;
} seq_step_t;
typedef struct {
    uint8_t ver, len, ch, swing;            /* SEQ_VER; 1..SEQ_STEPS; 0..15; 50..75 (% of an 8th) */
    seq_step_t s[SEQ_STEPS];
} seq_pattern_t;
#define SEQ_VER 1u
#define SEQ_BLOB ((uint32_t)sizeof(seq_pattern_t))
typedef char seq_blob_is_100[SEQ_BLOB == 100u ? 1 : -1];

enum { ARP_UP, ARP_DOWN, ARP_UPDOWN, ARP_RANDOM, ARP_PLAYED, ARP_MODES };
#define ARP_RATES 7
static const uint16_t ARP_RATE[ARP_RATES] = { 960, 480, 320, 240, 160, 120, 80 };   /* 1/4 1/8 1/8T 1/16 1/16T 1/32 1/32T */

/* settings: the UI's knobs point at them (int32_t); TIMER5 reads them */
static int32_t seq_bpm = 120;               /* 30..300 */
static int32_t seq_sync;                    /* 0: follow MIDI clock when it comes, 1: internal only */
static int32_t seq_clk_out;                 /* 1: clock, start, stop out while playing on the internal clock */
static int32_t arp_on, arp_mode, arp_rate = 3, arp_gate = 50, arp_oct = 1, arp_latch;

static seq_pattern_t seq_pat[SEQ_PATTERNS];

/* what the UI reads */
static volatile int32_t seq_cur = -1;       /* the step playing, -1: stopped */
static volatile uint8_t seq_run;            /* the pattern plays */
static volatile uint8_t seq_armed;          /* on the MIDI clock: PLAY pressed, it plays while the clock runs */
static volatile uint8_t seq_ext;            /* MIDI clock comes in (and seq_sync allows it) */
static volatile uint8_t seq_rec;            /* REC: keys go into the running pattern */
static volatile uint8_t seq_pat_i, seq_pat_next;   /* the pattern playing, the one after it */

enum { SRC_SEQ, SRC_ARP };
typedef struct {
    uint32_t at, len;                       /* len 0: tied (ends with the next step) */
    uint8_t note, vel, ch, src;
} seq_pend_t;
typedef struct {
    uint32_t off;
    uint8_t note, ch, src, tie;
} seq_act_t;
#define SEQ_PEND 16u
#define SEQ_ACT 24u
#define SEQ_HELD 16u
#define SEQ_RECN 4u

static struct {
    seq_pend_t pend[SEQ_PEND];              /* note-ons to come */
    seq_act_t act[SEQ_ACT];                 /* notes sounding */
    uint32_t npend, nact;
    uint32_t now, pos, acc;                 /* ticks; the timeline; its fraction */
    uint32_t next_step, nxt, skip;          /* the next step's position and index; steps not to play once */
    uint32_t next_clk;                      /* clock out */
    uint32_t clk_pos, ext_last, ext_q4;     /* clock in: its last position, tick, period (ticks, Q4) */
    uint32_t stop_pos;                      /* where it stopped (MIDI continue) */
    uint8_t ext_run, ext_wait, ext_cont;
    uint32_t arp_next, arp_i, rnd;
    uint8_t arp_live, arp_ch;
    uint8_t held[SEQ_HELD], held_vel[SEQ_HELD];   /* the arp's notes, as played */
    uint32_t nheld, nphys;
    uint8_t rec_note[SEQ_RECN], rec_step[SEQ_RECN];
    uint32_t rec_at[SEQ_RECN];
} sq;

#define SEQ_DUE(t) ((int32_t)(sq.pos - (t)) >= 0)

static void seq_put(uint32_t st, uint32_t d1, uint32_t d2)
{
    const uint32_t cin = st >= 0xF0u ? 0xFu : st >> 4;
    midi_out_event(cin | st << 8 | d1 << 16 | d2 << 24);
}

static uint32_t seq_gate_units(uint32_t g) { return g ? g * SEQ_STEP / 100u : SEQ_STEP / 2u; }
static uint32_t seq_swing_units(uint32_t sw) { return sw > 50u ? (sw > 75u ? 25u : sw - 50u) * (SEQ_PPQ / 2u) / 100u : 0u; }
static uint32_t seq_ceil(uint32_t v, uint32_t q) { return (v + q - 1u) / q * q; }

/* ---- the notes */

/* what is due: note-ons first (a note sounding again is ended before), then note-offs; 0 when the ring was full */
static int seq_fire(void)
{
    uint32_t i, j;
    for (i = 0; i < sq.npend;) {
        const seq_pend_t e = sq.pend[i];
        if (!SEQ_DUE(e.at)) {
            i++;
            continue;
        }
        if (midi_out_room() < 2u)
            return 0;
        for (j = 0; j < sq.nact; j++)
            if (sq.act[j].note == e.note && sq.act[j].ch == e.ch) {
                seq_put(0x80u | e.ch, e.note, 0);
                sq.act[j] = sq.act[--sq.nact];
                break;
            }
        if (sq.nact < SEQ_ACT) {
            seq_act_t* a = &sq.act[sq.nact++];
            seq_put(0x90u | e.ch, e.note, e.vel);
            a->off = e.at + e.len;
            a->note = e.note;
            a->ch = e.ch;
            a->src = e.src;
            a->tie = e.len == 0u;
        }
        sq.pend[i] = sq.pend[--sq.npend];
    }
    for (i = 0; i < sq.nact;) {
        if (sq.act[i].tie || !SEQ_DUE(sq.act[i].off)) {
            i++;
            continue;
        }
        if (!midi_out_room())
            return 0;
        seq_put(0x80u | sq.act[i].ch, sq.act[i].note, 0);
        sq.act[i] = sq.act[--sq.nact];
    }
    return 1;
}

static void seq_pend(uint32_t at, uint32_t len, uint32_t note, uint32_t vel, uint32_t ch, uint32_t src)
{
    seq_pend_t* e;
    if (sq.npend >= SEQ_PEND || note > 127u)
        return;
    e = &sq.pend[sq.npend++];
    e->at = at;
    e->len = len;
    e->note = (uint8_t)note;
    e->vel = (uint8_t)(vel ? vel : 1u);
    e->ch = (uint8_t)ch;
    e->src = (uint8_t)src;
}

/* a source's notes: those to come dropped, those sounding ended now */
static void seq_flush(uint32_t src)
{
    uint32_t i;
    for (i = 0; i < sq.npend;)
        if (sq.pend[i].src == src)
            sq.pend[i] = sq.pend[--sq.npend];
        else
            i++;
    for (i = 0; i < sq.nact; i++)
        if (sq.act[i].src == src) {
            sq.act[i].tie = 0;
            sq.act[i].off = sq.pos;
        }
}

/* the timeline jumps to `to`; what is scheduled keeps its distance */
static void seq_rebase(uint32_t to)
{
    const uint32_t d = to - sq.pos;
    uint32_t i;
    for (i = 0; i < sq.npend; i++)
        sq.pend[i].at += d;
    for (i = 0; i < sq.nact; i++)
        sq.act[i].off += d;
    sq.arp_next += d;
    sq.pos = to;
}

/* ---- the pattern */

static void seq_step(void)
{
    const seq_pattern_t* p = &seq_pat[seq_pat_i];
    const uint32_t grid = sq.next_step;
    uint32_t k = sq.nxt < p->len ? sq.nxt : 0u, at = grid, i;
    const seq_step_t s = p->s[k];
    const uint32_t note = s.note & 0x7Fu, tie = s.vel & SEQ_TIE, len = tie ? 0u : seq_gate_units(s.gate);
    int play = (s.note & SEQ_ON) && !(sq.skip >> k & 1u);
    sq.skip &= ~(1u << k);
    if (k & 1u)
        at += seq_swing_units(p->swing);
    for (i = 0; i < sq.nact; i++) {         /* a tied note: goes on into the same note, else ends where this starts */
        seq_act_t* a = &sq.act[i];
        if (a->src != SRC_SEQ || !a->tie)
            continue;
        if (play && a->note == note && a->ch == p->ch) {
            a->tie = tie != 0u;
            a->off = at + len;
            play = 0;
        } else {
            a->tie = 0;
            a->off = play ? at : grid;
        }
    }
    if (play)
        seq_pend(at, len, note, s.vel & 0x7Fu, p->ch, SRC_SEQ);
    seq_cur = (int32_t)k;
    sq.next_step = grid + SEQ_STEP;
    k = k + 1u >= p->len ? 0u : k + 1u;
    if (!k && seq_pat_next != seq_pat_i) {  /* a new pattern starts with the bar */
        seq_pat_i = seq_pat_next;
        sq.skip = 0;
    }
    sq.nxt = k;
}

/* ---- the arpeggiator */

static void arp_step(void)
{
    uint8_t list[SEQ_HELD];
    const uint32_t n = sq.nheld, oct = arp_oct < 1 ? 1u : arp_oct > 4 ? 4u : (uint32_t)arp_oct, t = n * oct;
    const uint32_t rate = ARP_RATE[arp_rate < 0 || arp_rate >= ARP_RATES ? 3 : arp_rate];
    uint32_t i, j, idx = sq.arp_i, v = 100u;
    if (!n)
        return;
    for (i = 0; i < n; i++)                 /* sorted, unless as played */
        list[i] = sq.held[i];
    if (arp_mode != ARP_PLAYED)
        for (i = 1; i < n; i++)
            for (j = i; j && list[j - 1u] > list[j]; j--) {
                const uint8_t x = list[j];
                list[j] = list[j - 1u];
                list[j - 1u] = x;
            }
    switch (arp_mode) {
    case ARP_DOWN:
        idx = t - 1u - idx % t;
        break;
    case ARP_UPDOWN:
        idx = t > 1u ? idx % (2u * t - 2u) : 0u;
        idx = idx < t ? idx : 2u * t - 2u - idx;
        break;
    case ARP_RANDOM:
        sq.rnd = sq.rnd * 1664525u + 1013904223u;
        idx = (sq.rnd >> 16) % t;
        break;
    default:
        idx %= t;
        break;
    }
    for (i = 0; i < n; i++)
        if (sq.held[i] == list[idx % n])
            v = sq.held_vel[i];
    seq_pend(sq.arp_next, rate * (uint32_t)(arp_gate < 1 ? 1 : arp_gate > 100 ? 100 : arp_gate) / 100u,
             list[idx % n] + 12u * (idx / n), v, sq.arp_ch, SRC_ARP);
    sq.arp_i++;
}

static uint32_t arp_rate_units(void) { return ARP_RATE[arp_rate < 0 || arp_rate >= ARP_RATES ? 3 : arp_rate]; }

/* the arp's first note: on the grid while the pattern plays, else now */
static void arp_begin(void)
{
    sq.arp_live = 1;
    sq.arp_i = 0;
    sq.arp_next = seq_run ? seq_ceil(sq.pos, arp_rate_units()) : sq.pos;
}

static void arp_end(void)
{
    uint32_t i;
    sq.arp_live = 0;
    for (i = 0; i < sq.npend;)              /* to come: dropped; sounding: their gate */
        if (sq.pend[i].src == SRC_ARP)
            sq.pend[i] = sq.pend[--sq.npend];
        else
            i++;
}

/* ---- the transport */

static void seq_begin(uint32_t from)        /* the pattern plays from `from` on (on its grid) */
{
    seq_pat_i = seq_pat_next;
    sq.next_step = seq_ceil(from, SEQ_STEP);
    sq.nxt = sq.next_step / SEQ_STEP % seq_pat[seq_pat_i].len;
    sq.skip = 0;
    seq_run = 1;
    if (sq.arp_live)
        sq.arp_next = seq_ceil(sq.pos, arp_rate_units());
}

static void seq_halt(void)
{
    if (!seq_run)
        return;
    seq_run = 0;
    seq_cur = -1;
    sq.stop_pos = sq.pos;
    seq_flush(SRC_SEQ);
    if (!seq_ext && seq_clk_out)
        seq_put(0xFCu, 0, 0);
}

static void seq_due(void)                   /* everything that fell due by sq.pos */
{
    if (seq_run && !seq_ext && seq_clk_out)
        while (SEQ_DUE(sq.next_clk) && midi_out_room()) {
            seq_put(0xF8u, 0, 0);
            sq.next_clk += SEQ_CLK;
        }
    while (seq_run && SEQ_DUE(sq.next_step))
        seq_step();
    while (sq.arp_live && SEQ_DUE(sq.arp_next)) {
        arp_step();
        sq.arp_next += arp_rate_units();
    }
    seq_fire();
}

static void seq_ext_gone(void)
{
    seq_halt();
    seq_ext = 0;
    seq_armed = 0;
    sq.ext_run = sq.ext_wait = 0;
    sq.ext_q4 = 0;
    sq.acc = 0;
}

/* TIMER5, every 0.1 ms */
static void seq_tick(void)
{
    sq.now++;
    if (seq_ext && (sq.now - sq.ext_last > SEQ_EXT_LOST || seq_sync))
        seq_ext_gone();
    if (seq_ext) {                          /* between clocks: on at the last period's pace, never past the next */
        if (sq.ext_q4) {
            sq.acc += SEQ_CLK * 16u;
            while (sq.acc >= sq.ext_q4 && (int32_t)(sq.clk_pos + SEQ_CLK - 1u - sq.pos) > 0) {
                sq.acc -= sq.ext_q4;
                sq.pos++;
            }
            if (sq.acc > sq.ext_q4)
                sq.acc = sq.ext_q4;
        }
    } else {
        sq.acc += (uint32_t)(seq_bpm < 30 ? 30 : seq_bpm > 300 ? 300 : seq_bpm) * SEQ_PPQ;
        while (sq.acc >= 60u * SEQ_TICK_HZ) {
            sq.acc -= 60u * SEQ_TICK_HZ;
            sq.pos++;
        }
    }
    seq_due();
}

/* TIMER5 (the USB poll): MIDI clock F8, start FA, continue FB, stop FC */
static void seq_midi_rt(uint32_t st)
{
    if (seq_sync)
        return;
    if (st == 0xF8u) {
        const uint32_t period = sq.now - sq.ext_last;
        const int first = !seq_ext;
        if (first) {                        /* the clock arrives: the internal one stops, PLAY stays meant */
            if (seq_run)
                seq_armed = 1;
            seq_halt();
            seq_ext = 1;
            sq.ext_q4 = 0;
            sq.clk_pos = sq.pos;
        } else if (period < SEQ_EXT_LOST) {
            sq.ext_q4 = sq.ext_q4 ? (sq.ext_q4 * 3u + period * 16u) / 4u : period * 16u;
        }
        sq.ext_last = sq.now;
        if (sq.ext_wait) {                  /* the first clock after start / continue is the position */
            sq.ext_wait = 0;
            seq_rebase(sq.ext_cont ? seq_ceil(sq.stop_pos, SEQ_CLK) : 0u);
            sq.clk_pos = sq.pos;
            if (seq_armed)
                seq_begin(sq.pos);
        } else if (!first) {
            sq.clk_pos += SEQ_CLK;
            if ((int32_t)(sq.clk_pos - sq.pos) > 0)
                sq.pos = sq.clk_pos;
        }
        sq.acc = 0;
        seq_due();
    } else if (st == 0xFAu || st == 0xFBu) {
        if (seq_run) {                      /* (the clock may come only with the start: the first F8 decides) */
            if (!seq_ext)
                seq_armed = 1;
            seq_halt();
            seq_fire();
        }
        sq.ext_run = 1;
        sq.ext_wait = 1;
        sq.ext_cont = st == 0xFBu;
    } else if (st == 0xFCu) {
        sq.ext_run = 0;
        sq.ext_wait = 0;
        seq_halt();
        seq_fire();
    }
}

/* ---- the main loop's calls */

static void seq_pattern_clear(seq_pattern_t* p)
{
    uint32_t i;
    p->ver = SEQ_VER;
    p->len = 16;
    p->ch = 0;
    p->swing = 50;
    for (i = 0; i < SEQ_STEPS; i++) {
        p->s[i].note = 60;
        p->s[i].vel = 100;
        p->s[i].gate = 50;
    }
}

static void seq_init(void)
{
    uint32_t i;
    for (i = 0; i < SEQ_PATTERNS; i++)
        seq_pattern_clear(&seq_pat[i]);
    sq.rnd = 0x2545F491u;
}

/* PLAY: on the internal clock start / stop; on MIDI clock arm / disarm (armed, it plays while the clock runs) */
static void seq_play(void)
{
    fm1_irq_off();
    if (seq_ext) {
        seq_armed = !seq_armed;
        if (!seq_armed)
            seq_halt();
        else if (sq.ext_run && !sq.ext_wait)
            seq_begin(sq.pos);              /* joins the running clock at the next step */
    } else if (seq_run) {
        seq_halt();
    } else {
        sq.acc = 0;
        seq_rebase(0);
        sq.next_clk = 0;
        if (seq_clk_out)
            seq_put(0xFAu, 0, 0);
        seq_begin(0);
    }
    seq_due();
    fm1_irq_on();
}

/* a pattern (0..SEQ_PATTERNS-1): playing, it follows at the end of this one */
static void seq_select(uint32_t pat)
{
    if (pat >= SEQ_PATTERNS)
        return;
    fm1_irq_off();
    seq_pat_next = (uint8_t)pat;
    if (!seq_run)
        seq_pat_i = (uint8_t)pat;
    fm1_irq_on();
}

/* the arp's keys: a key down / up (note 0..127, vel 1..127) on channel ch (0..15) */
static void seq_arp_key(uint32_t note, uint32_t vel, uint32_t ch, int down)
{
    uint32_t i;
    fm1_irq_off();
    sq.arp_ch = (uint8_t)ch;
    if (down) {
        if (arp_latch && !sq.nphys)         /* latched: a new chord replaces the last */
            sq.nheld = 0;
        sq.nphys++;
        for (i = 0; i < sq.nheld && sq.held[i] != note; i++)
            ;
        if (i == sq.nheld && sq.nheld < SEQ_HELD) {
            sq.held[sq.nheld] = (uint8_t)note;
            sq.held_vel[sq.nheld++] = (uint8_t)vel;
        }
        if (!sq.arp_live && arp_on)
            arp_begin();                    /* (its first note with the next tick: the frame's other keys are in) */
    } else {
        if (sq.nphys)
            sq.nphys--;
        if (!arp_latch)
            for (i = 0; i < sq.nheld; i++)
                if (sq.held[i] == note) {
                    for (; i + 1u < sq.nheld; i++) {
                        sq.held[i] = sq.held[i + 1u];
                        sq.held_vel[i] = sq.held_vel[i + 1u];
                    }
                    sq.nheld--;
                    break;
                }
        if (!sq.nheld && sq.arp_live)
            arp_end();
    }
    fm1_irq_on();
}

/* the arp off: its notes end; latch off: what is no longer held goes */
static void seq_arp_changed(void)
{
    fm1_irq_off();
    if (!arp_on) {
        sq.nheld = sq.nphys = 0;
        arp_end();
        seq_flush(SRC_ARP);
    } else if (!arp_latch && !sq.nphys) {
        sq.nheld = 0;
        arp_end();
    }
    fm1_irq_on();
}

/* REC while it plays: a key goes into the step it is nearest to (an early key into the next step, which then does not
   play this time round: the key sounds already); its length becomes the gate. 1 when recorded */
static int seq_rec_key(uint32_t note, uint32_t vel, int down)
{
    seq_pattern_t* p;
    uint32_t i, k, d;
    int done = 0;
    fm1_irq_off();
    p = &seq_pat[seq_pat_i];
    if (down && seq_run && seq_cur >= 0) {
        k = (uint32_t)seq_cur;
        if (sq.pos - (sq.next_step - SEQ_STEP) >= SEQ_STEP / 2u) {
            k = sq.nxt;
            sq.skip |= 1u << k;
        }
        if (k < p->len) {
            p->s[k].note = (uint8_t)(note | SEQ_ON);
            p->s[k].vel = (uint8_t)(vel ? vel : 1u);
            p->s[k].gate = 50;
            for (i = 0; i < SEQ_RECN && sq.rec_note[i]; i++)
                ;
            if (i < SEQ_RECN) {
                sq.rec_note[i] = (uint8_t)(note | SEQ_ON);
                sq.rec_step[i] = (uint8_t)k;
                sq.rec_at[i] = sq.pos;
            }
            done = 1;
        }
    } else if (!down) {
        for (i = 0; i < SEQ_RECN; i++)
            if (sq.rec_note[i] == (note | SEQ_ON)) {
                d = (sq.pos - sq.rec_at[i]) * 100u / SEQ_STEP;
                p->s[sq.rec_step[i]].gate = (uint8_t)(d < 1u ? 1u : d > 100u ? 100u : d);
                sq.rec_note[i] = 0;
            }
    }
    fm1_irq_on();
    return done;
}

/* the tempo the pattern plays at, in tenths of a BPM (MIDI clock: measured) */
static int32_t seq_tempo10(void)
{
    const uint32_t q4 = sq.ext_q4;
    if (!seq_ext)
        return seq_bpm * 10;
    return q4 ? (int32_t)((4000000u + q4 / 2u) / q4) : 0;   /* 60 s * 10 kHz / 24 clocks, x 16 (Q4), x 10 */
}

/* ---- the patterns as blobs (the preset store): SEQ_BLOB bytes each */

static uint32_t seq_blob_get(uint32_t pat, void* dst)
{
    if (pat >= SEQ_PATTERNS)
        return 0;
    memcpy(dst, &seq_pat[pat], SEQ_BLOB);
    return SEQ_BLOB;
}

/* 0, or -1: not a pattern of this version (nothing changed) */
static int seq_blob_set(uint32_t pat, const void* src, uint32_t n)
{
    seq_pattern_t p;
    uint32_t i;
    if (pat >= SEQ_PATTERNS || n != SEQ_BLOB)
        return -1;
    memcpy(&p, src, SEQ_BLOB);
    if (p.ver != SEQ_VER)
        return -1;
    p.len = p.len < 1u ? 1u : p.len > SEQ_STEPS ? (uint8_t)SEQ_STEPS : p.len;
    p.ch &= 15u;
    p.swing = p.swing < 50u ? 50u : p.swing > 75u ? 75u : p.swing;
    for (i = 0; i < SEQ_STEPS; i++) {
        if (!(p.s[i].vel & 0x7Fu))
            p.s[i].vel |= 100u;
        p.s[i].gate = p.s[i].gate < 1u ? 50u : p.s[i].gate > 100u ? 100u : p.s[i].gate;
    }
    fm1_irq_off();
    seq_pat[pat] = p;
    fm1_irq_on();
    return 0;
}

#endif
