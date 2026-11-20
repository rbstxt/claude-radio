/* claude-radio: listen to Claude FM from the terminal with as little memory as possible.
 *
 *   net thread     HTTPS (BearSSL) -> segment by segment -> compressed ring (128 KB)
 *   decode thread  compressed ring -> faad2 (float) -> [resampler] -> PCM ring (64 KB)
 *   HAL IOProc     PCM ring -> output device
 *   main thread    keyboard + one status line
 */
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "audio.h"
#include "decode.h"
#include "net.h"
#include "resample.h"
#include "yt.h"

#define VERSION       "0.1.0"
#define CRING_SIZE    (128 * 1024) /* ~8 s of 128 kbps AAC */
#define START_BEHIND  2            /* start this many segments behind the live edge */
#define MAX_BEHIND    30           /* jump back to the edge if we fall further behind */
#define SEGMENT_UA    "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15\r\n"

/* ---- options ------------------------------------------------------------- */
static struct {
    const char *video;
    int itag;
    int volume; /* percent */
    int no_ui;
} opt = {NULL, 234, 100, 0};

/* ---- compressed ring (net -> decode) -------------------------------------- */
static unsigned char cring[CRING_SIZE];
static size_t c_head, c_len;
static unsigned c_gen;
static pthread_mutex_t c_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c_cv = PTHREAD_COND_INITIALIZER;

static int cring_write(const unsigned char *p, size_t n, unsigned gen) {
    pthread_mutex_lock(&c_mu);
    while (n > 0) {
        while (c_len == CRING_SIZE && c_gen == gen) pthread_cond_wait(&c_cv, &c_mu);
        if (c_gen != gen) {
            pthread_mutex_unlock(&c_mu);
            return -1;
        }
        size_t tail = (c_head + c_len) % CRING_SIZE;
        size_t k = CRING_SIZE - c_len;
        if (k > CRING_SIZE - tail) k = CRING_SIZE - tail;
        if (k > n) k = n;
        memcpy(cring + tail, p, k);
        c_len += k, p += k, n -= k;
        pthread_cond_broadcast(&c_cv);
    }
    pthread_mutex_unlock(&c_mu);
    return 0;
}

/* Blocks until data arrives or the ring is flushed (then returns 0 and updates *gen). */
static size_t cring_read(unsigned char *p, size_t max, unsigned *gen) {
    pthread_mutex_lock(&c_mu);
    while (c_len == 0 && c_gen == *gen) pthread_cond_wait(&c_cv, &c_mu);
    if (c_gen != *gen) {
        *gen = c_gen;
        pthread_mutex_unlock(&c_mu);
        return 0;
    }
    size_t k = c_len;
    if (k > CRING_SIZE - c_head) k = CRING_SIZE - c_head;
    if (k > max) k = max;
    memcpy(p, cring + c_head, k);
    c_head = (c_head + k) % CRING_SIZE;
    c_len -= k;
    pthread_cond_broadcast(&c_cv);
    pthread_mutex_unlock(&c_mu);
    return k;
}

static void cring_flush(void) {
    pthread_mutex_lock(&c_mu);
    c_head = c_len = 0;
    c_gen++;
    pthread_cond_broadcast(&c_cv);
    pthread_mutex_unlock(&c_mu);
}

static unsigned cring_gen(void) {
    pthread_mutex_lock(&c_mu);
    unsigned g = c_gen;
    pthread_mutex_unlock(&c_mu);
    return g;
}

static size_t cring_fill(void) {
    pthread_mutex_lock(&c_mu);
    size_t n = c_len;
    pthread_mutex_unlock(&c_mu);
    return n;
}

/* ---- shared status --------------------------------------------------------- */
enum { ST_CONNECTING, ST_OK, ST_RETRYING };
static _Atomic int net_state = ST_CONNECTING;
static _Atomic int have_audio;
static _Atomic int resync_req;
static _Atomic int out_rate;
static _Atomic int paused;
static pthread_mutex_t msg_mu = PTHREAD_MUTEX_INITIALIZER;
static char net_msg[512];
static char video_id_shown[32];

static void set_status(int st, const char *fmt, ...) {
    pthread_mutex_lock(&msg_mu);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(net_msg, sizeof net_msg, fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&msg_mu);
    atomic_store(&net_state, st);
}

static void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}

