/* ADTS AAC framing + decoding via faad2. See decode.h. */
#include "decode.h"

#include <neaacdec.h>
#include <stdio.h>
#include <string.h>

#define INBUF 16384 /* > max ADTS frame (8191 bytes) */

static NeAACDecHandle h;
static int inited;
static unsigned char in[INBUF];
static size_t off, len;   /* valid bytes are in[off .. off+len) */
static size_t skip;       /* bytes of an ID3 tag still to discard */
static unsigned long errors;
static unsigned long long bytes_total, frames_total;
static int last_rate;
static float stereo[2048 * 2];

unsigned long dec_errors(void) { return errors; }

double dec_bytes_per_sec(void) {
    if (!frames_total || !last_rate) return 0;
    return (double)bytes_total * last_rate / (double)frames_total;
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
    if (inited) NeAACDecPostSeekReset(h, 0);
}

static void consume(size_t n) {
    off += n;
    len -= n;
    if (len == 0) off = 0;
}

static int decode_frame(const unsigned char *f, size_t flen, pcm_sink sink, void *ctx) {
    if (!inited) {
        unsigned long rate;
        unsigned char ch;
        if (NeAACDecInit(h, (unsigned char *)f, (unsigned long)flen, &rate, &ch) < 0) {
            errors++;
            return 0;
        }
        inited = 1;
    }
    NeAACDecFrameInfo fi;
    float *pcm = NeAACDecDecode(h, &fi, (unsigned char *)f, (unsigned long)flen);
    if (fi.error || !pcm || fi.channels == 0) {
        if (fi.error) errors++;
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
    bytes_total += flen;
    frames_total += (unsigned long long)frames;
    last_rate = (int)fi.samplerate;
    return sink(stereo, frames, (int)fi.samplerate, ctx);
}

int dec_feed(const void *data, size_t n, pcm_sink sink, void *ctx) {
    const unsigned char *p = data;
    while (n > 0) {
        if (skip) {
            size_t k = n < skip ? n : skip;
            p += k, n -= k, skip -= k;
            continue;
        }
        if (off + len == INBUF) { /* compact */
            memmove(in, in + off, len);
            off = 0;
        }
        size_t room = INBUF - (off + len);
        size_t k = n < room ? n : room;
        memcpy(in + off + len, p, k);
        len += k, p += k, n -= k;

        for (;;) {
            if (skip) {
                size_t s = len < skip ? len : skip;
                consume(s);
                skip -= s;
                if (skip) break;
            }
            if (len < 10) break;
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
                if (len < flen) break;
                int stop = decode_frame(b, flen, sink, ctx);
                consume(flen);
                if (stop) return stop;
                continue;
            }
            consume(1); /* resync */
        }
    }
    return 0;
}
