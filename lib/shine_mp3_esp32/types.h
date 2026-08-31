#ifndef PRIV_TYPES_H
#define PRIV_TYPES_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <math.h>

#define GRANULE_SIZE  576

#include "bitstream.h"

/* Include arch-specific instructions,
 * when defined. */
#if defined(__mips__) && (__mips == 32)
#include "mult_mips_gcc.h"
#elif defined(__arm__) && !defined(__thumb__)
#include "mult_sarm_gcc.h"
#endif

/* Include and define generic instructions,
 * when not already defined above. */
#include "mult_noarch_gcc.h"

#ifndef SWAB32
#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))
#define SWAB32(x)	__builtin_bswap32(x)
#else
#define SWAB32(x)	(((unsigned int)(x) >> 24) | (((unsigned int)(x) >> 8) & 0xff00) | (((unsigned int)(x) & 0xff00) << 8) | ((unsigned int)(x) << 24))
#endif
#endif

/* #define DEBUG if you want the library to dump info to stdout */

#define PI          3.14159265358979
#define PI4         0.78539816339745
#define PI12        0.26179938779915
#define PI36        0.087266462599717
#define PI64        0.049087385212
#define SQRT2       1.41421356237
#define LN2         0.69314718
#define LN_TO_LOG10 0.2302585093
#define BLKSIZE     1024
#define HAN_SIZE    512 /* for loop unrolling, require that HAN_SIZE%8==0 */
#define SCALE_BLOCK 12
#define SCALE_RANGE 64
#define SCALE       32768
#define SBLIMIT     32

#ifndef MAX_CHANNELS
// Upstream default is 2 (stereo). speaker.cpp's mic_start_recording()
// always configures PCM_MONO (config->wave.channels = 1) - a board mic
// has exactly one channel to begin with - so the second channel's worth
// of buffers this constant sizes every per-channel array/allocation for
// (shine_global_config's l3_sb_sample/subband.x/scalefactor/etc, plus the
// separate heap_caps_malloc() calls for l3_enc/mdct_freq in
// shine_initialise()) is dead weight: never written by
// shine_iteration_loop()/shine_subband_initialise()/etc, which all loop
// `for (ch = 0; ch < config->wave.channels; ch++)`, and every other
// place that touches channel index 1 unconditionally (layer3.c's
// buffer[1], reservoir.c/l3bitstream.c's mean_bits-splitting logic) is
// itself gated behind `config->wave.channels == 2` first. Halving this
// saves ~18KB total (struct fields + the separate per-channel
// allocations) - the difference between the encoder fitting in this
// no-PSRAM board's free heap and not (see speaker.cpp's
// mic_start_recording() comment). See MAX_GRANULES below for the same
// treatment of the other axis these arrays are sized by.
#define MAX_CHANNELS 1
#endif

#ifndef MAX_GRANULES
// Upstream default is 2 (MPEG-I's 2 granules/frame). speaker.cpp's
// mic_start_recording() always configures 16kHz, which
// shine_mpeg_version() (layer3.c) maps to MPEG-II - granules_per_frame
// (layer3.c's `granules_per_frame[]` table) is 1 for every non-MPEG-I
// version, and calc_scfsi() (l3loop.c, the one function that reads
// granule index 1 unconditionally, comparing granule 0 against granule
// 1's energy for MPEG-I's cross-granule scalefactor sharing) only ever
// runs `if (config->mpeg.version == MPEG_I)` - dead code on this board.
// Every other granule-indexed loop bounds itself by the runtime
// `config->mpeg.granules_per_frame`, not this constant, so lowering it
// doesn't change which array indices ever get touched - *except*
// l3_sb_sample's declared depth is MAX_GRANULES+1, not MAX_GRANULES:
// l3mdct.c's shine_mdct_sub() writes each granule's subband samples to
// slot `gr+1` (reserving slot 0 as "previous frame's last granule", fed
// back via that function's own closing memcpy from slot
// `granules_per_frame` to slot 0) - with granules_per_frame always 1
// here, only slots 0 and 1 are ever read or written, so MAX_GRANULES+1
// only ever needs to be 2, i.e. this constant only ever needs to be 1.
// Halving this (on top of MAX_CHANNELS above) saves another ~13KB,
// mostly from l3_sb_sample and subband.fl/mdct.cos_l's neighbors in
// shine_global_config, not the encoder's other, larger allocations.
#define MAX_GRANULES 1
#endif