static long backoff_ms(int fails) {
    long ms = 1000L << (fails < 5 ? fails : 5);
    return ms > 30000 ? 30000 : ms;
}

/* ---- net thread ------------------------------------------------------------ */
static stream_info si;

static void *net_main(void *arg) {
    (void)arg;
    static char url[CR_URL_MAX];
    char host[256], prev_id[32] = "";
    const char *path;
    long long sq = -1;
    int need_resolve = 1, fails = 0, misses = 0;
    time_t last_resolve = 0;

    for (;;) {
        if (need_resolve) {
            time_t now = time(NULL);
            if (last_resolve && now - last_resolve < 10) sleep_ms(backoff_ms(fails));
            last_resolve = time(NULL);
            if (yt_resolve(opt.video, opt.itag, &si) < 0) {
                fails++;
                set_status(sq < 0 && fails == 1 ? ST_CONNECTING : ST_RETRYING, "%s", yt_last_error());
                sleep_ms(backoff_ms(fails));
                continue;
            }
            int same = strcmp(prev_id, si.video_id) == 0;
            if (!(same && sq >= 0 && si.head_sq - sq <= MAX_BEHIND && sq <= si.head_sq + 2))
                sq = si.head_sq - START_BEHIND;
            snprintf(prev_id, sizeof prev_id, "%s", si.video_id);
            pthread_mutex_lock(&msg_mu);
            snprintf(video_id_shown, sizeof video_id_shown, "%s", si.video_id);
            pthread_mutex_unlock(&msg_mu);
            need_resolve = 0;
            misses = 0;
        }
        if (atomic_exchange(&resync_req, 0)) {
            if (yt_refresh(&si) < 0) {
                need_resolve = 1;
                continue;
            }
            sq = si.head_sq - START_BEHIND;
        }
        if (si.expire && time(NULL) > si.expire - 300) {
            need_resolve = 1; /* signed URLs expire after ~6 h */
            continue;
        }
        if (yt_segment_url(&si, sq, url, sizeof url) < 0 ||
            url_split(url, host, sizeof host, &path) < 0) {
            need_resolve = 1;
            continue;
        }

        unsigned gen = cring_gen();
        http_resp r;
        if (http_request("GET", host, path, SEGMENT_UA, NULL, 0, &r) < 0) {
            fails++;
            set_status(ST_RETRYING, "%s", net_last_error());
            if (fails >= 3) need_resolve = 1;
            else sleep_ms(backoff_ms(fails));
            continue;
        }
        if (r.status == 200) {
            unsigned char buf[4096];
            ssize_t n;
            int aborted = 0;
            while ((n = http_read(buf, sizeof buf)) > 0)
                if (cring_write(buf, (size_t)n, gen) < 0) {
                    aborted = 1;
                    break;
                }
            if (aborted) { /* flushed (resume / resync) while downloading */
                http_close();
                continue;
            }
            sq++; /* even on a cut-off body: replaying half a segment sounds worse than a skip */
            if (n < 0) {
                fails++;
                set_status(ST_RETRYING, "%s", net_last_error());
                continue;
            }
            fails = 0;
            misses = 0;
            if (atomic_load(&net_state) != ST_OK) set_status(ST_OK, "");
            continue;
        }
        http_discard();
        if (r.status == 404 || r.status == 204) {
            if (r.head_seq >= 0 && sq < r.head_seq - MAX_BEHIND) {
                sq = r.head_seq - START_BEHIND;
                continue;
            }
            if (++misses > 15) {
                need_resolve = 1;
                set_status(ST_RETRYING, "stream is not advancing");
                continue;
            }
            sleep_ms(1000);
            continue;
        }
        if (r.status == 401 || r.status == 403 || r.status == 410) {
            need_resolve = 1; /* signature expired or rejected */
            continue;
        }
        fails++;
        set_status(ST_RETRYING, "segment server returned HTTP %d", r.status);
        if (fails >= 3) need_resolve = 1;
        else sleep_ms(backoff_ms(fails));
    }
    return NULL;
}

/* ---- decode thread --------------------------------------------------------- */
static resampler rs;
static float rs_out[(256 * 9 + 16) * 2];
static int rs_in_rate, rs_out_rate;
static unsigned dec_gen;

