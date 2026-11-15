/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl
 * Ported for Beiwagen from the Steckfeld engine (Engine/src/<File>), float only, static buffers */
/* The effect chain (bw_fx.cc, fx_*.cc, built with the FPU): ints and pointers only across, so app.c may include it.
 *
 * A fixed serial chain, in this order: Distortion, Filter, Chorus, Delay, Reverb (bw_fx_name(0..bw_fx_count()-1)).
 * Each effect has exactly 4 parameters (one UI page, KNOB1-4), each an integer 0..1000 that the effect maps
 * musically (exponential for times and frequencies); a choice parameter (a mode) has bw_fx_steps() positions spread
 * evenly over 0..1000. Every effect starts disabled; a disabled effect costs no CPU, enabling one starts it from
 * silence (its delay lines cleared). Enable, disable and bypass fade over 10 ms. */
#ifndef BW_FX_H
#define BW_FX_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void bw_fx_init(void);                            /* once, before the audio starts */
void bw_fx_process(int32_t *lr, uint32_t frames); /* INTERRUPT: in place, L R L R ..., +-32767 in and out */

int bw_fx_count(void);
const char *bw_fx_name(int fx);
void bw_fx_enable(int fx, int on);
int bw_fx_enabled(int fx);

#define BW_FX_PARAMS 4
#define BW_FX_MAX 1000
const char *bw_fx_param_name(int fx, int p);
void bw_fx_set(int fx, int p, int value);             /* clamped to 0..BW_FX_MAX; the interrupt takes it next block */
int bw_fx_get(int fx, int p);
int bw_fx_steps(int fx, int p);                       /* positions of a choice parameter, 0 if continuous */
void bw_fx_format(int fx, int p, char *out, int max); /* the value as shown: "350 ms", "1.2 kHz", "40 %", "LP 24" */

void bw_fx_bypass(int on); /* the whole chain; the effects stop while bypassed and start from silence after */
int bw_fx_bypassed(void);

#ifdef __cplusplus
}
#endif

#endif
