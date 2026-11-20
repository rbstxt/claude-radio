/* Audio output straight to the CoreAudio HAL. See audio.h. */
#include "audio.h"

#include <CoreAudio/CoreAudio.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RING_MASK (PCM_RING_FRAMES - 1)
#define PREROLL   (PCM_RING_FRAMES / 2)

_Static_assert((PCM_RING_FRAMES & RING_MASK) == 0, "ring size must be a power of two");

static float ring[PCM_RING_FRAMES * 2];
static _Atomic uint32_t wpos, rpos; /* monotonically increasing frame counters */
static _Atomic int drop_req;
static _Atomic float gain = 1.0f;
static _Atomic unsigned long underruns;
static _Atomic int dev_changed;
static int primed; /* IOProc thread only */

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
void pcm_drop(void) { atomic_store(&drop_req, 1); }

size_t pcm_fill(void) {
    return atomic_load_explicit(&wpos, memory_order_acquire) -
           atomic_load_explicit(&rpos, memory_order_acquire);
}

size_t pcm_space(void) { return PCM_RING_FRAMES - pcm_fill(); }

void pcm_write(const float *frames, size_t n) {
    uint32_t w = atomic_load_explicit(&wpos, memory_order_relaxed);
    for (size_t i = 0; i < n; i++) {
        uint32_t k = (w + (uint32_t)i) & RING_MASK;
        ring[2 * k] = frames[2 * i];
        ring[2 * k + 1] = frames[2 * i + 1];
    }
    atomic_store_explicit(&wpos, w + (uint32_t)n, memory_order_release);
}

int audio_take_device_change(void) { return atomic_exchange(&dev_changed, 0); }

static OSStatus ioproc(AudioObjectID d, const AudioTimeStamp *now, const AudioBufferList *in,
                       const AudioTimeStamp *in_time, AudioBufferList *out,
                       const AudioTimeStamp *out_time, void *user) {
    (void)d, (void)now, (void)in, (void)in_time, (void)out_time, (void)user;
    if (out->mNumberBuffers == 0) return noErr;
    if (atomic_exchange(&drop_req, 0)) {
        atomic_store_explicit(&rpos, atomic_load_explicit(&wpos, memory_order_acquire),
                              memory_order_release);
        primed = 0;
    }
    uint32_t r = atomic_load_explicit(&rpos, memory_order_relaxed);
    uint32_t avail = atomic_load_explicit(&wpos, memory_order_acquire) - r;
    UInt32 nch0 = out->mBuffers[0].mNumberChannels ? out->mBuffers[0].mNumberChannels : 1;
    UInt32 nframes = out->mBuffers[0].mDataByteSize / (UInt32)(sizeof(float) * nch0);

    if (!primed && avail >= PREROLL) primed = 1;
    uint32_t take = primed ? (avail < nframes ? avail : nframes) : 0;
    if (primed && take < nframes) {
        atomic_fetch_add(&underruns, 1);
        primed = 0; /* re-buffer instead of crackling */
    }

    float g = atomic_load_explicit(&gain, memory_order_relaxed);
    UInt32 base = 0;
    for (UInt32 b = 0; b < out->mNumberBuffers; b++) {
        float *s = out->mBuffers[b].mData;
        UInt32 nch = out->mBuffers[b].mNumberChannels;
        if (!s || nch == 0) continue;
        UInt32 n = out->mBuffers[b].mDataByteSize / (UInt32)(sizeof(float) * nch);
        for (UInt32 i = 0; i < n; i++) {
            float l = 0, rr = 0;
            if (i < take) {
                uint32_t k = (r + i) & RING_MASK;
                l = ring[2 * k] * g;
                rr = ring[2 * k + 1] * g;
            }
            for (UInt32 c = 0; c < nch; c++) {
                UInt32 gch = base + c + 1; /* HAL channel numbers are 1-based */
                float v = total_ch == 1 ? (l + rr) * 0.5f : gch == ch_left ? l : gch == ch_right ? rr : 0;
                s[i * nch + c] = v;
            }
        }
        base += nch;
    }
    atomic_store_explicit(&rpos, r + take, memory_order_release);
    return noErr;
}

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
}

void audio_close(void) {
    if (dev == kAudioObjectUnknown) return;
    audio_stop();
    AudioObjectRemovePropertyListener(dev, &addr_rate, on_change, NULL);
    AudioDeviceDestroyIOProcID(dev, procid);
    dev = kAudioObjectUnknown;
}
