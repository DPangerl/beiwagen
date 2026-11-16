/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl */
/* The sequencer and arpeggiator (apps/beiwagen/bw_seq.h) on a simulated TIMER5 (10 kHz): the MIDI it queues, with the
 * tick it was queued at, checked against the ideal times: note-ons and -offs at a tempo, swing, a 24-ppqn clock in
 * (start, stop, continue), the arp's orders, live recording, the blobs, and no hanging note after stop, a pattern or
 * channel change, ties, the arp let go, a full out ring. The USB poll (2 kHz) is modelled for the jitter on the wire.
 * Run by tools/seq_test.py. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fm1_irq_off(void) {}
static void fm1_irq_on(void) {}

#define LOG_MAX 20000
static struct {
    uint32_t t, st, d1, d2;
} out[LOG_MAX];
static int nout, failures;
static uint32_t room = 64, cap = 64;     /* the ring's free slots; refilled each tick (the USB poll) */
static uint32_t ticks;                      /* the test's own clock (seq's sq.now may be rebased) */

static void midi_out_event(uint32_t pkt)
{
    if (nout < LOG_MAX) {
        out[nout].t = ticks;
        out[nout].st = pkt >> 8 & 0xFFu;
        out[nout].d1 = pkt >> 16 & 0xFFu;
        out[nout].d2 = pkt >> 24 & 0xFFu;
        nout++;
    }
    if (room)
        room--;
}
static uint32_t midi_out_room(void) { return room; }

#include "bw_seq.h"

#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: ", __func__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void reset(void)
{
    memset(&sq, 0, sizeof sq);
    seq_bpm = 120;
    seq_sync = seq_clk_out = 0;
    arp_on = arp_mode = arp_latch = 0;
    arp_rate = 3;
    arp_gate = 50;
    arp_oct = 1;
    seq_cur = -1;
    seq_run = seq_armed = seq_ext = seq_rec = 0;
    seq_pat_i = seq_pat_next = 0;
    seq_init();
    nout = 0;
    room = cap = 64;
    ticks = 0;
}

static void run(uint32_t n)
{
    while (n--) {
        ticks++;
        room = cap;
        seq_tick();
    }
}

static int is_on(int i) { return (out[i].st & 0xF0u) == 0x90u && out[i].d2; }
static int is_off(int i) { return (out[i].st & 0xF0u) == 0x80u || ((out[i].st & 0xF0u) == 0x90u && !out[i].d2); }

/* every note-on has its note-off, none comes twice before it; 0 when all ended */
static int hanging(const char* what)
{
    static int sounding[16][128];
    int i, c, n, bad = 0;
    memset(sounding, 0, sizeof sounding);
    for (i = 0; i < nout; i++) {
        c = (int)(out[i].st & 15u);
        n = (int)out[i].d1;
        if (is_on(i)) {
            CHECK(!sounding[c][n], "%s: note-on %d ch %d while it sounds (t %u)", what, n, c + 1, out[i].t);
            sounding[c][n] = 1;
        } else if (is_off(i)) {
            CHECK(sounding[c][n], "%s: note-off %d ch %d that was not on (t %u)", what, n, c + 1, out[i].t);
            sounding[c][n] = 0;
        }
    }
    for (c = 0; c < 16; c++)
        for (n = 0; n < 128; n++)
            if (sounding[c][n]) {
                printf("FAIL %s: note %d ch %d hangs\n", what, n, c + 1);
                failures++;
                bad++;
            }
    return bad;
}

static void all_on(uint32_t pat, uint32_t base)
{
    uint32_t i;
    for (i = 0; i < SEQ_STEPS; i++)
        seq_pat[pat].s[i].note = (uint8_t)((base + i % 12u) | SEQ_ON);
}

/* ---- timing: note-ons and -offs at a tempo; the error from the ideal, in ticks, max (engine and on the wire) */
static double max_err_us, max_wire_us;

