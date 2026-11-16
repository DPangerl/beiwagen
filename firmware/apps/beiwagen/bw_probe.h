/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl */
/* The probe that the loop canceller (bw_aec.cc) and the mixer's return finder (bw_mix.cc) send out to find the
 * OP-1's loop: PROBE samples of binary noise from a fixed seed, and its second difference, which a recording is
 * correlated with (that whitens music playing meanwhile; the probe stays sharp). bw_probe.cc, FPU C++ only. */
#ifndef BW_PROBE_H
#define BW_PROBE_H
#include <stdint.h>

namespace bwprobe {

const int PROBE = 2048;          /* samples (46 ms) */
extern uint32_t bits[PROBE / 32];
extern int8_t d2[PROBE + 2];     /* the second difference, from index -1 */

void init();                     /* fills both; again is harmless */
inline int bit(int i) { return (int)(bits[i >> 5] >> (i & 31)) & 1; }
inline int sign(int i) { return bit(i) ? 1 : -1; }

} // namespace bwprobe

#endif
