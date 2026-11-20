/* ADTS AAC framing + decoding via faad2 (float output).
 * Skips the ID3 tag YouTube puts at the start of every HLS segment. */
#ifndef CR_DECODE_H
#define CR_DECODE_H

#include <stddef.h>

/* Receives interleaved stereo float frames at the stream's sample rate.
 * Return non-zero to stop decoding the rest of the current feed. */
typedef int (*pcm_sink)(const float *stereo, int frames, int rate, void *ctx);

int dec_open(void);
void dec_reset(void); /* drop buffered input, e.g. after a jump in the stream */

/* Feed compressed bytes; complete frames are decoded and sent to sink.
 * Returns 0, or non-zero if the sink asked to stop. */
int dec_feed(const void *data, size_t n, pcm_sink sink, void *ctx);

unsigned long dec_errors(void);
double dec_bytes_per_sec(void); /* compressed bitrate seen so far, 0 if unknown */

#endif
