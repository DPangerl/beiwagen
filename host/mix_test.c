/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl */
/* The mixdown engine (apps/beiwagen/bw_mix.h) on the host, through its C API only, as app.c drives it: four synthetic
 * tracks (as usb_play_read_pairs gives them) -> bw_mix_process -> the volume -> bw_mix_output, the output looped back
 * onto one pair as the OP-1 does (a delay, a gain, the live playing on top). Checks unity level and the dB scale,
 * equal-power pan, mute and return (silent, no click), the sends (the delay's first repeat, a reverb tail), the
 * inserts at 2x and 1x, every parameter at 0 and 1000 (no value past full scale; UBSan: no NaN reaching the ints),
 * the blob, the return detection (found on the right track at the loop's delay, failed without a loop), and that a
 * loop on track 4 stays out of the mix once detected. Writes build/host/out/mix_*.wav. Run by tools/mix_test.py. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_mix.h"
#include "fw.h"

#define N FW_BLOCK
#define SEC(s) ((int)((s) * FW_FS))
#define MAXW SEC(40)
#define LOOPN 16384

static int failures;
static void check(int ok, const char *what)
{
    printf("  %s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static int16_t pairs[4 * 2 * N];
static int32_t out[2 * N];
static int16_t wav[2 * MAXW];
static int wav_n;
static double ph[4];
static double amp[4] = {8000, 8000, 8000, 8000};
static double hz[4] = {220, 330, 440, 550};
static int impulse_at = -1;      /* a sample of a click on track 1 at this block's frame 0 */
static int range_bad;

/* the loop: what the FM-1 sent comes back on pair loop_pair, gain loop_g, loop_d samples later */
static int loop_pair = -1, loop_d = 1234, loop_half; /* loop_half: half a sample more (two taps averaged) */
static double loop_g = 0.8;
static int32_t hist[LOOPN][2];
static uint32_t hist_w;

static void fill(void)
{
    for (int p = 0; p < 4; p++)
        for (int i = 0; i < N; i++) {
            double s = amp[p] * sin(ph[p]);
            ph[p] += 2.0 * M_PI * hz[p] / FW_FS;
            if (p == 0 && impulse_at >= 0 && i == 0) s = 20000.0;
            int32_t l = (int32_t)s, r = (int32_t)s;
            if (p == loop_pair) {
                const int32_t *h = hist[(hist_w + (uint32_t)i - (uint32_t)loop_d) & (LOOPN - 1)];
                const int32_t *g = loop_half ? hist[(hist_w + (uint32_t)i - (uint32_t)loop_d - 1u) & (LOOPN - 1)] : h;
                l += (int32_t)((h[0] + g[0]) * 0.5 * loop_g);
                r += (int32_t)((h[1] + g[1]) * 0.5 * loop_g);
            }
            pairs[p * 2 * N + 2 * i] = (int16_t)(l > 32767 ? 32767 : l < -32767 ? -32767 : l);
            pairs[p * 2 * N + 2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32767 ? -32767 : r);
        }
    impulse_at = -1;
}

/* one block: in, the mix, the output looped and kept */
static void run_block(void)
{
    fill();
    bw_mix_process(pairs, 4, out, N);
    bw_mix_output(out, N);
    for (int i = 0; i < N; i++) {
        for (int c = 0; c < 2; c++)
            if (out[2 * i + c] > 32767 || out[2 * i + c] < -32767) range_bad++;
        hist[(hist_w + (uint32_t)i) & (LOOPN - 1)][0] = out[2 * i];
        hist[(hist_w + (uint32_t)i) & (LOOPN - 1)][1] = out[2 * i + 1];
        if (wav_n < 2 * MAXW) {
            wav[wav_n++] = (int16_t)out[2 * i];
            wav[wav_n++] = (int16_t)out[2 * i + 1];
        }
    }
    hist_w += N;
}

