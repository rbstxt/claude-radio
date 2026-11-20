/* Minimal HTTPS/1.1 client on top of BearSSL. See net.h. */
#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bearssl.h"
#include "trust_anchors.h"

#define CONNECT_TIMEOUT_MS 10000
#define IO_TIMEOUT_SEC     20

static int fd = -1;
static br_ssl_client_context sc;
static br_x509_minimal_context xc;
static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
static br_sslio_context ioc;
static int tls_ready;
static char cur_host[256];   /* host of the open connection */
static char sess_host[256];  /* host the cached TLS session belongs to */

/* current response body state */
static int body_done = 1;
static int body_chunked;
static int chunk_in_data;    /* chunked: currently inside chunk data */
static int chunk_need_crlf;  /* chunked: CRLF after chunk data still unread */
static long long body_left;  /* content-length remaining, or chunk remaining */
static int body_until_close;
static int conn_close;

static char errbuf[256];

static void seterr(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, sizeof errbuf, fmt, ap);
    va_end(ap);
}

const char *net_last_error(void) { return errbuf[0] ? errbuf : "unknown error"; }

int url_split(const char *url, char *host, size_t hostsz, const char **path) {
    if (strncmp(url, "https://", 8) != 0) return -1;
    const char *h = url + 8;
    const char *slash = strchr(h, '/');
    size_t hl = slash ? (size_t)(slash - h) : strlen(h);
    if (hl == 0 || hl >= hostsz) return -1;
    memcpy(host, h, hl);
    host[hl] = 0;
    *path = slash ? slash : "/";
    return 0;
}

/* ---- socket ---------------------------------------------------------- */

static int sock_read(void *ctx, unsigned char *buf, size_t len) {
    int s = *(int *)ctx;
    for (;;) {
        ssize_t r = read(s, buf, len);
        if (r > 0) return (int)r;
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) seterr("network read timed out");
        return -1;
    }
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len) {
    int s = *(int *)ctx;
    for (;;) {
        ssize_t r = write(s, buf, len);
        if (r > 0) return (int)r;
        if (r < 0 && errno == EINTR) continue;
        return -1;
    }
}

static int tcp_connect(const char *host) {
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int e = getaddrinfo(host, "443", &hints, &res);
    if (e != 0) {
        seterr("DNS lookup failed for %s: %s", host, gai_strerror(e));
        return -1;
    }
    int s = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0) continue;
        int fl = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, fl | O_NONBLOCK);
        int r = connect(s, ai->ai_addr, ai->ai_addrlen);
        if (r < 0 && errno == EINPROGRESS) {
            struct pollfd p = {s, POLLOUT, 0};
            r = poll(&p, 1, CONNECT_TIMEOUT_MS) == 1 ? 0 : -1;
            if (r == 0) {
                int soerr = 0;
                socklen_t sl = sizeof soerr;
                getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                if (soerr) r = -1;
            }
        }
        if (r == 0) {
            fcntl(s, F_SETFL, fl);
            break;
        }
        close(s);
        s = -1;
    }
    freeaddrinfo(res);
    if (s < 0) {
        seterr("could not connect to %s", host);
        return -1;
    }
    int one = 1;
    struct timeval tv = {IO_TIMEOUT_SEC, 0};
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return s;
}

/* ---- connection ------------------------------------------------------ */

void http_close(void) {
    if (fd >= 0) close(fd);
    fd = -1;
    cur_host[0] = 0;
    body_done = 1;
}

static void tls_error(const char *what) {
    int e = tls_ready ? br_ssl_engine_last_error(&sc.eng) : 0;
    if (e == BR_ERR_X509_NOT_TRUSTED)
        seterr("%s: certificate not trusted (TLS error %d; root CAs may need updating)", what, e);
    else if (e != 0)
        seterr("%s: TLS error %d", what, e);
    else if (!errbuf[0])
        seterr("%s: connection closed", what);
}

static int conn_open(const char *host) {
    fd = tcp_connect(host);
    if (fd < 0) return -1;
    if (!tls_ready) {
        br_ssl_client_init_full(&sc, &xc, TAs, TAs_NUM);
        br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof iobuf, 1);
        tls_ready = 1;
    }
    int resume = strcmp(sess_host, host) == 0;
    if (!br_ssl_client_reset(&sc, host, resume)) {
        tls_error("TLS setup");
        http_close();
        return -1;
    }
    br_sslio_init(&ioc, &sc.eng, sock_read, &fd, sock_write, &fd);
    snprintf(cur_host, sizeof cur_host, "%s", host);
    snprintf(sess_host, sizeof sess_host, "%s", host);
    return 0;
}

/* Read one header line (without CRLF). Over-long lines are truncated. */
static int read_line(char *line, size_t sz) {
    size_t n = 0;
    for (;;) {
        unsigned char c;
        if (br_sslio_read(&ioc, &c, 1) != 1) return -1;
        if (c == '\n') break;
        if (n + 1 < sz) line[n++] = (char)c;
    }
    if (n > 0 && line[n - 1] == '\r') n--;
    line[n] = 0;
    return (int)n;
}