static void timing(int bpm, int swing, int gate)
{
    const double step_ticks = 60.0 * SEQ_TICK_HZ / bpm / 4.0;
    int i, ons = 0, offs = 0;
    double worst = 0, wire = 0;
    reset();
    seq_bpm = bpm;
    all_on(0, 60);
    seq_pat[0].swing = (uint8_t)swing;
    for (i = 0; i < (int)SEQ_STEPS; i++)
        seq_pat[0].s[i].gate = (uint8_t)gate;
    run(37);                                /* (any phase) */
    {
        const uint32_t t0 = ticks;
        seq_play();
        uint32_t t_stop;
        run((uint32_t)(step_ticks * 64 - 0.5));
        t_stop = ticks;
        seq_play();
        run(10);
        for (i = 0; i < nout; i++) {
            const int k = is_on(i) ? ons : offs;
            const double sw = (k & 1) ? (double)((swing - 50) * 480 / 100) / SEQ_PPQ * 4.0 * step_ticks : 0.0;
            const double gl = (double)(gate * 240 / 100) / SEQ_PPQ * 4.0 * step_ticks;
            double ideal = t0 + k * step_ticks + sw + (is_on(i) ? 0.0 : gl), e, w;
            if (!is_on(i) && !is_off(i))
                continue;
            if (is_off(i) && ideal > t_stop)
                continue;                   /* ended by the stop */
            e = out[i].t - ideal;
            e = e < 0 ? -e : e;
            w = (double)((out[i].t + 4u) / 5u * 5u) - ideal;   /* the next USB poll (2 kHz) takes it */
            w = w < 0 ? -w : w;
            worst = e > worst ? e : worst;
            wire = w > wire ? w : wire;
            if (is_on(i))
                ons++;
            else
                offs++;
        }
    }
    CHECK(ons == 64, "%d BPM: %d note-ons in 64 steps", bpm, ons);
    CHECK(worst <= 1.0, "%d BPM swing %d gate %d: %.2f ticks off the ideal", bpm, swing, gate, worst);
    hanging("timing");
    printf("timing %3d BPM swing %d gate %3d: %d on / %d off, max %.0f us from ideal (engine), %.0f us at the USB poll\n",
           bpm, swing, gate, ons, offs, worst * 100.0, wire * 100.0);
    max_err_us = worst * 100.0 > max_err_us ? worst * 100.0 : max_err_us;
    max_wire_us = wire * 100.0 > max_wire_us ? wire * 100.0 : max_wire_us;
}

/* ---- MIDI clock in: 24 ppqn at a tempo, with the given jitter (ticks); steps land on every 6th clock */
static uint32_t clk_t[4096];
static int nclk;

static void clocks(double bpm, int n, int jitter, int record)
{
    const double period = 60.0 * SEQ_TICK_HZ / bpm / 24.0;
    static double next;
    int i;
    if (!next || !record)
        next = ticks + 1;
    for (i = 0; i < n; i++) {
        const uint32_t at = (uint32_t)(next + (jitter ? rand() % (2 * jitter + 1) - jitter : 0));
        while (ticks < at)
            run(1);
        seq_midi_rt(0xF8u);
        if (record && nclk < 4096)
            clk_t[nclk++] = ticks;
        next += period;
    }
}

