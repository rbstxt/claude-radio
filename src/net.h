/* Minimal HTTPS/1.1 client on top of BearSSL.
 *
 * One connection at a time, kept alive while the host stays the same.
 * Bodies are read incrementally so nothing larger than a few KB is ever
 * buffered here; callers stream whatever they need out of http_read().
 */
#ifndef CR_NET_H
#define CR_NET_H

#include <stddef.h>
#include <sys/types.h>

typedef struct {
    int status;           /* HTTP status code, 0 if no response */
    long long head_seq;   /* X-Head-Seqnum, -1 if absent */
    char location[2048];  /* Location header, "" if absent */
} http_resp;

/* Split "https://host/path" into host and path (path keeps the leading '/').
 * Returns 0 on success. */
int url_split(const char *url, char *host, size_t hostsz, const char **path);

/* Send a request and read the status line and headers.
 * body may be NULL. extra_headers is a block of "Name: value\r\n" lines or NULL.
 * Returns 0 when a response arrived (any status), -1 on network/TLS error. */
int http_request(const char *method, const char *host, const char *path,
                 const char *extra_headers, const char *body, size_t body_len,
                 http_resp *resp);

/* Read up to n bytes of the current response body.
 * Returns bytes read, 0 at end of body, -1 on error. */
ssize_t http_read(void *buf, size_t n);

/* Read and discard the rest of the body (keeps the connection reusable). */
int http_discard(void);

/* Drop the connection (e.g. after abandoning a body half way). */
void http_close(void);

/* Human readable description of the last network error. */
const char *net_last_error(void);

#endif
