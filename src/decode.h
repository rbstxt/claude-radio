/* ADTS AAC framing + decoding via faad2 (float output), pull style.
 * Skips the ID3 tag YouTube puts at the start of every HLS segment.
 * dec_next() is called from the real-time audio callback: no locks, no I/O. */
#ifndef CR_DECODE_H
#define CR_DECODE_H

#include <stddef.h>

/* Supplies up to max compressed bytes; returns 0 when nothing is available. */
typedef size_t (*dec_pull)(void *dst, size_t max);

int dec_open(void);
void dec_reset(void); /* drop buffered input, e.g. after a jump in the stream */

/* Decode the next frame. On success returns the frame count (>0), sets *pcm to
 * interleaved stereo float (valid until the next call) and *rate.
 * Returns 0 when pull() runs dry before a complete frame is available. */
int dec_next(dec_pull pull, const float **pcm, int *rate);

/* bytes buffered inside the decoder that pull() already handed over */
size_t dec_buffered(void);

unsigned long dec_errors(void);
double dec_bytes_per_sec(void); /* compressed bitrate seen so far, 0 if unknown */

#endif
