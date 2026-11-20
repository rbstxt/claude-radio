/* ADTS AAC framing + decoding via faad2. See decode.h. */
#include "decode.h"

#include <neaacdec.h>
#include <stdatomic.h>
#include <string.h>

#define INBUF 16384 /* > 2 x max ADTS frame (8191 bytes) */

static NeAACDecHandle h;
static int inited;
static unsigned char in[INBUF];
static size_t off, len; /* valid bytes are in[off .. off+len) */
static size_t skip;     /* bytes of an ID3 tag still to discard */
static float stereo[2048 * 2];
static _Atomic unsigned long errors;
static _Atomic unsigned long long bytes_total, frames_total;
static _Atomic int last_rate;
static _Atomic size_t buffered;

unsigned long dec_errors(void) { return atomic_load(&errors); }
size_t dec_buffered(void) { return atomic_load(&buffered); }

double dec_bytes_per_sec(void) {
    unsigned long long f = atomic_load(&frames_total);
    int r = atomic_load(&last_rate);
    if (!f || !r) return 0;
    return (double)atomic_load(&bytes_total) * r / (double)f;
}

int dec_open(void) {
    h = NeAACDecOpen();
    if (!h) return -1;
    NeAACDecConfigurationPtr c = NeAACDecGetCurrentConfiguration(h);
    c->outputFormat = FAAD_FMT_FLOAT; /* keep full precision, like a browser's decoder */
    c->downMatrix = 1;                /* anything beyond stereo is folded down */
    NeAACDecSetConfiguration(h, c);
    return 0;
}

void dec_reset(void) {
    off = len = skip = 0;
    atomic_store(&buffered, 0);
    if (inited) NeAACDecPostSeekReset(h, 0);
}

static void consume(size_t n) {
    off += n;
    len -= n;
    if (len == 0) off = 0;
}

/* Returns frames decoded, 0 if the frame was unusable. */
static int decode_frame(const unsigned char *f, size_t flen) {
    if (!inited) {
        unsigned long rate;
        unsigned char ch;
        if (NeAACDecInit(h, (unsigned char *)f, (unsigned long)flen, &rate, &ch) < 0) {
            atomic_fetch_add(&errors, 1);
            return 0;
        }
        inited = 1;
    }
    NeAACDecFrameInfo fi;
    float *pcm = NeAACDecDecode(h, &fi, (unsigned char *)f, (unsigned long)flen);
    if (fi.error || !pcm || fi.channels == 0) {
        if (fi.error) atomic_fetch_add(&errors, 1);
        return 0;
    }
    int frames = (int)(fi.samples / fi.channels);
    if (frames > 2048) frames = 2048;
    if (fi.channels == 2) {
        memcpy(stereo, pcm, (size_t)frames * 2 * sizeof(float));
    } else {
        for (int i = 0; i < frames; i++) {
            float l = pcm[i * fi.channels];
            float r = fi.channels > 1 ? pcm[i * fi.channels + 1] : l;
            stereo[2 * i] = l;
            stereo[2 * i + 1] = r;
        }
    }
    atomic_fetch_add(&bytes_total, flen);
    atomic_fetch_add(&frames_total, (unsigned long long)frames);
    atomic_store(&last_rate, (int)fi.samplerate);
    return frames;
}

int dec_next(dec_pull pull, const float **pcm, int *rate) {
    for (;;) {
        if (skip && len) {
            size_t s = len < skip ? len : skip;
            consume(s);
            skip -= s;
        }
        if (!skip && len >= 10) {
            const unsigned char *b = in + off;
            if (b[0] == 'I' && b[1] == 'D' && b[2] == '3') {
                size_t tag = ((size_t)(b[6] & 0x7f) << 21) | ((size_t)(b[7] & 0x7f) << 14) |
                             ((size_t)(b[8] & 0x7f) << 7) | (size_t)(b[9] & 0x7f);
                skip = 10 + tag + ((b[5] & 0x10) ? 10 : 0);
                continue;
            }
            if (b[0] == 0xFF && (b[1] & 0xF6) == 0xF0) { /* ADTS sync, layer 0 */
                size_t flen = ((size_t)(b[3] & 0x03) << 11) | ((size_t)b[4] << 3) | (b[5] >> 5);
                if (flen < 7) {
                    consume(1);
                    continue;
                }
                if (len >= flen) {
                    int frames = decode_frame(b, flen);
                    consume(flen);
                    atomic_store(&buffered, len);
                    if (frames > 0) {
                        *pcm = stereo;
                        *rate = atomic_load(&last_rate);
                        return frames;
                    }
                    continue;
                }
            } else {
                consume(1); /* resync */
                continue;
            }
        }
        /* need more input */
        if (off > 0 && off + len > INBUF / 2) {
            memmove(in, in + off, len);
            off = 0;
        }
        size_t room = INBUF - (off + len);
        if (room == 0) { /* garbage that never syncs: start over */
            off = len = 0;
            room = INBUF;
        }
        size_t n = pull(in + off + len, room);
        if (n == 0) {
            atomic_store(&buffered, len);
            return 0;
        }
        len += n;
    }
}