/* RMS of each side over a number of blocks (after settling ones) */
static void measure(int settle, int blocks, double *l, double *r)
{
    double el = 0, er = 0;
    for (int b = 0; b < settle; b++) run_block();
    for (int b = 0; b < blocks; b++) {
        run_block();
        for (int i = 0; i < N; i++) {
            el += (double)out[2 * i] * out[2 * i];
            er += (double)out[2 * i + 1] * out[2 * i + 1];
        }
    }
    *l = sqrt(el / (blocks * N));
    *r = sqrt(er / (blocks * N));
}

static double db(double a, double b) { return 20.0 * log10((a + 1e-9) / (b + 1e-9)); }

static void solo(int t)
{
    for (int k = 0; k < 4; k++) bw_mix_set(BW_MIX_P(k, BW_MIX_MUTE), k != t ? 1000 : 0);
}

static void defaults(void)
{
    for (int p = 0; p < BW_MIX_PARAMS; p++) bw_mix_set(p, bw_mix_default(p));
}

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }
static void save(const char *name)
{
    char path[128];
    snprintf(path, sizeof path, "build/host/out/mix_%s.wav", name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        check(0, "write the WAV");
        return;
    }
    const uint32_t bytes = (uint32_t)wav_n * 2u;
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 2);
    put32(f, FW_FS);
    put32(f, FW_FS * 4);
    put16(f, 4);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, bytes);
    fwrite(wav, 2, (size_t)wav_n, f);
    fclose(f);
    printf("wav %s (%.1f s)\n", path, wav_n / 2.0 / FW_FS);
    wav_n = 0;
}

/* the largest step between neighbouring samples over some blocks */
static int max_step(int blocks)
{
    int top = 0, last = out[2 * N - 2];
    for (int b = 0; b < blocks; b++) {
        run_block();
        for (int i = 0; i < N; i++) {
            const int d = abs(out[2 * i] - last);
            top = d > top ? d : top;
            last = out[2 * i];
        }
    }
    return top;
}

static int detect(void)
{
    bw_mix_detect();
    for (int b = 0; b < SEC(5) / N && bw_mix_detect_state() == BW_MIX_DETECT_RUNNING; b++) run_block();
    return bw_mix_detect_state();
}

