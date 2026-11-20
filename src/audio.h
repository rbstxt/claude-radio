/* Audio output straight to the CoreAudio HAL (no AudioUnit / AudioQueue layer).
 *
 * The decoder thread pushes interleaved stereo float frames at the device's
 * sample rate into a lock-free single-producer/single-consumer ring; the HAL
 * IOProc pulls from it and maps L/R onto the device's preferred stereo pair.
 */
#ifndef CR_AUDIO_H
#define CR_AUDIO_H

#include <stddef.h>

#define PCM_RING_FRAMES 8192 /* ~186 ms at 44.1 kHz */

int audio_open(void);   /* default output device; 0 on success */
void audio_close(void);
int audio_start(void);
void audio_stop(void);

double audio_rate(void);             /* device nominal sample rate */
const char *audio_device_name(void);
const char *audio_last_error(void);
int audio_take_device_change(void);  /* 1 once after the default device or its rate changed */

void audio_set_gain(float g);        /* linear, 1.0 = unity */

/* producer side (decoder thread) */
size_t pcm_space(void);
size_t pcm_fill(void);
void pcm_write(const float *frames, size_t n); /* n <= pcm_space() */

/* ask the IOProc to throw away what is buffered (on resume / resync) */
void pcm_drop(void);

unsigned long audio_underruns(void);

#endif
