/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl
 * The loop canceller: takes the FM-1's own output, as the OP-1 returns it, out of the FM-1's input */
/* bw_aec.cc, built with the FPU: ints and pointers only across, so app.c may include it.
 *
 * The OP-1 puts the FM-1's output x on the track the FM-1 listens to, so the input is y = s + h*x (s: the live
 * playing, h: the loop, a delay and a gain). Enabling measures h with a noise burst on the output (calibrating,
 * about 1 s, up to four 46 ms bursts at -30 dBFS while playing drowns them; the input is muted meanwhile), then
 * cancels it: bw_aec_input returns e = y - h*x, about s. h stays as measured: a changed loop (the OP-1's level, a
 * new delay) needs bw_aec_enable(1) again.
 * Call both every block, in this order: bw_aec_input on the incoming pair before the effects, bw_aec_output on the
 * final output just before it leaves (after the effects and the volume), so x is exactly what goes back. */
#ifndef BW_AEC_H
#define BW_AEC_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { BW_AEC_OFF, BW_AEC_CALIBRATING, BW_AEC_ON, BW_AEC_FAILED };

void bw_aec_init(void);                            /* once, before the audio starts */
void bw_aec_enable(int on);                        /* on: (re)calibrates, then cancels; off: the input untouched */
int bw_aec_state(void);                            /* BW_AEC_* */
int bw_aec_delay(void);                            /* the loop's delay in samples, -1 unknown */
int bw_aec_erle_db(void);                          /* echo removed in dB as calibrated (h over its noise), 0 unknown */
void bw_aec_input(int32_t *lr, uint32_t frames);   /* INTERRUPT: y in, e out, in place, L R ..., +-32767 */
void bw_aec_output(int32_t *lr, uint32_t frames);  /* INTERRUPT: the final output x, in place (the probe added) */

#ifdef __cplusplus
}
#endif

#endif
