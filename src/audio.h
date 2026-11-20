/* Audio output straight to the CoreAudio HAL (no AudioUnit / AudioQueue layer).
 *
 * The IOProc - CoreAudio's real-time thread - does all the audio work itself:
 * it pulls compressed AAC from the lock-free stream buffer (sbuf), decodes it
 * with faad2, resamples if the device is not at the stream rate, and maps L/R
 * onto the device's preferred stereo pair. There is no PCM ring and no decode
 * thread, so audio keeps flowing as long as compressed data is buffered, even
 * if the rest of the process is starved of CPU time.
 */
#ifndef CR_AUDIO_H
#define CR_AUDIO_H

#include <stdint.h>

int audio_open(void);   /* default output device; 0 on success */
void audio_close(void);
int audio_start(void);
void audio_stop(void);

double audio_rate(void);             /* device nominal sample rate */
const char *audio_device_name(void);
const char *audio_last_error(void);
int audio_take_device_change(void);  /* 1 once after the default device or its rate changed */

void audio_set_gain(float g);        /* linear, 1.0 = unity */
int audio_playing(void);             /* 1 while decoded audio is being output */
unsigned long audio_underruns(void);

/* Flush protocol (stale audio after pause, resync to the live edge):
 *   main thread:  n = audio_flush_request();
 *   net thread:   sees audio_flush_pending(&seen) -> stops writing old data ->
 *                 audio_flush_ack(n, sbuf_wpos())
 *   IOProc:       outputs silence until the ack, then skips to that position. */
unsigned audio_flush_request(void);
int audio_flush_pending(unsigned *seen); /* producer side; updates *seen */
void audio_flush_ack(unsigned gen, uint64_t drop_pos);

#endif
