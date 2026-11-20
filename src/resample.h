/* Stereo float sample-rate converter (windowed-sinc, Kaiser window).
 * Only used when the output device does not run at the stream's rate;
 * equal rates are passed through untouched. */
#ifndef CR_RESAMPLE_H
#define CR_RESAMPLE_H

#define RS_TAPS   32
#define RS_PHASES 128
#define RS_MAXIN  1024 /* max input frames per rs_process call */

typedef struct {
    double step; /* input frames per output frame */
    double pos;  /* read position in buf, in input frames */
    int len;     /* frames held in buf */
    float buf[(RS_TAPS + RS_MAXIN) * 2];
    float table[(RS_PHASES + 1) * RS_TAPS];
} resampler;

void rs_init(resampler *rs, double in_rate, double out_rate);

/* Consume nin (<= RS_MAXIN) stereo frames, write up to maxout frames.
 * maxout must be >= nin * out_rate / in_rate + 2. Returns frames written. */
int rs_process(resampler *rs, const float *in, int nin, float *out, int maxout);

#endif
