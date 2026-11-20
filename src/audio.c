/* Audio output straight to the CoreAudio HAL; decoding happens in the IOProc. See audio.h. */
#include "audio.h"

#include <CoreAudio/CoreAudio.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "decode.h"
#include "resample.h"
#include "sbuf.h"

#define PRIME_BYTES (16 * 1024) /* compressed audio to buffer before (re)starting, ~1 s */
#define MIX_MAX     8192        /* largest IO buffer we render in one go (frames) */
#define RS_CHUNK    256
#define RS_OUT_MAX  (RS_CHUNK * 9 + 16) /* enough for 22.05 kHz -> 192 kHz */

/* shared with other threads */
static _Atomic float gain = 1.0f;
static _Atomic unsigned long underruns;
static _Atomic int dev_changed, playing;
static _Atomic unsigned flush_req, flush_ack;
static _Atomic uint64_t flush_pos;

/* IOProc-only state (touched elsewhere only while the IOProc is stopped) */
static unsigned flush_applied;
static int primed, was_playing;
static const float *pend;        /* rendered frames waiting to be output */
static int pend_n, pend_off;
static const float *dec_ptr;     /* decoded frames waiting for the resampler */
static int dec_left, stream_rate;
static resampler rs;
static float rs_out[RS_OUT_MAX * 2];
static int rs_in_rate, rs_out_rate;
static float mix[MIX_MAX * 2];

/* device */
static AudioObjectID dev = kAudioObjectUnknown;
static AudioDeviceIOProcID procid;
static Float64 rate;
static UInt32 ch_left = 1, ch_right = 2, total_ch = 2;
static int running, listening_default;
static char dev_name[128];
static char errbuf[256];

const char *audio_last_error(void) { return errbuf; }
double audio_rate(void) { return rate; }
const char *audio_device_name(void) { return dev_name; }
void audio_set_gain(float g) { atomic_store(&gain, g); }
unsigned long audio_underruns(void) { return atomic_load(&underruns); }
int audio_playing(void) { return atomic_load(&playing); }
int audio_take_device_change(void) { return atomic_exchange(&dev_changed, 0); }

unsigned audio_flush_request(void) { return atomic_fetch_add(&flush_req, 1) + 1; }

int audio_flush_pending(unsigned *seen) {
    unsigned f = atomic_load_explicit(&flush_req, memory_order_acquire);
    if (f == *seen) return 0;
    *seen = f;
    return 1;
}

void audio_flush_ack(unsigned gen, uint64_t drop_pos) {
    atomic_store_explicit(&flush_pos, drop_pos, memory_order_relaxed);
    atomic_store_explicit(&flush_ack, gen, memory_order_release);
}

/* ---- rendering (IOProc) --------------------------------------------------- */

static size_t pull(void *dst, size_t max) { return sbuf_read(dst, max); }

static void reset_stream(void) {
    dec_reset();
    pend_n = pend_off = dec_left = 0;
    primed = was_playing = 0;
}

/* Fill out[0..n) with stereo frames; returns how many are real audio. */
static int render(float *out, int n) {
    int i = 0, dev_rate = (int)rate;
    while (i < n) {
        if (pend_off < pend_n) {
            int k = pend_n - pend_off < n - i ? pend_n - pend_off : n - i;
            memcpy(out + 2 * i, pend + 2 * pend_off, (size_t)k * 2 * sizeof(float));
            pend_off += k;
            i += k;
            continue;
        }
        if (dec_left > 0) {
            if (stream_rate == dev_rate) { /* bit-exact pass-through */
                pend = dec_ptr;
                pend_n = dec_left;
                dec_left = 0;
            } else {
                if (rs_in_rate != stream_rate || rs_out_rate != dev_rate) {
                    rs_init(&rs, stream_rate, dev_rate);
                    rs_in_rate = stream_rate;
                    rs_out_rate = dev_rate;
                }
                int nin = dec_left < RS_CHUNK ? dec_left : RS_CHUNK;
                pend = rs_out;
                pend_n = rs_process(&rs, dec_ptr, nin, rs_out, RS_OUT_MAX);
                dec_ptr += nin * 2;
                dec_left -= nin;
            }
            pend_off = 0;
            continue;
        }
        if (!primed) {
            if (sbuf_fill() + dec_buffered() < PRIME_BYTES) break;
            primed = 1;
        }
        int r = 0, frames = dec_next(pull, &dec_ptr, &r);
        if (frames <= 0) { /* ran dry: re-buffer instead of crackling */
            if (was_playing) atomic_fetch_add(&underruns, 1);
            primed = was_playing = 0;
            break;
        }
        stream_rate = r;
        dec_left = frames;
    }
    if (i > 0) was_playing = 1;
    return i;
}

