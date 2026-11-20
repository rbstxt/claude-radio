/* Stereo float sample-rate converter. See resample.h. */
#include "resample.h"

#include <math.h>
#include <string.h>

#define HALF (RS_TAPS / 2)

static double bessel_i0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 64; k++) {
        double h = x / (2.0 * k);
        term *= h * h;
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

void rs_init(resampler *rs, double in_rate, double out_rate) {
    memset(rs, 0, sizeof *rs);
    rs->step = in_rate / out_rate;
    /* cut a little below the lower Nyquist; Kaiser beta 8.6 gives ~ -85 dB stopband */
    double fc = (out_rate < in_rate ? out_rate / in_rate : 1.0) * 0.97;
    double beta = 8.6, norm = bessel_i0(beta);
    for (int p = 0; p <= RS_PHASES; p++) {
        double frac = (double)p / RS_PHASES, sum = 0;
        float *row = rs->table + p * RS_TAPS;
        for (int j = 0; j < RS_TAPS; j++) {
            double d = (j - HALF + 1) - frac;
            double x = d / HALF;
            double w = fabs(x) <= 1 ? bessel_i0(beta * sqrt(1 - x * x)) / norm : 0;
            double s = fabs(d) < 1e-12 ? 1 : sin(M_PI * fc * d) / (M_PI * fc * d);
            row[j] = (float)(fc * s * w);
            sum += row[j];
        }
        for (int j = 0; j < RS_TAPS; j++) row[j] = (float)(row[j] / sum); /* unity DC gain */
    }
    /* leading silence so the first output lines up with the first input frame */
    rs->len = HALF - 1;
    rs->pos = HALF - 1;
}

int rs_process(resampler *rs, const float *in, int nin, float *out, int maxout) {
    const int cap = RS_TAPS + RS_MAXIN;
    if (nin > RS_MAXIN) nin = RS_MAXIN;
    if (rs->len + nin > cap) { /* cannot happen with a large enough maxout; stay safe */
        int drop = rs->len + nin - cap;
        memmove(rs->buf, rs->buf + drop * 2, (size_t)(rs->len - drop) * 2 * sizeof(float));
        rs->len -= drop;
        rs->pos -= drop;
    }
    memcpy(rs->buf + rs->len * 2, in, (size_t)nin * 2 * sizeof(float));
    rs->len += nin;

    int n = 0;
    while (n < maxout) {
        int i0 = (int)rs->pos;
        if (i0 + HALF >= rs->len) break;
        double ph = (rs->pos - i0) * RS_PHASES;
        int pi = (int)ph;
        float a = (float)(ph - pi);
        const float *c0 = rs->table + pi * RS_TAPS, *c1 = c0 + RS_TAPS;
        const float *x = rs->buf + (i0 - HALF + 1) * 2;
        float l = 0, r = 0;
        for (int j = 0; j < RS_TAPS; j++) {
            float c = c0[j] + a * (c1[j] - c0[j]);
            l += c * x[2 * j];
            r += c * x[2 * j + 1];
        }
        out[2 * n] = l;
        out[2 * n + 1] = r;
        n++;
        rs->pos += rs->step;
    }
    int drop = (int)rs->pos - HALF + 1;
    if (drop > 0) {
        if (drop > rs->len) drop = rs->len;
        memmove(rs->buf, rs->buf + drop * 2, (size_t)(rs->len - drop) * 2 * sizeof(float));
        rs->len -= drop;
        rs->pos -= drop;
    }
    return n;
}