/* Push frames into the PCM ring, waiting for room. -1 if the stream was flushed. */
static int pcm_push(const float *p, size_t n) {
    while (n > 0) {
        if (cring_gen() != dec_gen) return -1;
        size_t sp = pcm_space();
        if (sp < n && sp < PCM_RING_FRAMES / 2) {
            /* sleep until about half the ring is free: few wakeups per second */
            int rate = atomic_load(&out_rate);
            long ms = rate > 0 ? (long)((PCM_RING_FRAMES / 2 - sp) * 1000L / rate) : 20;
            if (atomic_load(&paused) || ms < 5) ms = ms < 5 ? 5 : 200;
            sleep_ms(ms);
            continue;
        }
        size_t k = sp < n ? sp : n;
        pcm_write(p, k);
        p += k * 2;
        n -= k;
    }
    return 0;
}

static int play_sink(const float *stereo, int frames, int rate, void *ctx) {
    (void)ctx;
    atomic_store(&have_audio, 1);
    int orate = atomic_load(&out_rate);
    if (rate != rs_in_rate || orate != rs_out_rate) {
        if (rate != orate) rs_init(&rs, rate, orate);
        rs_in_rate = rate;
        rs_out_rate = orate;
    }
    if (rate == orate) return pcm_push(stereo, (size_t)frames) < 0;
    for (int off = 0; off < frames; off += 256) {
        int nin = frames - off < 256 ? frames - off : 256;
        int n = rs_process(&rs, stereo + off * 2, nin, rs_out, (int)(sizeof rs_out / sizeof *rs_out / 2));
        if (pcm_push(rs_out, (size_t)n) < 0) return 1;
    }
    return 0;
}

static void *decode_main(void *arg) {
    (void)arg;
    static unsigned char buf[4096];
    dec_gen = cring_gen();
    for (;;) {
        unsigned g = dec_gen;
        size_t n = cring_read(buf, sizeof buf, &g);
        if (g != dec_gen) {
            dec_gen = g;
            dec_reset();
            continue;
        }
        if (dec_feed(buf, n, play_sink, NULL)) {
            dec_reset();
            dec_gen = cring_gen();
        }
    }
    return NULL;
}

/* ---- terminal -------------------------------------------------------------- */
static struct termios tio_saved;
static int tty_raw;
static volatile sig_atomic_t quit;

static void tty_enter(void) {
    struct termios t = tio_saved;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    (void)!write(1, "\033[?25l", 6);
    tty_raw = 1;
}

static void tty_leave(void) {
    if (!tty_raw) return;
    tcsetattr(0, TCSANOW, &tio_saved);
    (void)!write(1, "\033[?25h", 6);
    tty_raw = 0;
}

static void on_quit(int sig) { (void)sig; quit = 1; }

static void on_tstp(int sig) {
    (void)sig;
    tty_leave();
    signal(SIGTSTP, SIG_DFL);
    raise(SIGTSTP);
}

static void on_cont(int sig) {
    (void)sig;
    tty_enter();
    signal(SIGTSTP, on_tstp);
}

static int term_cols(void) {
    struct winsize ws;
    return ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 ? ws.ws_col : 80;
}

static void apply_volume(int muted) {
    float v = muted ? 0.0f : opt.volume / 100.0f;
    audio_set_gain(v * v); /* rough perceptual curve; 100% is exactly unity gain */
}

static const char *quality_name(void) {
    return opt.itag == 233 ? "HE-AAC 48k" : "AAC 128k";
}

/* Build the one-line status. Returns a short state word for non-tty logging. */
static const char *status_line(char *out, size_t sz, int muted, double listened) {
    const char *state;
    char msg[512];
    pthread_mutex_lock(&msg_mu);
    snprintf(msg, sizeof msg, "%s", net_msg);
    pthread_mutex_unlock(&msg_mu);
    int st = atomic_load(&net_state);
    double bps = dec_bytes_per_sec();
    int rate = atomic_load(&out_rate);
    double buffered = (bps > 0 ? cring_fill() / bps : 0) + (rate > 0 ? (double)pcm_fill() / rate : 0);

    if (atomic_load(&paused)) state = "paused";
    else if (st == ST_RETRYING) state = "reconnecting";
    else if (!atomic_load(&have_audio)) state = "connecting";
    else if (buffered < 0.05) state = "buffering";
    else state = "playing";

    long t = (long)listened;
    int n = snprintf(out, sz, "Claude FM  %s  %s  vol %d%%%s  buf %.1fs  %02ld:%02ld:%02ld  %s",
                     strcmp(state, "playing") == 0 ? "\033[1;31m\xe2\x97\x8f LIVE\033[0m" : "\xe2\x97\x8b LIVE",
                     state, opt.volume, muted ? " (muted)" : "", buffered, t / 3600, t / 60 % 60,
                     t % 60, quality_name());
    unsigned long drops = audio_underruns();
    if (drops && n > 0 && (size_t)n < sz) n += snprintf(out + n, sz - (size_t)n, "  dropouts %lu", drops);
    if ((st != ST_OK || !atomic_load(&have_audio)) && msg[0] && n > 0 && (size_t)n < sz)
        snprintf(out + n, sz - (size_t)n, "  - %s", msg);
    return state;
}

