/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl */
/* The loop probe (bw_probe.h). */
#include "bw_probe.h"

namespace bwprobe {

uint32_t bits[PROBE / 32];
int8_t d2[PROBE + 2];

void init() {
    uint32_t s = 0x2545F491u;
    for (int w = 0; w < PROBE / 32; w++) {
        uint32_t b = 0;
        for (int k = 0; k < 32; k++) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            b |= (s >> 31) << k;
        }
        bits[w] = b;
    }
    for (int i = -1; i <= PROBE; i++) {
        const int b = i >= 0 && i < PROBE ? sign(i) : 0;
        const int p = i - 1 >= 0 && i - 1 < PROBE ? sign(i - 1) : 0;
        const int q = i + 1 >= 0 && i + 1 < PROBE ? sign(i + 1) : 0;
        d2[i + 1] = (int8_t)(2 * b - p - q);
    }
}

} // namespace bwprobe
