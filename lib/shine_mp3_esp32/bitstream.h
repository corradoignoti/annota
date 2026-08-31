#ifndef BITSTREAM_H
#define BITSTREAM_H

typedef struct  bit_stream_struc {
    unsigned char *data;        /* Processed data */
    int         data_size;      /* Total data size */
    int         data_position;  /* Data position */
    unsigned int cache;			/* bit stream cache */
    int         cache_bits;     /* free bits in cache */
} bitstream_t;

/* "bit_stream.h" Definitions */

#define         MINIMUM         4    /* Minimum size of the buffer in bytes */
#define         MAX_LENGTH      32   /* Maximum length of word written or
                                        read from bit stream */

// Upstream default is 4096 - sized for a worst-case MPEG-I stereo frame
// at up to 320kbps (up to ~1044 bytes/frame), with headroom to spare.
// speaker.cpp's mic_start_recording() always encodes MPEG-II mono at
// 32kbps/16kHz (~144 bytes/frame), and layer3.c's shine_encode_buffer_
// internal()/shine_flush() both reset bs.data_position to 0 after every
// single frame (see their own bodies) - this buffer never has to hold
// more than one frame at a time, regardless of how long the recording
// runs. 512 bytes leaves >3x headroom over our actual frame size, and
// shine_putbits()'s own realloc-if-too-small logic (bitstream.c) means
// even a wrong guess here just costs a one-time realloc, never silent
// data loss - so this is a size tuned for our specific use, not a
// hard requirement. Saves ~3.5KB versus upstream's 4096, same reason as
// types.h's MAX_CHANNELS comment.
#define         BUFFER_SIZE     512

//#define         MIN(A, B)       ((A) < (B) ? (A) : (B))
//#define         MAX(A, B)       ((A) > (B) ? (A) : (B))

void shine_open_bit_stream(bitstream_t *bs,const int size);
void shine_close_bit_stream(bitstream_t *bs);
void shine_putbits(bitstream_t *bs,unsigned int val, unsigned int N);
int  shine_get_bits_count(bitstream_t *bs);

#endif