/* Print within the terminal width; the escape sequences above do not take columns. */
static void draw(const char *line) {
    int cols = term_cols() - 1, w = 0;
    char out[1024];
    size_t o = 0;
    out[o++] = '\r';
    for (const char *p = line; *p && o < sizeof out - 8; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\033') { /* copy the whole escape sequence */
            while (*p && o < sizeof out - 8) {
                out[o++] = *p;
                if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) break;
                p++;
            }
            continue;
        }
        if ((c & 0xC0) != 0x80) {
            if (w >= cols) break;
            w++;
        }
        out[o++] = *p;
    }
    memcpy(out + o, "\033[0m\033[K", 7);
    o += 7;
    (void)!write(1, out, o);
}

/* ---- --decode (for testing the decoder) ------------------------------------ */
static int file_sink(const float *stereo, int frames, int rate, void *ctx) {
    (void)rate, (void)ctx;
    fwrite(stereo, sizeof(float) * 2, (size_t)frames, stdout);
    return 0;
}

static int decode_file(const char *path) {
    FILE *f = strcmp(path, "-") ? fopen(path, "rb") : stdin;
    if (!f) {
        perror(path);
        return 1;
    }
    if (dec_open() < 0) return 1;
    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) dec_feed(buf, n, file_sink, NULL);
    fprintf(stderr, "decode errors: %lu\n", dec_errors());
    return 0;
}

/* ---- main ------------------------------------------------------------------ */
static void usage(FILE *f) {
    fprintf(f,
            "usage: claude-radio [options]\n"
            "\n"
            "Play the Claude FM live stream (https://clau.de/radio) in the terminal.\n"
            "\n"
            "options:\n"
            "  -q, --quality high|low  high: AAC-LC ~128 kbps (default, same as the browser)\n"
            "                          low:  HE-AAC ~48 kbps (less bandwidth)\n"
            "  -V, --volume N          start volume 0-100 (default 100)\n"
            "      --video ID          play this YouTube live video instead of clau.de/radio\n"
            "      --no-ui             no status line or keys; log to stderr\n"
            "      --decode FILE       decode an ADTS file to raw float32 stereo on stdout\n"
            "  -h, --help              show this help\n"
            "  -v, --version           show the version\n"
            "\n"
            "keys: space pause/resume   + / - volume   m mute   q quit\n");
}

static int start_thread(void *(*fn)(void *)) {
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 256 * 1024);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&t, &a, fn, NULL);
    pthread_attr_destroy(&a);
    return r;
}