static OSStatus ioproc(AudioObjectID d, const AudioTimeStamp *now, const AudioBufferList *in,
                       const AudioTimeStamp *in_time, AudioBufferList *out,
                       const AudioTimeStamp *out_time, void *user) {
    (void)d, (void)now, (void)in, (void)in_time, (void)out_time, (void)user;
    if (out->mNumberBuffers == 0) return noErr;
    UInt32 nch0 = out->mBuffers[0].mNumberChannels ? out->mBuffers[0].mNumberChannels : 1;
    UInt32 nframes = out->mBuffers[0].mDataByteSize / (UInt32)(sizeof(float) * nch0);
    if (nframes > MIX_MAX) nframes = MIX_MAX;

    int got = 0;
    unsigned fr = atomic_load_explicit(&flush_req, memory_order_acquire);
    if (fr != flush_applied && atomic_load_explicit(&flush_ack, memory_order_acquire) == fr) {
        sbuf_skip_to(atomic_load_explicit(&flush_pos, memory_order_relaxed));
        reset_stream();
        flush_applied = fr;
    }
    if (fr == flush_applied) got = render(mix, (int)nframes); /* else: silence until the ack */
    memset(mix + 2 * got, 0, (size_t)(nframes - (UInt32)got) * 2 * sizeof(float));
    atomic_store_explicit(&playing, got > 0, memory_order_relaxed);

    float g = atomic_load_explicit(&gain, memory_order_relaxed);
    UInt32 base = 0;
    for (UInt32 b = 0; b < out->mNumberBuffers; b++) {
        float *s = out->mBuffers[b].mData;
        UInt32 nch = out->mBuffers[b].mNumberChannels;
        if (!s || nch == 0) continue;
        UInt32 n = out->mBuffers[b].mDataByteSize / (UInt32)(sizeof(float) * nch);
        for (UInt32 i = 0; i < n; i++) {
            float l = 0, r = 0;
            if (i < nframes) {
                l = mix[2 * i] * g;
                r = mix[2 * i + 1] * g;
            }
            for (UInt32 c = 0; c < nch; c++) {
                UInt32 gch = base + c + 1; /* HAL channel numbers are 1-based */
                s[i * nch + c] = total_ch == 1 ? (l + r) * 0.5f : gch == ch_left ? l : gch == ch_right ? r : 0;
            }
        }
        base += nch;
    }
    return noErr;
}

/* ---- device --------------------------------------------------------------- */

static OSStatus on_change(AudioObjectID obj, UInt32 n, const AudioObjectPropertyAddress *a, void *u) {
    (void)obj, (void)n, (void)a, (void)u;
    atomic_store(&dev_changed, 1);
    return noErr;
}

static const AudioObjectPropertyAddress addr_default = {
    kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain};
static const AudioObjectPropertyAddress addr_rate = {
    kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain};

static int fail(const char *what, OSStatus st) {
    snprintf(errbuf, sizeof errbuf, "%s (CoreAudio error %d)", what, (int)st);
    return -1;
}