int main(void)
{
    char text[32];
    double l, r, l0, r0;
    bw_mix_init();

    printf("parameters\n");
    for (int p = 0; p < BW_MIX_PARAMS; p++) {
        if (p >= BW_MIX_TRACK_PARAMS && p < BW_MIX_DELAY_TIME) continue;
        printf("  %2d %-9s", p, bw_mix_param_name(p));
        for (int v = 0; v <= 1000; v += 250) {
            bw_mix_set(p, v);
            bw_mix_format(p, text, sizeof text);
            printf(" %4d: %-9s", v, text);
        }
        bw_mix_format(p, text, 4);
        if (strlen(text) > 3) check(0, "format overruns max");
        printf("\n");
    }
    defaults();

    printf("levels and pan (track 1 alone, 8000 peak)\n");
    solo(0);
    measure(4, 40, &l0, &r0);
    const double in_rms = 8000.0 / sqrt(2.0);
    printf("  0 dB: L %.1f dB R %.1f dB\n", db(l0, in_rms), db(r0, in_rms));
    check(fabs(db(l0, in_rms)) < 0.1 && fabs(db(r0, in_rms)) < 0.1, "level 900, centre: unity");
    bw_mix_set(BW_MIX_P(0, BW_MIX_LEVEL), 700);
    measure(4, 40, &l, &r);
    printf("  level 700 (%s): %.2f dB\n", (bw_mix_format(BW_MIX_P(0, BW_MIX_LEVEL), text, 32), text), db(l, l0));
    check(fabs(db(l, l0) + 12.0) < 0.1, "level 700 is -12 dB");
    bw_mix_set(BW_MIX_P(0, BW_MIX_LEVEL), 900);
    bw_mix_set(BW_MIX_P(0, BW_MIX_PAN), 0);
    measure(4, 40, &l, &r);
    printf("  hard left: L %+.2f dB, R %.1f dB; power %+.3f dB\n", db(l, l0), db(r, l0),
           10 * log10((l * l + r * r) / (l0 * l0 + r0 * r0)));
    check(fabs(db(l, l0) - 3.01) < 0.1 && r < 1.0, "hard left: +3 dB left, nothing right");
    bw_mix_set(BW_MIX_P(0, BW_MIX_PAN), 250);
    measure(4, 40, &l, &r);
    printf("  pan 250: power %+.3f dB\n", 10 * log10((l * l + r * r) / (l0 * l0 + r0 * r0)));
    check(fabs(10 * log10((l * l + r * r) / (l0 * l0 + r0 * r0))) < 0.05, "equal power");
    bw_mix_set(BW_MIX_P(0, BW_MIX_PAN), 500);

    printf("mute and return (a 220 Hz sine: the sine's own largest step is %d)\n",
           (int)(8000 * 2 * M_PI * 220 / FW_FS) + 1);
    measure(2, 2, &l, &r);
    const int sine_step = (int)(8000 * 2 * M_PI * 220 / FW_FS) + 2;
    bw_mix_set(BW_MIX_P(0, BW_MIX_MUTE), 1000);
    int step = max_step(4);
    measure(0, 10, &l, &r);
    check(l < 1.0 && r < 1.0, "muted: silent");
    check(step <= sine_step, "mute: no click");
    bw_mix_set(BW_MIX_P(0, BW_MIX_MUTE), 0);
    step = max_step(4);
    check(step <= sine_step, "unmute: no click");
    bw_mix_set(BW_MIX_P(0, BW_MIX_RETURN), 1000);
    measure(2, 10, &l, &r);
    check(l < 1.0 && r < 1.0, "the return track: out of the mix");
    bw_mix_set(BW_MIX_P(0, BW_MIX_RETURN), 0);

    printf("inserts (track 1, 2 kHz)\n");
    hz[0] = 2000;
    measure(4, 20, &l0, &r0);
    bw_mix_set(BW_MIX_P(0, BW_MIX_MODE), 1000 / 8); /* LP 12 */
    bw_mix_set(BW_MIX_P(0, BW_MIX_CUTOFF), 300);
    bw_mix_format(BW_MIX_P(0, BW_MIX_MODE), text, 32);
    measure(8, 20, &l, &r);
    printf("  %s at %d: %.1f dB\n", text, 300, db(l, l0));
    check(db(l, l0) < -12.0, "a lowpass under 2 kHz takes it down");
    bw_mix_set(BW_MIX_INSERTS, 1000);
    measure(8, 20, &l, &r);
    printf("  at the sample rate: %.1f dB\n", db(l, l0));
    check(db(l, l0) < -12.0, "the same at 1x");
    bw_mix_set(BW_MIX_P(0, BW_MIX_CUTOFF), 1000);
    bw_mix_set(BW_MIX_P(0, BW_MIX_DRIVE), 1000);
    measure(8, 20, &l, &r);
    printf("  open, full drive (1x): %.1f dB\n", db(l, l0));
    bw_mix_set(BW_MIX_INSERTS, 0);
    measure(8, 20, &l, &r);
    printf("  open, full drive (2x): %.1f dB\n", db(l, l0));
    bw_mix_set(BW_MIX_P(0, BW_MIX_MODE), 0);
    measure(8, 20, &l, &r);
    check(fabs(db(l, l0)) < 0.1, "insert off: as before");
    hz[0] = 220;
    save("levels");

    printf("sends (an impulse on track 1, the others muted)\n");
    amp[0] = 0;
    bw_mix_set(BW_MIX_P(0, BW_MIX_LEVEL), 0); /* the dry impulse out: the delay's repeat alone */
    bw_mix_set(BW_MIX_P(0, BW_MIX_SEND_DELAY), 1000);
    measure(2, 2, &l, &r);
    check(l < 1.0, "level off: the sends with it (post-fader)");
    bw_mix_set(BW_MIX_P(0, BW_MIX_LEVEL), 900);
    bw_mix_set(BW_MIX_P(0, BW_MIX_SEND_DELAY), 1000);
    bw_mix_set(BW_MIX_P(0, BW_MIX_LEVEL), 900);
    measure(0, 100, &l, &r); /* the tail of what came before */
    impulse_at = 0;
    int best = 0, best_v = 0, at = wav_n;
    for (int b = 0; b < SEC(0.6) / N; b++) run_block();
    for (int i = SEC(0.01); i < (wav_n - at) / 2; i++)
        if (abs(wav[at + 2 * i]) > best_v) {
            best_v = abs(wav[at + 2 * i]);
            best = i;
        }
    printf("  delay: first repeat at %.1f ms (%d)\n", best * 1000.0 / FW_FS, best_v);
    check(fabs(best * 1000.0 / FW_FS - 374.0) < 3.0 && best_v > 1000, "the delay's first repeat at its time");
    bw_mix_set(BW_MIX_P(0, BW_MIX_SEND_DELAY), 0);
    for (int b = 0; b < SEC(8) / N; b++) run_block();
    bw_mix_set(BW_MIX_P(0, BW_MIX_SEND_REVERB), 1000);
    measure(2, 2, &l, &r);
    impulse_at = 0;
    at = wav_n;
    for (int b = 0; b < SEC(1.0) / N; b++) run_block();
    double tail = 0;
    for (int i = SEC(0.3); i < SEC(0.5); i++) tail += (double)wav[at + 2 * i] * wav[at + 2 * i];
    printf("  reverb: tail 0.3-0.5 s at %.1f dB rms\n", 10 * log10(tail / SEC(0.2) + 1e-9) - 90.3);
    check(tail > 0, "a reverb tail");
    bw_mix_set(BW_MIX_P(0, BW_MIX_SEND_REVERB), 0);
    amp[0] = 8000;
    save("sends");

    printf("all four, every knob at 0 and at 1000, every insert mode\n");
    defaults();
    range_bad = 0;
    for (int t = 0; t < 4; t++) {
        bw_mix_set(BW_MIX_P(t, BW_MIX_SEND_DELAY), 500);
        bw_mix_set(BW_MIX_P(t, BW_MIX_SEND_REVERB), 500);
        bw_mix_set(BW_MIX_P(t, BW_MIX_PAN), 200 * t + 200);
        amp[t] = 30000;
    }
    measure(10, 100, &l, &r);
    printf("  four loud tracks, sends: L %.0f R %.0f rms\n", l, r);
    for (int p = 0; p < BW_MIX_PARAMS; p++)
        if (p % BW_MIX_TRACK_PARAMS != BW_MIX_MUTE && p % BW_MIX_TRACK_PARAMS != BW_MIX_RETURN) bw_mix_set(p, 1000);
    measure(0, SEC(3) / N, &l, &r);
    printf("  all 1000: L %.0f R %.0f rms\n", l, r);
    for (int p = 0; p < BW_MIX_PARAMS; p++) bw_mix_set(p, 0);
    measure(2, SEC(1) / N, &l, &r);
    printf("  all 0: L %.0f R %.0f rms\n", l, r);
    check(l < 1.0 && r < 1.0, "all 0 (master off): silent");
    defaults();
    for (int m = 0; m < 9; m++)
        for (int t = 0; t < 4; t++) {
            bw_mix_set(BW_MIX_P(t, BW_MIX_MODE), m * 1000 / 8);
            bw_mix_set(BW_MIX_P(t, BW_MIX_RESO), 1000);
            bw_mix_set(BW_MIX_P(t, BW_MIX_DRIVE), 1000);
            bw_mix_set(BW_MIX_P(t, BW_MIX_CUTOFF), 100 * m);
            bw_mix_set(BW_MIX_INSERTS, (m & 1) * 1000);
            measure(0, 20, &l, &r);
        }
    check(range_bad == 0, "no value past full scale");
    for (int t = 0; t < 4; t++) amp[t] = 8000;
    defaults();
    save("stress");

    printf("blob\n");
    uint8_t blob[BW_MIX_BLOB], small[8];
    bw_mix_set(BW_MIX_P(2, BW_MIX_PAN), 123);
    bw_mix_set(BW_MIX_REVERB_DECAY, 777);
    bw_mix_set(BW_MIX_P(3, BW_MIX_RETURN), 1000);
    const int len = bw_mix_save(blob, sizeof blob);
    check(len == BW_MIX_BLOB && !bw_mix_save(small, sizeof small), "saved, and refused when too small");
    defaults();
    check(bw_mix_load(blob, len) && bw_mix_get(BW_MIX_P(2, BW_MIX_PAN)) == 123 && bw_mix_get(BW_MIX_REVERB_DECAY) == 777,
          "loaded back");
    check(bw_mix_get(BW_MIX_P(3, BW_MIX_RETURN)) == 0, "the return flags left as they were");
    blob[0] = 'X';
    check(!bw_mix_load(blob, len) && !bw_mix_load(blob, 3), "not a blob: refused");
    printf("  %d bytes\n", len);
    defaults();

    printf("return detection\n");
    loop_pair = 3;
    measure(0, SEC(1) / N, &l, &r);
    printf("  a loop on track 4 (gain 0.8, %d samples), not detected yet: L %.0f rms\n", loop_d, l);
    int st = detect();
    printf("  state %d, track %d, delay %d\n", st, bw_mix_detect_track() + 1, bw_mix_detect_delay());
    check(st == BW_MIX_DETECT_FOUND && bw_mix_detect_track() == 3, "found on track 4");
    check(abs(bw_mix_detect_delay() - loop_d) <= 1, "at the loop's delay");
    check(bw_mix_get(BW_MIX_P(3, BW_MIX_RETURN)) == BW_MIX_MAX, "track 4's return flag set");
    /* the loop is out: muting the other three, the mix is silent although track 4 holds the mix */
    solo(3);
    measure(4, 40, &l, &r);
    check(l < 1.0, "track 4 out of the mix: no loop");
    defaults();
    save("detect");

    amp[3] = 0;
    loop_pair = 1;
    loop_d = 2700;
    loop_g = 0.3;
    for (int t = 0; t < 4; t++) amp[t] = t == 1 ? 16000 : 8000; /* loud playing on the loop's track too */
    st = detect();
    printf("  loop on track 2 (gain 0.3, 2700, loud): state %d, track %d, delay %d\n", st, bw_mix_detect_track() + 1,
           bw_mix_detect_delay());
    check(st == BW_MIX_DETECT_FOUND && bw_mix_detect_track() == 1 && abs(bw_mix_detect_delay() - 2700) <= 1,
          "found on track 2");
    check(bw_mix_get(BW_MIX_P(3, BW_MIX_RETURN)) == 0 && bw_mix_get(BW_MIX_P(1, BW_MIX_RETURN)) == BW_MIX_MAX,
          "the flag moved to track 2");
    loop_pair = 2;
    loop_d = 3000;
    loop_half = 1;
    st = detect();
    printf("  loop on track 3 (gain 0.3, 3000.5, smeared): state %d, track %d, delay %d\n", st,
           bw_mix_detect_track() + 1, bw_mix_detect_delay());
    check(st == BW_MIX_DETECT_FOUND && bw_mix_detect_track() == 2 && abs(bw_mix_detect_delay() - 3000) <= 3,
          "found on track 3 (the delay within 3: the second difference rings on a smeared peak)");
    loop_pair = -1;
    st = detect();
    printf("  no loop: state %d\n", st);
    check(st == BW_MIX_DETECT_FAILED, "no loop: failed");
    check(bw_mix_get(BW_MIX_P(2, BW_MIX_RETURN)) == BW_MIX_MAX, "failed: the flags unchanged");
    measure(0, 40, &l, &r);
    check(l > 100.0, "the mix is back after it");
    save("detect_more");

    if (bw_mix_param_name(-1)[0] || bw_mix_get(BW_MIX_PARAMS) || bw_mix_steps(999)) check(0, "out-of-range index");
    bw_mix_set(-1, 5);
    bw_mix_format(BW_MIX_PARAMS, text, sizeof text);
    printf(failures ? "mix test: %d failures\n" : "mix test: ok\n", failures);
    return failures != 0;
}
