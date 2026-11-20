/* Lock-free SPSC byte ring. See sbuf.h. */
#include "sbuf.h"

#include <stdatomic.h>
#include <string.h>

_Static_assert((SBUF_SIZE & (SBUF_SIZE - 1)) == 0, "SBUF_SIZE must be a power of two");

static unsigned char data[SBUF_SIZE];
static _Atomic uint64_t wpos, rpos; /* monotonically increasing byte counters */

size_t sbuf_fill(void) {
    return (size_t)(atomic_load_explicit(&wpos, memory_order_acquire) -
                    atomic_load_explicit(&rpos, memory_order_acquire));
}

size_t sbuf_space(void) { return SBUF_SIZE - sbuf_fill(); }

uint64_t sbuf_wpos(void) { return atomic_load_explicit(&wpos, memory_order_relaxed); }

size_t sbuf_write(const void *p, size_t n) {
    uint64_t w = atomic_load_explicit(&wpos, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&rpos, memory_order_acquire);
    size_t space = SBUF_SIZE - (size_t)(w - r);
    if (n > space) n = space;
    size_t at = (size_t)(w & (SBUF_SIZE - 1));
    size_t first = SBUF_SIZE - at < n ? SBUF_SIZE - at : n;
    memcpy(data + at, p, first);
    memcpy(data, (const unsigned char *)p + first, n - first);
    atomic_store_explicit(&wpos, w + n, memory_order_release);
    return n;
}

size_t sbuf_read(void *p, size_t n) {
    uint64_t r = atomic_load_explicit(&rpos, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&wpos, memory_order_acquire);
    size_t avail = (size_t)(w - r);
    if (n > avail) n = avail;
    size_t at = (size_t)(r & (SBUF_SIZE - 1));
    size_t first = SBUF_SIZE - at < n ? SBUF_SIZE - at : n;
    memcpy(p, data + at, first);
    memcpy((unsigned char *)p + first, data, n - first);
    atomic_store_explicit(&rpos, r + n, memory_order_release);
    return n;
}

void sbuf_skip_to(uint64_t pos) {
    uint64_t r = atomic_load_explicit(&rpos, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&wpos, memory_order_acquire);
    if (pos > w) pos = w;
    if (pos > r) atomic_store_explicit(&rpos, pos, memory_order_release);
}
