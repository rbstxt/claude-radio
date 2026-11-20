/* Resolve the Claude FM live stream to an audio-only HLS segment URL. See yt.h.
 *
 * Everything is parsed as a stream with small fixed buffers: the player
 * response (~50 KB of JSON) is never held in memory, we only pick out the
 * few strings we need while it flows past.
 */
#include "yt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"

#define RADIO_HOST    "clau.de"
#define RADIO_PATH    "/radio"
#define DEFAULT_VIDEO "tRsQsTMvPNg" /* used only if clau.de/radio cannot be reached */

/* Innertube client that hands out HLS manifests without a JS player or PO token.
 * Mirrors yt-dlp's 'visionos' client (yt_dlp/extractor/youtube/_base.py). */
#define CLIENT_NAME    "VISIONOS"
#define CLIENT_ID      "101"
#define CLIENT_VERSION "1.02"
#define CLIENT_UA                                                                  \
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, " \
    "like Gecko) Version/26.0 Safari/605.1.15"

static char errbuf[512];

static void seterr(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, sizeof errbuf, fmt, ap);
    va_end(ap);
}

const char *yt_last_error(void) { return errbuf; }

/* ---- streaming string scanner ------------------------------------------
 * Finds `needle` in a byte stream and captures the JSON string value that
 * follows it (up to the closing quote), unescaping \/, \" and \uXXXX (ASCII). */
typedef struct {
    const char *needle;
    size_t nlen, pos;
    int capturing, done, esc, uhex, ucode;
    char *out;
    size_t outsz, outlen;
} scanner;

static void scan_init(scanner *s, const char *needle, char *out, size_t outsz) {
    memset(s, 0, sizeof *s);
    s->needle = needle;
    s->nlen = strlen(needle);
    s->out = out;
    s->outsz = outsz;
    out[0] = 0;
}

static void scan_put(scanner *s, char c) {
    if (s->outlen + 1 < s->outsz) {
        s->out[s->outlen++] = c;
        s->out[s->outlen] = 0;
    }
}

static void scan_feed(scanner *s, const char *buf, size_t n) {
    for (size_t i = 0; i < n && !s->done; i++) {
        char c = buf[i];
        if (!s->capturing) {
            if (c == s->needle[s->pos]) s->pos++;
            else s->pos = (c == s->needle[0]) ? 1 : 0;
            if (s->pos == s->nlen) s->capturing = 1;
            continue;
        }
        if (s->uhex) {
            int v = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0;
            s->ucode = s->ucode * 16 + v;
            if (--s->uhex == 0 && s->ucode < 0x80) scan_put(s, (char)s->ucode);
        } else if (s->esc) {
            s->esc = 0;
            if (c == 'u') {
                s->uhex = 4;
                s->ucode = 0;
            } else {
                scan_put(s, c == 'n' ? '\n' : c == 't' ? '\t' : c);
            }
        } else if (c == '\\') {
            s->esc = 1;
        } else if (c == '"') {
            s->done = 1;
        } else {
            scan_put(s, c);
        }
    }
}

/* ---- line reader over an HTTP body -------------------------------------- */
typedef int (*line_cb)(const char *line, void *ctx); /* return 1 to stop early */

static int body_lines(line_cb cb, void *ctx) {
    static char line[CR_URL_MAX + 512];
    char buf[2048];
    size_t len = 0;
    int stop = 0;
    ssize_t r;
    while ((r = http_read(buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] == '\n') {
                if (len && line[len - 1] == '\r') len--;
                line[len] = 0;
                if (!stop) stop = cb(line, ctx);
                len = 0;
            } else if (len + 1 < sizeof line) {
                line[len++] = buf[i];
            }
        }
    }
    if (r < 0) return -1;
    if (len && !stop) {
        line[len] = 0;
        cb(line, ctx);
    }
    return 0;
}

/* ---- helpers ------------------------------------------------------------ */

static long long url_number(const char *url, const char *key) {
    const char *p = strstr(url, key);
    return p ? strtoll(p + strlen(key), NULL, 10) : -1;
}