typedef struct {
    int16_t channels;
    int samplerate;
} priv_shine_wave_t;

typedef struct {
    int    version;
    int    layer;
    int    granules_per_frame;
    int    mode;      /* + */ /* Stereo mode */
    int    bitr;      /* + */ /* Must conform to known bitrate - see Main.c */
    int    emph;      /* + */ /* De-emphasis */
    int    padding;
    int    bits_per_frame;
    int    bits_per_slot;
    double frac_slots_per_frame;
    double slot_lag;
    int    whole_slots_per_frame;
    int    bitrate_index;     /* + */ /* See Main.c and Layer3.c */
    int    samplerate_index;  /* + */ /* See Main.c and Layer3.c */
    int    crc;
    int    ext;
    int    mode_ext;
    int    copyright;  /* + */
    int    original;   /* + */
} priv_shine_mpeg_t;

typedef struct {
  int32_t *xr;                    /* magnitudes of the spectral values */
  int32_t *xrsq;     /* xr squared */
  int32_t *xrabs;    /* xr absolute */
  int32_t xrmax;                  /* maximum of xrabs array */
  int32_t en_tot[MAX_GRANULES];   /* gr */
  int32_t en[MAX_GRANULES][21];
  int32_t xm[MAX_GRANULES][21];
  int32_t xrmaxl[MAX_GRANULES];
  double steptab[128]; /* 2**(-x/4)  for x = -127..0 */
  int32_t steptabi[128];  /* 2**(-x/4)  for x = -127..0 */
  int16_t int2idx[10000]; /* x**(3/4)   for x = 0..9999 */
} l3loop_t;

typedef struct {
  int32_t cos_l[18][36];
} mdct_t;

typedef struct {
  int off[MAX_CHANNELS];
  int32_t fl[SBLIMIT][64];
  int32_t x[MAX_CHANNELS][HAN_SIZE];
} subband_t; 

/* Side information */
typedef struct {
  unsigned part2_3_length;
  unsigned big_values;
  unsigned count1;
  unsigned global_gain;
  unsigned scalefac_compress;
  unsigned table_select[3];
  unsigned region0_count;
  unsigned region1_count;
  unsigned preflag;
  unsigned scalefac_scale;
  unsigned count1table_select;
  unsigned part2_length;
  unsigned sfb_lmax;
  unsigned address1;
  unsigned address2;
  unsigned address3;
  int quantizerStepSize;
  unsigned slen[4];
} gr_info;

typedef struct {
    unsigned private_bits;
    int resvDrain;
    unsigned scfsi[MAX_CHANNELS][4];
    struct {
        struct {
            gr_info tt;
        } ch[MAX_CHANNELS];
    } gr[MAX_GRANULES];
} shine_side_info_t;

typedef struct {
    float  l[MAX_GRANULES][MAX_CHANNELS][21];
} shine_psy_ratio_t;

typedef struct {
    double  l[MAX_GRANULES][MAX_CHANNELS][21];
} shine_psy_xmin_t;

typedef struct {
    int32_t l[MAX_GRANULES][MAX_CHANNELS][22];            /* [cb] */
    int32_t s[MAX_GRANULES][MAX_CHANNELS][13][3];         /* [window][cb] */
} shine_scalefac_t;


typedef struct shine_global_flags { 
  priv_shine_wave_t    wave;
  priv_shine_mpeg_t    mpeg;
  bitstream_t    bs;
  shine_side_info_t side_info;
  int            sideinfo_len;
  int            mean_bits;
  //shine_psy_ratio_t ratio;
  shine_scalefac_t  scalefactor;
  int16_t       *buffer[MAX_CHANNELS];
  double          pe[MAX_CHANNELS][MAX_GRANULES];
  int            *l3_enc[MAX_CHANNELS][MAX_GRANULES]; //4% reduction in performance IRAM
  int32_t        l3_sb_sample[MAX_CHANNELS][MAX_GRANULES+1][18][SBLIMIT];
  int32_t        *mdct_freq[MAX_CHANNELS][MAX_GRANULES]; //1% reduction in perormance IRAM
  int            ResvSize;
  int            ResvMax;
  l3loop_t       *l3loop;
  mdct_t         mdct;
  subband_t      subband;
} shine_global_config;

#endif