static void ext_clock(void)
{
    int i, ons = 0, ons_after = 0;
    double worst = 0;
    uint32_t stop_t;
    reset();
    all_on(0, 48);
    seq_pat[0].swing = 66;
    clocks(100, 48, 0, 0);                  /* the clock comes, stopped: PLAY arms */
    CHECK(seq_ext, "clock not seen");
    CHECK(seq_tempo10() >= 995 && seq_tempo10() <= 1005, "tempo %d / 10 BPM, not 100", (int)seq_tempo10());
    seq_play();
    CHECK(seq_armed && !seq_run, "PLAY on the clock: armed %d run %d", seq_armed, seq_run);
    nout = 0;
    seq_midi_rt(0xFAu);
    nclk = 0;
    clocks(100, 24 * 8, 0, 1);              /* 8 beats = 32 steps */
    for (i = 0; i < nout; i++)
        if (is_on(i)) {
            const uint32_t c = clk_t[6 * ons];
            double e;
            if (ons & 1) {                  /* swung: 76 units = 1.9 clocks past the clock, interpolated */
                const double per = (double)(clk_t[6 * ons + 2] - c) / 2.0;
                e = (double)out[i].t - (c + 1.9 * per);
            } else {
                e = (double)out[i].t - c;
            }
            e = e < 0 ? -e : e;
            worst = e > worst ? e : worst;
            ons++;
        }
    CHECK(ons == 32, "clocked: %d note-ons for 32 steps", ons);
    CHECK(worst <= 2.0, "clocked: %.1f ticks off (on the clock: 0, swung: interpolated)", worst);
    printf("clock in 100 BPM: %d note-ons, max %.0f us from the clock (straight steps on the clock's tick, swung "
           "ones interpolated)\n", ons, worst * 100.0);
    seq_midi_rt(0xFCu);
    stop_t = ticks;
    clocks(100, 24, 0, 0);
    for (i = 0; i < nout; i++) {
        if (out[i].t >= stop_t && is_on(i))
            ons_after++;
    }
    CHECK(!ons_after, "%d note-ons after stop", ons_after);
    CHECK(!seq_run && seq_armed, "after stop: run %d armed %d", seq_run, seq_armed);
    hanging("clock stop");
    /* continue: on from where it stopped, 32 steps in -> step 0 of the 16 (at the next step after the position) */
    nout = 0;
    seq_midi_rt(0xFBu);
    clocks(100, 1, 0, 0);
    CHECK(seq_run, "continue: not running");
    clocks(100, 24, 0, 0);
    seq_midi_rt(0xFCu);
    run(100);
    hanging("clock continue");
    /* with jitter (+-0.5 ms per clock): steps still on their clock */
    reset();
    all_on(0, 48);
    clocks(120, 30, 5, 0);
    seq_play();
    nout = 0;
    nclk = 0;
    seq_midi_rt(0xFAu);
    clocks(120, 24 * 4, 5, 1);
    worst = 0;
    for (i = 0, ons = 0; i < nout; i++)
        if (is_on(i)) {
            const double e = (double)out[i].t - clk_t[6 * ons++];
            worst = (e < 0 ? -e : e) > worst ? (e < 0 ? -e : e) : worst;
        }
    CHECK(ons == 16 && worst == 0, "jittery clock: %d ons, %.0f ticks off their clock", ons, worst);
    /* the clock goes away while it plays: it stops, no note hangs */
    run(SEQ_EXT_LOST + 10);
    CHECK(!seq_ext && !seq_run, "clock lost: ext %d run %d", seq_ext, seq_run);
    hanging("clock lost");
    /* PLAY before the start; a start before the first clock (sent only while playing) */
    reset();
    all_on(0, 48);
    seq_midi_rt(0xFAu);
    clocks(120, 24, 0, 0);
    CHECK(!seq_run, "not armed: it must not play");
    seq_midi_rt(0xFCu);
    seq_play();
    seq_midi_rt(0xFAu);
    clocks(120, 24, 0, 0);
    CHECK(seq_run, "armed, start, clock: not playing");
    seq_midi_rt(0xFCu);
    run(10);
    hanging("start first");
}

/* ---- clock out */
static void clock_out(void)
{
    int i, f8 = 0, fa = 0, fc = 0;
    uint32_t first = 0, last = 0;
    reset();
    seq_clk_out = 1;
    seq_bpm = 125;                          /* a clock: 20 ms = 200 ticks */
    seq_play();
    run(10000);
    seq_play();
    for (i = 0; i < nout; i++) {
        if (out[i].st == 0xF8u) {
            if (!f8)
                first = out[i].t;
            last = out[i].t;
            f8++;
        }
        fa += out[i].st == 0xFAu;
        fc += out[i].st == 0xFCu;
    }
    CHECK(fa == 1 && fc == 1, "start %d stop %d", fa, fc);
    CHECK(f8 == 51 && last - first == 50u * 200u, "clocks %d over %u ticks", f8, last - first);
    CHECK(out[0].st == 0xFAu && out[1].st == 0xF8u, "start, then the first clock");
}

/* ---- the arp */
static void arp_case(int mode, int oct, const uint8_t* keys, int nk, const uint8_t* want, int nw, const char* name)
{
    int i, got = 0;
    reset();
    arp_on = 1;
    arp_mode = mode;
    arp_oct = oct;
    for (i = 0; i < nk; i++)
        seq_arp_key(keys[i], 90, 2, 1);
    run(1250 * (uint32_t)nw - 10);
    for (i = 0; i < nout && got < nw; i++)
        if (is_on(i)) {
            if (want)
                CHECK(out[i].d1 == want[got], "%s: note %d is %u, want %u", name, got, out[i].d1, want[got]);
            else
                CHECK(out[i].d1 == 60 || out[i].d1 == 64 || out[i].d1 == 67, "%s: note %u not held", name, out[i].d1);
            CHECK((out[i].st & 15u) == 2u, "%s: channel %u", name, (out[i].st & 15u) + 1u);
            CHECK(out[i].t == (got ? 1250u * (uint32_t)got : 1u), "%s: note %d at %u", name, got, out[i].t);
            got++;
        }
    CHECK(got == nw, "%s: %d notes", name, got);
    for (i = 0; i < nk; i++)
        seq_arp_key(keys[i], 0, 2, 0);
    run(2000);
    hanging(name);
    printf("arp %-8s ok (%d notes)\n", name, got);
}

