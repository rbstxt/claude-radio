/* Resolve the Claude FM live stream to an audio-only HLS segment URL.
 *
 *   clau.de/radio        -> 302 to watch?v=<id>          (skipped with --video)
 *   youtube.com/sw.js_data -> visitorData
 *   youtubei/v1/player   -> streamingData.hlsManifestUrl (VISIONOS client: no JS, no PO token)
 *   master .m3u8         -> audio rendition for the wanted itag
 *   media .m3u8          -> newest segment URL (.../sq/<N>/...)
 *
 * Segments are then addressed directly by rewriting the sq number.
 */
#ifndef CR_YT_H
#define CR_YT_H

#include <stddef.h>

#define CR_URL_MAX 4096

typedef struct {
    char video_id[32];
    char media_url[CR_URL_MAX];   /* media playlist, for cheap re-syncs */
    char seg_url[CR_URL_MAX];     /* one segment URL used as a template */
    long long head_sq;            /* newest segment number seen */
    long long expire;             /* unix time the signed URLs expire, 0 if unknown */
} stream_info;

/* Full resolution. video_id may be NULL/"" to follow clau.de/radio.
 * itag: 234 (AAC-LC ~128k) or 233 (HE-AAC ~48k).
 * Returns 0 on success, otherwise -1 with a message in yt_last_error(). */
int yt_resolve(const char *video_id, int itag, stream_info *si);

/* Re-read the media playlist to find the live edge again. */
int yt_refresh(stream_info *si);

/* Write the URL of segment sq into out. */
int yt_segment_url(const stream_info *si, long long sq, char *out, size_t outsz);

const char *yt_last_error(void);

#endif
