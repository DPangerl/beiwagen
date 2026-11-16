/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Daniel Pangerl */
/* The mixdown engine (bw_mix.cc, built with the FPU): ints and pointers only across, so app.c may include it.
 *
 * The OP-1's four tape tracks (USB mode 8: four stereo pairs) in, one stereo mix out, which goes back to the OP-1 and
 * lands on its selected track. Per track: an insert (the effect chain's filter with its drive, or off), a level, an
 * equal-power pan (a balance: unity in the middle, +3 dB on the side kept at the ends), sends after the level to the
 * shared delay and reverb, mute, and "return": the track the mix lands on, kept out of the mix (else it loops).
 * Every change glides (levels over a block, the insert in and out over 10 ms), so nothing clicks.
 * The delay and the reverb are the effect chain's (bw_fx.h) one instance each, their lines in the pool: run
 * bw_mix_process or bw_fx_process in a block, not both (the one that runs takes them over from silence).
 *
 * Parameters: an integer 0..BW_MIX_MAX each, as bw_fx's: a name, the value as shown, the positions of a choice (0:
 * continuous). Track t's k-th is BW_MIX_P(t, k); the shared ones follow. The whole state packs into a blob of at most
 * BW_MIX_BLOB bytes for a preset (bw_mix_save / bw_mix_load; loading leaves the return flags as they are: they belong
 * to the OP-1's selected track, not to a preset).
 *
 * Return detection: bw_mix_detect() fades the mix out, sends a noise probe (46 ms, -30..-18 dBFS) alone, records the
 * four incoming pairs, fades the mix back in (without the delay and the reverb, whose line it then searches in) and
 * correlates each pair with the probe, about 1.5 s in all. The one pair that holds the probe gets the return flag (the
 * others lose theirs); none, or more than one: FAILED, the flags unchanged. Call bw_mix_output on the final output
 * (after the volume) so the probe goes out whatever the volume. */
#ifndef BW_MIX_H
#define BW_MIX_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BW_MIX_TRACKS 4
#define BW_MIX_MAX 1000

enum {
    BW_MIX_LEVEL,        /* Off, -54 .. +6 dB (900: 0 dB), 1.2 dB a 20 */
    BW_MIX_PAN,          /* 500: centre */
    BW_MIX_SEND_DELAY,   /* after the level */
    BW_MIX_SEND_REVERB,
    BW_MIX_CUTOFF,       /* the insert: the filter's four */
    BW_MIX_RESO,
    BW_MIX_DRIVE,
    BW_MIX_MODE,         /* Off, LP 12 .. Notch 24 (9 positions) */
    BW_MIX_MUTE,         /* 2 positions */
    BW_MIX_RETURN,       /* 2 positions: kept out of the mix */
    BW_MIX_TRACK_PARAMS
};
#define BW_MIX_P(t, k) ((t) * BW_MIX_TRACK_PARAMS + (k))
enum {
    BW_MIX_DELAY_TIME = BW_MIX_TRACKS * BW_MIX_TRACK_PARAMS,
    BW_MIX_DELAY_FEEDBACK,
    BW_MIX_DELAY_TONE,
    BW_MIX_DELAY_RETURN, /* the delay's level in the mix */
    BW_MIX_REVERB_SIZE,
    BW_MIX_REVERB_DECAY,
    BW_MIX_REVERB_DAMPING,
    BW_MIX_REVERB_RETURN,
    BW_MIX_MASTER,       /* as a track's level */
    BW_MIX_INSERTS,      /* 2 positions: the inserts at twice the rate (as the chain), or at the rate (cheaper) */
    BW_MIX_PARAMS
};
#define BW_MIX_BLOB (4 + 2 * BW_MIX_PARAMS)

enum { BW_MIX_DETECT_IDLE, BW_MIX_DETECT_RUNNING, BW_MIX_DETECT_FOUND, BW_MIX_DETECT_FAILED };

void bw_mix_init(void); /* once, before the audio starts */
/* INTERRUPT: npairs pairs (16-bit L R interleaved, pair k at pairs + 2 * frames * k, as usb_play_read_pairs gives
   them; 0: silence) mixed into lr, L R ..., +-32767 */
void bw_mix_process(const int16_t *pairs, int npairs, int32_t *lr, uint32_t frames);
void bw_mix_output(int32_t *lr, uint32_t frames); /* INTERRUPT: the final output, in place (the probe added) */

const char *bw_mix_param_name(int p);
void bw_mix_set(int p, int value); /* clamped to 0..BW_MIX_MAX; the interrupt takes it next block */
int bw_mix_get(int p);
int bw_mix_default(int p);
int bw_mix_steps(int p);                       /* positions of a choice parameter, 0 if continuous */
void bw_mix_format(int p, char *out, int max); /* "-6.0 dB", "40 L", "1.2 kHz", "LP 24", "On" */
int bw_mix_peak(int t);                        /* track t's input, the last block's peak, 0..32767 */

int bw_mix_save(void *dst, int max);       /* the state into dst: the bytes written, 0 if max is too small */
int bw_mix_load(const void *src, int len); /* 1: taken (all but the return flags), 0: not a mixer blob */

void bw_mix_detect(void);       /* the main loop: find the return track (again) */
int bw_mix_detect_state(void);  /* BW_MIX_DETECT_* */
int bw_mix_detect_track(void);  /* the track found, -1 none */
int bw_mix_detect_delay(void);  /* the loop's delay in samples (within a few), -1 unknown */

#ifdef __cplusplus
}
#endif

#endif