static int valid_id(const char *s, size_t n) {
    if (n < 6 || n >= 32) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

static int id_from_url(const char *u, char *out, size_t outsz) {
    const char *keys[] = {"v=", "/live/", "youtu.be/"};
    for (size_t k = 0; k < sizeof keys / sizeof *keys; k++) {
        const char *p = strstr(u, keys[k]);
        if (!p) continue;
        p += strlen(keys[k]);
        size_t n = strcspn(p, "&?#/");
        if (valid_id(p, n) && n < outsz) {
            memcpy(out, p, n);
            out[n] = 0;
            return 0;
        }
    }
    return -1;
}

static int get_video_id(char *out, size_t outsz) {
    http_resp r;
    if (http_request("GET", RADIO_HOST, RADIO_PATH, "User-Agent: claude-radio\r\n", NULL, 0, &r) == 0) {
        http_discard();
        if (r.status / 100 == 3 && id_from_url(r.location, out, outsz) == 0) return 0;
    }
    fprintf(stderr, "claude-radio: could not follow https://" RADIO_HOST RADIO_PATH
                    ", using the last known stream\n");
    snprintf(out, outsz, "%s", DEFAULT_VIDEO);
    return 0;
}

static int get_visitor(char *out, size_t outsz) {
    http_resp r;
    if (http_request("GET", "www.youtube.com", "/sw.js_data", "User-Agent: " CLIENT_UA "\r\n",
                     NULL, 0, &r) < 0) {
        seterr("youtube.com: %s", net_last_error());
        return -1;
    }
    /* the visitor id is the first "Cg..." string in this small JSON blob */
    scanner s;
    scan_init(&s, "\"Cg", out + 2, outsz - 2);
    char buf[1024];
    ssize_t n;
    while ((n = http_read(buf, sizeof buf)) > 0) scan_feed(&s, buf, (size_t)n);
    if (r.status != 200 || !s.done || s.outlen < 8 || s.outlen + 3 >= outsz - 2) {
        seterr("youtube.com: could not get a visitor id (HTTP %d)", r.status);
        return -1;
    }
    out[0] = 'C';
    out[1] = 'g';
    return 0;
}

static int get_manifest(const char *video_id, const char *visitor, char *out, size_t outsz) {
    char headers[4096], body[4096];
    snprintf(headers, sizeof headers,
             "Content-Type: application/json\r\n"
             "User-Agent: " CLIENT_UA "\r\n"
             "Origin: https://www.youtube.com\r\n"
             "X-YouTube-Client-Name: " CLIENT_ID "\r\n"
             "X-YouTube-Client-Version: " CLIENT_VERSION "\r\n"
             "X-Goog-Visitor-Id: %s\r\n",
             visitor);
    int blen = snprintf(body, sizeof body,
                        "{\"context\":{\"client\":{\"clientName\":\"" CLIENT_NAME "\","
                        "\"clientVersion\":\"" CLIENT_VERSION "\",\"deviceMake\":\"Apple\","
                        "\"deviceModel\":\"RealityDevice17,1\",\"osName\":\"visionOS\","
                        "\"osVersion\":\"26.5.23O471\",\"hl\":\"en\",\"visitorData\":\"%s\"}},"
                        "\"videoId\":\"%s\",\"contentCheckOk\":true,\"racyCheckOk\":true}",
                        visitor, video_id);
    http_resp r;
    if (http_request("POST", "www.youtube.com", "/youtubei/v1/player?prettyPrint=false", headers,
                     body, (size_t)blen, &r) < 0) {
        seterr("youtube.com: %s", net_last_error());
        return -1;
    }
    char status[64], reason[256];
    scanner s_url, s_status, s_reason;
    scan_init(&s_url, "\"hlsManifestUrl\":\"", out, outsz);
    scan_init(&s_status, "\"status\":\"", status, sizeof status);
    scan_init(&s_reason, "\"reason\":\"", reason, sizeof reason);
    char buf[2048];
    ssize_t n;
    while ((n = http_read(buf, sizeof buf)) > 0) {
        scan_feed(&s_url, buf, (size_t)n);
        scan_feed(&s_status, buf, (size_t)n);
        scan_feed(&s_reason, buf, (size_t)n);
    }
    if (n < 0) {
        seterr("youtube.com: %s", net_last_error());
        return -1;
    }
    if (r.status != 200) {
        seterr("YouTube player API returned HTTP %d", r.status);
        return -1;
    }
    if (strcmp(status, "OK") != 0) {
        seterr("YouTube refused the stream (%s%s%s)", status[0] ? status : "no status",
               reason[0] ? ": " : "", reason);
        return -1;
    }
    if (!s_url.done || strncmp(out, "https://", 8) != 0) {
        seterr("video %s is not a live HLS stream (is Claude FM on air?)", video_id);
        return -1;
    }
    return 0;
}

static int fetch_lines(const char *url, line_cb cb, void *ctx, int *status) {
    char host[256];
    const char *path;
    if (url_split(url, host, sizeof host, &path) < 0) {
        seterr("bad URL in playlist");
        return -1;
    }
    http_resp r;
    if (http_request("GET", host, path, "User-Agent: " CLIENT_UA "\r\n", NULL, 0, &r) < 0) {
        seterr("%s: %s", host, net_last_error());
        return -1;
    }
    *status = r.status;
    if (r.status != 200) {
        http_discard();
        seterr("%s returned HTTP %d", host, r.status);
        return -1;
    }
    if (body_lines(cb, ctx) < 0) {
        seterr("%s: %s", host, net_last_error());
        return -1;
    }
    return 0;
}

typedef struct {
    char want[24];
    char *out;
    size_t outsz;
} master_ctx;

static int master_line(const char *line, void *vctx) {
    master_ctx *c = vctx;
    if (strncmp(line, "#EXT-X-MEDIA:", 13) != 0 || !strstr(line, "TYPE=AUDIO") ||
        !strstr(line, c->want))
        return 0;
    const char *u = strstr(line, "URI=\"");
    if (!u) return 0;
    u += 5;
    size_t n = strcspn(u, "\"");
    if (n >= c->outsz) return 0;
    memcpy(c->out, u, n);
    c->out[n] = 0;
    return 1;
}

static int media_line(const char *line, void *vctx) {
    stream_info *si = vctx;
    if (strncmp(line, "https://", 8) == 0 && strstr(line, "/sq/") && strlen(line) < CR_URL_MAX)
        strcpy(si->seg_url, line); /* keep only the newest */
    return 0;
}

int yt_refresh(stream_info *si) {
    int status = 0;
    si->seg_url[0] = 0;
    if (fetch_lines(si->media_url, media_line, si, &status) < 0) return -1;
    if (!si->seg_url[0]) {
        seterr("media playlist had no segments");
        return -1;
    }
    si->head_sq = url_number(si->seg_url, "/sq/");
    si->expire = url_number(si->seg_url, "/expire/");
    if (si->expire < 0) si->expire = 0;
    return si->head_sq >= 0 ? 0 : -1;
}

int yt_resolve(const char *video_id, int itag, stream_info *si) {
    static char manifest[CR_URL_MAX];
    char visitor[2048];
    errbuf[0] = 0;
    memset(si, 0, sizeof *si);

    if (video_id && *video_id) snprintf(si->video_id, sizeof si->video_id, "%s", video_id);
    else get_video_id(si->video_id, sizeof si->video_id);

    if (get_visitor(visitor, sizeof visitor) < 0) return -1;
    if (get_manifest(si->video_id, visitor, manifest, sizeof manifest) < 0) return -1;

    master_ctx mc;
    snprintf(mc.want, sizeof mc.want, "GROUP-ID=\"%d\"", itag);
    mc.out = si->media_url;
    mc.outsz = sizeof si->media_url;
    si->media_url[0] = 0;
    int status = 0;
    if (fetch_lines(manifest, master_line, &mc, &status) < 0) return -1;
    if (!si->media_url[0]) {
        seterr("the stream has no audio-only rendition %d", itag);
        return -1;
    }
    return yt_refresh(si);
}

int yt_segment_url(const stream_info *si, long long sq, char *out, size_t outsz) {
    const char *p = strstr(si->seg_url, "/sq/");
    if (!p) return -1;
    p += 4;
    const char *q = p + strspn(p, "0123456789");
    int n = snprintf(out, outsz, "%.*s%lld%s", (int)(p - si->seg_url), si->seg_url, sq, q);
    return (n > 0 && (size_t)n < outsz) ? 0 : -1;
}