static void arp(void)
{
    static const uint8_t K[3] = { 67, 60, 64 };
    static const uint8_t UP[6] = { 60, 64, 67, 60, 64, 67 }, DOWN[6] = { 67, 64, 60, 67, 64, 60 };
    static const uint8_t UD[8] = { 60, 64, 67, 64, 60, 64, 67, 64 }, PL[6] = { 67, 60, 64, 67, 60, 64 };
    static const uint8_t UP2[7] = { 60, 64, 67, 72, 76, 79, 60 }, UD2[10] = { 60, 64, 67, 72, 76, 79, 76, 72, 67, 64 };
    int i, n;
    arp_case(ARP_UP, 1, K, 3, UP, 6, "up");
    arp_case(ARP_DOWN, 1, K, 3, DOWN, 6, "down");
    arp_case(ARP_UPDOWN, 1, K, 3, UD, 8, "up-down");
    arp_case(ARP_PLAYED, 1, K, 3, PL, 6, "played");
    arp_case(ARP_UP, 2, K, 3, UP2, 7, "up 2 oct");
    arp_case(ARP_UPDOWN, 2, K, 3, UD2, 10, "ud 2 oct");
    arp_case(ARP_RANDOM, 1, K, 3, 0, 12, "random");
    /* latch: let go, it goes on; a new chord replaces the old */
    reset();
    arp_on = arp_latch = 1;
    seq_arp_key(60, 100, 0, 1);
    seq_arp_key(64, 100, 0, 1);
    seq_arp_key(60, 0, 0, 0);
    seq_arp_key(64, 0, 0, 0);
    run(1250 * 4);
    CHECK(sq.arp_live && sq.nheld == 2, "latch: live %d held %u", sq.arp_live, sq.nheld);
    seq_arp_key(72, 100, 0, 1);
    nout = 0;
    run(1250 * 3);
    for (i = n = 0; i < nout; i++)
        if (is_on(i)) {
            CHECK(out[i].d1 == 72, "latch: new chord plays %u", out[i].d1);
            n++;
        }
    CHECK(n == 3, "latch: %d notes", n);
    arp_on = 0;                             /* the arp off mid-note: its note ends */
    seq_arp_changed();
    run(10);
    nout = 0;
    run(5000);
    CHECK(nout == 0, "arp off: %d events after", nout);
    /* the arp on the running pattern: on its grid */
    reset();
    arp_on = 1;
    seq_play();
    run(300);
    seq_arp_key(60, 100, 0, 1);
    run(2000);
    for (i = 0; i < nout && !is_on(i); i++)
        ;
    CHECK(i < nout && out[i].t == 1250u, "arp joins the running grid at %u, want 1250", i < nout ? out[i].t : 0u);
    seq_arp_key(60, 0, 0, 0);
    seq_play();
    run(2000);
    hanging("arp on the grid");
}

/* ---- no hanging notes: ties, a channel change, a pattern change, a stop, a full ring */
static void hang(void)
{
    uint32_t i;
    reset();
    all_on(0, 60);
    all_on(1, 40);
    seq_pat[1].ch = 5;
    for (i = 0; i < SEQ_STEPS; i += 3)
        seq_pat[0].s[i].vel |= SEQ_TIE;     /* ties, into other notes */
    seq_pat[0].s[4].note = 60 | SEQ_ON;     /* and into the same */
    seq_pat[0].s[3].note = 60 | SEQ_ON;
    seq_pat[0].s[3].vel |= SEQ_TIE;
    seq_play();
    run(20000);
    seq_pat[0].ch = 3;                      /* the channel, while notes sound */
    run(3000);
    seq_select(1);                          /* the pattern, at the bar */
    run(25000);
    CHECK(seq_pat_i == 1, "pattern change: still %u", seq_pat_i);
    seq_pat[1].s[5].vel |= SEQ_TIE;
    run(9000);
    seq_play();                             /* stop, mid-note */
    run(10);
    hanging("ties / channel / pattern / stop");
    /* a full ring: the events wait, nothing is lost */
    reset();
    all_on(0, 60);
    seq_play();
    run(5000);
    cap = 1;                                /* (one slot a tick, for 2 s) */
    run(20000);
    cap = 64;
    run(3000);
    seq_play();
    run(100);
    hanging("full ring");
    /* tied: the same note goes on, one note-on for both steps */
    reset();
    seq_pat[0].s[0].note = 62 | SEQ_ON;
    seq_pat[0].s[0].vel = 100 | SEQ_TIE;
    seq_pat[0].s[1].note = 62 | SEQ_ON;
    seq_play();
    run(1250 * 3);
    seq_play();
    run(10);
    CHECK(nout == 2 && is_on(0) && is_off(1) && out[1].t == 1250u + 625u, "tie: %d events, off at %u", nout,
          nout > 1 ? out[1].t : 0u);
    hanging("tie same note");
}