int audio_open(void) {
    AudioObjectPropertyAddress a = addr_default;
    UInt32 sz = sizeof dev;
    OSStatus st = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, &dev);
    if (st || dev == kAudioObjectUnknown) return fail("no default output device", st);

    a = addr_rate;
    sz = sizeof rate;
    if ((st = AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &rate)) || rate <= 0)
        return fail("could not read the output sample rate", st);

    /* the IOProc sees the streams' virtual format; we only speak float32 */
    a = (AudioObjectPropertyAddress){kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput,
                                     kAudioObjectPropertyElementMain};
    AudioStreamID streams[16];
    sz = sizeof streams;
    if ((st = AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, streams)) || sz == 0)
        return fail("the default device has no output streams", st);
    AudioStreamBasicDescription fmt;
    a = (AudioObjectPropertyAddress){kAudioStreamPropertyVirtualFormat,
                                     kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    sz = sizeof fmt;
    if ((st = AudioObjectGetPropertyData(streams[0], &a, 0, NULL, &sz, &fmt)))
        return fail("could not read the output format", st);
    if (fmt.mFormatID != kAudioFormatLinearPCM || !(fmt.mFormatFlags & kAudioFormatFlagIsFloat) ||
        fmt.mBitsPerChannel != 32)
        return fail("the output device does not use float32 samples", 0);

    /* total channel count and the device's preferred stereo pair */
    a = (AudioObjectPropertyAddress){kAudioDevicePropertyStreamConfiguration,
                                     kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
    union { AudioBufferList l; char raw[sizeof(AudioBufferList) + 16 * sizeof(AudioBuffer)]; } cfg;
    sz = sizeof cfg;
    total_ch = 0;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &cfg) == noErr)
        for (UInt32 i = 0; i < cfg.l.mNumberBuffers && i < 17; i++) total_ch += cfg.l.mBuffers[i].mNumberChannels;
    if (total_ch == 0) total_ch = 2;
    UInt32 pair[2] = {1, 2};
    a = (AudioObjectPropertyAddress){kAudioDevicePropertyPreferredChannelsForStereo,
                                     kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
    sz = sizeof pair;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, pair) != noErr) pair[0] = 1, pair[1] = 2;
    ch_left = pair[0];
    ch_right = pair[1];

    CFStringRef name = NULL;
    a = (AudioObjectPropertyAddress){kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
    sz = sizeof name;
    dev_name[0] = 0;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &name) == noErr && name) {
        CFStringGetCString(name, dev_name, sizeof dev_name, kCFStringEncodingUTF8);
        CFRelease(name);
    }

    /* prepare the resampler here rather than in the real-time thread */
    if ((int)rate != 44100 && rs_out_rate != (int)rate) {
        rs_init(&rs, 44100, (int)rate);
        rs_in_rate = 44100;
        rs_out_rate = (int)rate;
    }

    if ((st = AudioDeviceCreateIOProcID(dev, ioproc, NULL, &procid)))
        return fail("could not attach to the output device", st);

    AudioObjectAddPropertyListener(dev, &addr_rate, on_change, NULL);
    if (!listening_default) {
        AudioObjectAddPropertyListener(kAudioObjectSystemObject, &addr_default, on_change, NULL);
        listening_default = 1;
    }
    return 0;
}

int audio_start(void) {
    if (dev == kAudioObjectUnknown || running) return 0;
    OSStatus st = AudioDeviceStart(dev, procid);
    if (st) return fail("could not start audio output", st);
    running = 1;
    return 0;
}

void audio_stop(void) {
    if (dev != kAudioObjectUnknown && running) AudioDeviceStop(dev, procid);
    running = 0;
    atomic_store(&playing, 0);
}

void audio_close(void) {
    if (dev == kAudioObjectUnknown) return;
    audio_stop();
    AudioObjectRemovePropertyListener(dev, &addr_rate, on_change, NULL);
    AudioDeviceDestroyIOProcID(dev, procid);
    dev = kAudioObjectUnknown;
}