int main(int argc, char **argv) {
    static const struct option longopts[] = {
        {"quality", required_argument, 0, 'q'}, {"volume", required_argument, 0, 'V'},
        {"video", required_argument, 0, 1},     {"no-ui", no_argument, 0, 2},
        {"decode", required_argument, 0, 3},    {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},       {0, 0, 0, 0}};
    const char *decode_path = NULL;
    int c;
    while ((c = getopt_long(argc, argv, "q:V:hv", longopts, NULL)) != -1) {
        switch (c) {
        case 'q':
            if (!strcmp(optarg, "high")) opt.itag = 234;
            else if (!strcmp(optarg, "low")) opt.itag = 233;
            else {
                fprintf(stderr, "claude-radio: --quality must be high or low\n");
                return 2;
            }
            break;
        case 'V': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*end || v < 0 || v > 100) {
                fprintf(stderr, "claude-radio: --volume must be 0-100\n");
                return 2;
            }
            opt.volume = (int)v;
            break;
        }
        case 1: opt.video = optarg; break;
        case 2: opt.no_ui = 1; break;
        case 3: decode_path = optarg; break;
        case 'h': usage(stdout); return 0;
        case 'v': printf("claude-radio %s\n", VERSION); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (optind < argc) {
        usage(stderr);
        return 2;
    }
    if (decode_path) return decode_file(decode_path);

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_quit; /* no SA_RESTART: poll() returns at once */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    if (dec_open() < 0) {
        fprintf(stderr, "claude-radio: could not initialise the AAC decoder\n");
        return 1;
    }
    if (audio_open() < 0) {
        fprintf(stderr, "claude-radio: %s\n", audio_last_error());
        return 1;
    }
    atomic_store(&out_rate, (int)audio_rate());
    int muted = 0;
    apply_volume(muted);

    if (start_thread(net_main) || start_thread(decode_main)) {
        fprintf(stderr, "claude-radio: could not start threads\n");
        return 1;
    }
    if (audio_start() < 0) {
        fprintf(stderr, "claude-radio: %s\n", audio_last_error());
        return 1;
    }

    int ui = !opt.no_ui && isatty(0) && isatty(1);
    if (ui && tcgetattr(0, &tio_saved) == 0) {
        tty_enter();
        signal(SIGTSTP, on_tstp);
        signal(SIGCONT, on_cont);
        printf("\033[2mspace pause \xc2\xb7 +/- volume \xc2\xb7 m mute \xc2\xb7 q quit\033[0m\n");
        fflush(stdout);
    } else {
        ui = 0;
    }

    struct timespec last, now;
    clock_gettime(CLOCK_MONOTONIC, &last);
    double listened = 0;
    char line[1024], prev_state[32] = "";
    int device_retry = 0;
    while (!quit) {
        struct pollfd p = {0, POLLIN, 0};
        int pr = poll(&p, ui ? 1 : 0, 250);
        if (pr > 0 && (p.revents & POLLIN)) {
            char k;
            if (read(0, &k, 1) == 1) {
                if (k == 'q' || k == 'Q' || k == 3) break;
                if (k == ' ' || k == 'p') {
                    if (atomic_load(&paused)) {
                        cring_flush();                 /* drop stale audio ... */
                        atomic_store(&resync_req, 1);  /* ... and rejoin the live edge */
                        pcm_drop();
                        atomic_store(&paused, 0);
                        audio_start();
                    } else {
                        atomic_store(&paused, 1);
                        audio_stop();
                    }
                }
                if (k == '+' || k == '=') opt.volume = opt.volume + 5 > 100 ? 100 : opt.volume + 5;
                if (k == '-' || k == '_') opt.volume = opt.volume - 5 < 0 ? 0 : opt.volume - 5;
                if (k == 'm' || k == 'M') muted = !muted;
                apply_volume(muted);
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        double dt = (double)(now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9;
        last = now;

        /* default output changed (headphones plugged in, AirPods, sample rate...) */
        if (audio_take_device_change() || device_retry) {
            audio_close();
            if (audio_open() == 0) {
                device_retry = 0;
                atomic_store(&out_rate, (int)audio_rate());
                pcm_drop();
                if (!atomic_load(&paused)) audio_start();
            } else {
                device_retry = 1;
                sleep_ms(500);
            }
        }

        const char *state = status_line(line, sizeof line, muted, listened);
        if (!strcmp(state, "playing")) listened += dt;
        if (ui) {
            draw(line);
        } else if (strcmp(state, prev_state) != 0) {
            pthread_mutex_lock(&msg_mu);
            int show_msg = strcmp(state, "playing") != 0 && net_msg[0];
            fprintf(stderr, "claude-radio: %s%s%s\n", state, show_msg ? ": " : "", show_msg ? net_msg : "");
            pthread_mutex_unlock(&msg_mu);
            if (!strcmp(state, "playing"))
                fprintf(stderr, "claude-radio: %s via %s, video %s\n", quality_name(),
                        audio_device_name()[0] ? audio_device_name() : "default output", video_id_shown);
        }
        snprintf(prev_state, sizeof prev_state, "%s", state);
    }

    if (!ui) {
        long t = (long)listened;
        fprintf(stderr, "claude-radio: stopped after %02ld:%02ld:%02ld of playback, %lu dropouts, %lu decode errors\n",
                t / 3600, t / 60 % 60, t % 60, audio_underruns(), dec_errors());
    }
    audio_close();
    if (ui) {
        tty_leave();
        printf("\n");
    }
    return 0;
}