/* ---- live recording: a key nearest a step; ahead of the playhead: that step skips once */
static void rec(void)
{
    uint32_t i;
    int ons = 0;
    reset();
    seq_play();
    run(1250 * 2 + 300);                    /* in step 2, early: into step 2 */
    CHECK(seq_rec_key(70, 99, 1), "rec: not taken");
    run(625);
    seq_rec_key(70, 0, 0);
    CHECK(seq_pat[0].s[2].note == (70 | SEQ_ON) && seq_pat[0].s[2].vel == 99, "rec: step 2 is %02x", seq_pat[0].s[2].note);
    CHECK(seq_pat[0].s[2].gate >= 49 && seq_pat[0].s[2].gate <= 51, "rec: gate %u, want 50", seq_pat[0].s[2].gate);
    run(1250 * 2 - 300 - 625 + 1000);       /* late in step 4: into step 5, which then does not play this time */
    CHECK(seq_cur == 4, "rec: at step %d", (int)seq_cur);
    nout = 0;
    seq_rec_key(72, 100, 1);
    seq_rec_key(72, 0, 0);
    CHECK(seq_pat[0].s[5].note == (72 | SEQ_ON), "rec: step 5 is %02x", seq_pat[0].s[5].note);
    run(1250 * 2);
    for (i = 0; i < (uint32_t)nout; i++)
        ons += is_on((int)i);
    CHECK(ons == 0, "rec: step 5 played right after the key (%d note-ons)", ons);
    run(1250 * 16);
    for (i = 0, ons = 0; i < (uint32_t)nout; i++)
        ons += is_on((int)i) && out[i].d1 == 72;
    CHECK(ons == 1, "rec: step 5 next time round: %d", ons);
    seq_play();
    run(10);
    hanging("rec");
}

static void blobs(void)
{
    uint8_t b[SEQ_BLOB];
    reset();
    CHECK(SEQ_BLOB == 100u && SEQ_PATTERNS * SEQ_BLOB <= 3584u, "blob %u B, 27 of them %u B", SEQ_BLOB,
          SEQ_PATTERNS * SEQ_BLOB);
    all_on(3, 50);
    seq_pat[3].len = 12;
    seq_pat[3].swing = 60;
    CHECK(seq_blob_get(3, b) == SEQ_BLOB, "get");
    CHECK(!seq_blob_set(7, b, SEQ_BLOB) && !memcmp(&seq_pat[7], &seq_pat[3], SEQ_BLOB), "round trip");
    b[0] = 9;
    CHECK(seq_blob_set(8, b, SEQ_BLOB) == -1, "wrong version taken");
    b[0] = SEQ_VER;
    b[1] = 200;
    b[3] = 3;
    CHECK(!seq_blob_set(8, b, SEQ_BLOB) && seq_pat[8].len == SEQ_STEPS && seq_pat[8].swing == 50, "clamped");
    printf("blobs: %u B a pattern, %u B for %u\n", SEQ_BLOB, SEQ_PATTERNS * SEQ_BLOB, SEQ_PATTERNS);
}

int main(void)
{
    srand(1);
    timing(120, 50, 50);
    timing(133, 50, 100);
    timing(97, 66, 30);
    timing(300, 75, 100);
    timing(30, 58, 1);
    ext_clock();
    clock_out();
    arp();
    hang();
    rec();
    blobs();
    printf("jitter: max %.0f us from the ideal in the engine, %.0f us at the USB poll (2 kHz)\n", max_err_us, max_wire_us);
    printf(failures ? "seq_test: %d FAILED\n" : "seq_test: ok\n", failures);
    return failures != 0;
}
