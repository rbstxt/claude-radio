/* Lock-free single-producer/single-consumer byte ring for compressed audio.
 * Producer: the net thread. Consumer: the CoreAudio IOProc (real-time). */
#ifndef CR_SBUF_H
#define CR_SBUF_H

#include <stddef.h>
#include <stdint.h>

#define SBUF_SIZE (128 * 1024) /* ~8 s of 128 kbps AAC */

size_t sbuf_fill(void);
size_t sbuf_space(void);

/* producer */
size_t sbuf_write(const void *p, size_t n); /* writes what fits, returns bytes written */
uint64_t sbuf_wpos(void);

/* consumer */
size_t sbuf_read(void *p, size_t n);
void sbuf_skip_to(uint64_t pos); /* drop everything before stream position pos */

#endif