static int write_str(const char *s) { return br_sslio_write_all(&ioc, s, strlen(s)); }

static int send_and_read_head(const char *method, const char *host, const char *path,
                              const char *extra, const char *body, size_t body_len,
                              http_resp *resp) {
    char line[4096];
    snprintf(line, sizeof line,
             " HTTP/1.1\r\nHost: %s\r\nAccept: */*\r\nConnection: keep-alive\r\n", host);
    if (write_str(method) < 0 || write_str(" ") < 0 || write_str(path) < 0 ||
        write_str(line) < 0)
        return -1;
    if (extra && write_str(extra) < 0) return -1;
    if (body) {
        snprintf(line, sizeof line, "Content-Length: %zu\r\n", body_len);
        if (write_str(line) < 0) return -1;
    }
    if (write_str("\r\n") < 0) return -1;
    if (body && br_sslio_write_all(&ioc, body, body_len) < 0) return -1;
    if (br_sslio_flush(&ioc) < 0) return -1;

    if (read_line(line, sizeof line) < 0) return -1;
    if (strncmp(line, "HTTP/1.", 7) != 0 || strlen(line) < 12) {
        seterr("malformed HTTP response");
        return -1;
    }
    resp->status = atoi(line + 9);

    long long clen = -1;
    body_chunked = 0;
    conn_close = 0;
    for (;;) {
        int n = read_line(line, sizeof line);
        if (n < 0) return -1;
        if (n == 0) break;
        char *v = strchr(line, ':');
        if (!v) continue;
        *v++ = 0;
        while (*v == ' ' || *v == '\t') v++;
        if (!strcasecmp(line, "Content-Length")) clen = atoll(v);
        else if (!strcasecmp(line, "Transfer-Encoding")) body_chunked = strcasestr(v, "chunked") != NULL;
        else if (!strcasecmp(line, "Connection")) conn_close = strcasestr(v, "close") != NULL;
        else if (!strcasecmp(line, "Location")) snprintf(resp->location, sizeof resp->location, "%s", v);
        else if (!strcasecmp(line, "X-Head-Seqnum")) resp->head_seq = atoll(v);
    }

    body_done = 0;
    body_until_close = 0;
    chunk_in_data = 0;
    chunk_need_crlf = 0;
    if (!strcmp(method, "HEAD") || resp->status == 204 || resp->status == 304 ||
        resp->status / 100 == 1) {
        body_done = 1;
    } else if (body_chunked) {
        body_left = 0;
    } else if (clen >= 0) {
        body_left = clen;
        if (clen == 0) body_done = 1;
    } else {
        body_until_close = 1;
        conn_close = 1;
    }
    return 0;
}

int http_request(const char *method, const char *host, const char *path,
                 const char *extra, const char *body, size_t body_len, http_resp *resp) {
    errbuf[0] = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        resp->status = 0;
        resp->head_seq = -1;
        resp->location[0] = 0;
        /* reuse only a clean keep-alive connection to the same host */
        if (fd >= 0 && (strcmp(cur_host, host) != 0 || !body_done || conn_close)) http_close();
        int reused = fd >= 0;
        if (!reused && conn_open(host) < 0) return -1;
        if (send_and_read_head(method, host, path, extra, body, body_len, resp) == 0) return 0;
        tls_error(host);
        http_close();
        if (!reused) return -1; /* a stale keep-alive connection gets one retry */
        errbuf[0] = 0;
    }
    return -1;
}

static int chunk_next(void) {
    char line[128];
    if (chunk_need_crlf) {
        if (read_line(line, sizeof line) < 0) return -1;
        chunk_need_crlf = 0;
    }
    if (read_line(line, sizeof line) < 0) return -1;
    body_left = strtoll(line, NULL, 16);
    if (body_left > 0) {
        chunk_in_data = 1;
        return 1;
    }
    /* last chunk: skip trailers */
    for (;;) {
        int n = read_line(line, sizeof line);
        if (n < 0) return -1;
        if (n == 0) break;
    }
    body_done = 1;
    return 0;
}

ssize_t http_read(void *buf, size_t n) {
    if (body_done || fd < 0 || n == 0) return 0;
    if (body_until_close) {
        int r = br_sslio_read(&ioc, buf, n);
        if (r <= 0) {
            body_done = 1;
            return 0;
        }
        return r;
    }
    if (body_chunked && !chunk_in_data) {
        int r = chunk_next();
        if (r < 0) goto fail;
        if (r == 0) return 0;
    }
    size_t want = (long long)n < body_left ? n : (size_t)body_left;
    int r = br_sslio_read(&ioc, buf, want);
    if (r <= 0) goto fail;
    body_left -= r;
    if (body_left == 0) {
        if (body_chunked) {
            chunk_in_data = 0;
            chunk_need_crlf = 1;
        } else {
            body_done = 1;
        }
    }
    return r;
fail:
    tls_error("reading response");
    http_close();
    return -1;
}

int http_discard(void) {
    char tmp[2048];
    ssize_t r;
    while ((r = http_read(tmp, sizeof tmp)) > 0) {}
    return r < 0 ? -1 : 0;
}
