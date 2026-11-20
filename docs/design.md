# Design & Implementation

`claude-radio` is a single-purpose macOS terminal player for Claude FM (`https://clau.de/radio`). Its design goal is simple: **deliver the same audio quality as playing Claude FM in a web browser, while staying as close as possible to the minimum memory footprint a CoreAudio process can achieve on macOS.**

This document explains how the streaming pipeline works, where every megabyte of memory goes, how audio quality was verified, and the trade-offs behind each choice.

---

## Architecture & Pipeline

```text
  https://clau.de/radio ──► YouTube live video ID
                                   │
                    sw.js_data ──► visitorData
                                   │
       /youtubei/v1/player ──► hlsManifestUrl (VISIONOS client, streamed scan)
                                   │
        master .m3u8 ──► audio media .m3u8 ──► segment URL template (.../sq/<N>/...)
                                                          │
  ┌───────────────────────────────────────────────────────┘
  ▼
[ net thread ]            HTTPS/1.1 keep-alive (BearSSL TLS 1.2)
  │                       fetches /sq/<N>/, /sq/<N+1>/, ... directly (long-polled by server)
  │
  ▼  128 KB lock-free SPSC stream buffer (sbuf, ~8 s @ 128 kbps)
  │
[ CoreAudio HAL IOProc ]  pull ADTS from sbuf (16 KB scratch) + ID3 skip
  │                       ──► faad2 (FAAD_FMT_FLOAT)
  │                       ──► [windowed-sinc resampler if stream rate != device rate]
  │                       ──► gain + preferred stereo channel map / mono downmix
  ▼
Output device

[ main thread ]           250 ms poll() loop: raw-TTY keys, status line / --no-ui logs,
                          default-device & sample-rate change listener
```

### 1. Stream resolution (`src/yt.c`)

Resolution runs at startup and again proactively 5 minutes (`300 s`) before the signed segment URLs expire (roughly every 6 hours), or whenever a segment request returns `401`/`403`/`410` or fails repeatedly.

1. **Redirect lookup:** `GET https://clau.de/radio` returns a `3xx` redirect to a YouTube URL (`watch?v=<id>`, `/live/<id>`, or `youtu.be/<id>`). If `clau.de` is unreachable, `claude-radio` falls back to the last known stream ID. Passing `--video ID` skips this step.
2. **Visitor ID:** `GET https://www.youtube.com/sw.js_data` (~2.6 KB) is scanned as a stream for the first `"Cg..."` string, which provides `visitorData`.
3. **Player API:** `POST https://www.youtube.com/youtubei/v1/player?prettyPrint=false` identifies as the `VISIONOS` innertube client (mirroring `yt-dlp`'s `visionos` client in `yt_dlp/extractor/youtube/_base.py`). This client receives an HLS manifest without requiring a JavaScript player, signature deciphering, or a PO token.
4. **Zero-allocation JSON extraction:** Rather than buffering the ~50 KB player JSON or pulling in a JSON parser, `src/yt.c` runs three tiny state-machine byte scanners over 2 KB read chunks to capture only `"hlsManifestUrl"`, `"status"`, and `"reason"` (handling JSON escapes such as `\/`, `\"`, and ASCII `\uXXXX`).
5. **HLS playlists:** The master `.m3u8` is read line by line to find the `#EXT-X-MEDIA` entry with `TYPE=AUDIO` and `GROUP-ID="234"` (or `"233"` for `--quality low`). The media `.m3u8` is scanned line by line, keeping only the newest segment URL containing `/sq/` along with its sequence number (`head_sq`) and `/expire/` Unix timestamp.

When re-resolving for the same video ID, the net thread keeps its current sequence number `sq` as long as it is still within `MAX_BEHIND` (30 segments) of the new `head_sq`. This preserves continuity across the ~6-hour URL refresh without gaps or duplicate audio.

### 2. Segment streaming (`src/net.c`, `src/main.c`)

- **No playlist polling:** Starting `START_BEHIND` (2) segments behind `head_sq` gives a few seconds of initial cushion. From there, `yt_segment_url()` rewrites the `/sq/<N>/` path component in place to ask for `N+1`. YouTube's edge servers long-poll requests for segments that have not been published yet, so sequential requests naturally pace themselves to real time.
- **No MPEG-TS demuxing:** Although segment URLs end in `seg.ts`, the audio-only HLS renditions are raw ADTS AAC frames preceded by an ID3 metadata tag. `dec_next()` parses the ID3 syncsafe size header, skips it, and syncs directly on ADTS headers (`0xFFF0` / layer 0).
- **Single keep-alive TLS connection:** YouTube refuses plain HTTP for these segments, so TLS is required. `src/net.c` implements a minimal HTTP/1.1 client over BearSSL (TLS 1.2), keeping one socket open per host and reusing the cached TLS session if the connection has to be reopened to the same host.
- **Recovery:** If a segment returns `404` or `204` and the server's `X-Head-Seqnum` header indicates the player fell more than 30 segments behind, `sq` jumps back to `head_sq - 2`. After 15 consecutive misses, or on `401`/`403`/`410` or 3 consecutive transport errors, the player re-resolves.

### 3. Threading & buffering (`src/main.c`, `src/sbuf.c`, `src/decode.c`, `src/audio.c`)

- **Net thread (256 KB stack):** Streams HTTPS response bodies in 4 KB chunks into the 128 KB lock-free single-producer/single-consumer byte ring (`sbuf` in `src/sbuf.c`, ~8 seconds at 128 kbps). When `sbuf` is full, the net thread sleeps for roughly the time required to drain the pending chunk (clamped to 50–1,000 ms).
- **CoreAudio HAL `IOProc` (real-time decoding, no decode thread or PCM ring):** Runs directly on CoreAudio's real-time audio thread. It pulls compressed AAC bytes from `sbuf` via `sbuf_read()`, runs `dec_next()` (`faad2` configured with `FAAD_FMT_FLOAT` and `downMatrix = 1`), optionally runs the resampler when the stream rate differs from the device rate, applies volume gain, and maps L/R onto the device's preferred stereo channel pair. Because decoding runs inside the real-time callback rather than an ordinary OS thread, audio continues smoothly as long as compressed data sits in `sbuf` (~8 s), even if macOS delays or throttles normal threads. (Eliminating the decode thread and PCM ring also reduced 6-minute soak-test dropouts from 3–10 down to **0**.)
- **Priming & underrun recovery:** Before starting playback (or after a flush/underrun), `IOProc` waits until `PRIME_BYTES` (16 KB, ~1 s of compressed audio across `sbuf` and the decoder's input buffer) is available. If `dec_next()` runs dry, `IOProc` increments the dropout counter, clears `primed`, and outputs silence until 16 KB re-accumulates — re-buffering cleanly instead of crackling.
- **Lock-free flush protocol (pause / resume to live edge):** When resuming from pause, the main thread increments `flush_req` via `audio_flush_request()` and calls `audio_start()`. The net thread notices `audio_flush_pending()`, aborts any in-flight HTTP body, acknowledges with `audio_flush_ack(gen, sbuf_wpos())`, and calls `yt_refresh()` to jump back to `head_sq - 2`. Meanwhile `IOProc` outputs silence until `flush_ack == flush_req`, then calls `sbuf_skip_to(flush_pos)` and `dec_reset()` before priming on fresh live audio.
- **Main thread:** Polls `stdin` every 250 ms for keyboard input, watches an atomic flag set by CoreAudio property listeners (`kAudioHardwarePropertyDefaultOutputDevice` and `kAudioDevicePropertyNominalSampleRate`) to reopen seamlessly when headphones are plugged in or the device rate changes (also pre-initializing the resampler in `audio_open()` off the real-time thread when the device rate differs from 44.1 kHz), and updates the status line (or logs state transitions to `stderr` with `--no-ui`).

---

## Memory Budget & Measurements

All figures below were measured on Apple Silicon (macOS 27.2) with `footprint` — the same physical memory metric Activity Monitor displays — while actively streaming Claude FM:

| Setup | Memory (`footprint`) | Notes |
| --- | --- | --- |
| `mpv --no-video` + `yt-dlp` | 70 MB | Plus `yt-dlp` peaking at 85 MB for ~3 s at startup |
| Apple AAC via `AudioQueue` | 6.6 MB | Measured for comparison |
| **`claude-radio`** | **~4.5 MB** | **4,481 KB (`4,577 KB` peak), ~0.7% avg CPU (`2.45 s` over 6 min), 0 dropouts, 0 decode errors** |
| `AudioUnit` output alone | 4.2 MB | Output layer without decoding or networking |
| Apple `AudioConverter` decode alone | 3.9 MB | Decoder alone |
| CoreAudio HAL silence floor | 3.7 MB | Minimal C program outputting silence via HAL |
| Empty C process floor | 1.0 MB | Process doing nothing |

### Where `claude-radio`'s ~4.5 MB goes

Subtracting the 3.7 MB CoreAudio HAL floor leaves **under 1 MB** for everything `claude-radio` actually does:

| Component | Size | Source |
| --- | --- | --- |
| Lock-free compressed stream buffer (`data`) | 128 KB | `src/sbuf.c` (~8 s of 128 kbps AAC) |
| `IOProc` stereo render scratch (`mix`) | 64 KB | `src/audio.c` (8,192 frames max; only pages matching the device's IO buffer size are touched in RAM) |
| Resampler state + polyphase table (`rs`) & output (`rs_out`) | ~43 KB | `src/audio.c`, `src/resample.h` (untouched in RAM when device runs at 44.1 kHz) |
| BearSSL bidirectional TLS record buffer (`iobuf`) | ~33 KB | `src/net.c` (`BR_SSL_BUFSIZE_BIDI`) |
| ADTS input scratch (`in`) + decoded stereo frame (`stereo`) | 32 KB | `src/decode.c` (16 KB + 16 KB) |
| Other static state (TLS/X.509 contexts, URL, line, and status buffers) | ~30 KB | `src/net.c`, `src/yt.c`, `src/main.c` |
| **Total static buffers (`__DATA.__bss`)** | **~0.33 MB** | Fixed-size static allocations (unused scratch pages stay unfaulted) |
| `faad2` heap allocations | **~0.27 MB** | Runtime decoder state |
| Net thread stack (touched pages) | remainder | 256 KB stack limit (`start_thread()` in `src/main.c`) |

---

## Audio Quality

- **Source stream:** YouTube's audio-only HLS rendition `itag 234` is AAC-LC at ~128 kbps, 44.1 kHz stereo. That is the highest audio quality YouTube provides for this live stream (YouTube live streams do not offer Opus) and is the exact same 128 kbps AAC stream played by the browser. `--quality low` selects `itag 233` (HE-AAC ~48 kbps). Lo-fi music contains almost no spectral energy above 16 kHz in either rendition, though `high` is recommended.
- **Decoder precision:** `faad2` is configured with `FAAD_FMT_FLOAT` so decoded samples stay in 32-bit floating point end to end. Comparing `claude-radio --decode` against FFmpeg's float AAC decoder (the decoder Chrome uses) on the same 16 seconds of Claude FM shows a difference of **−87 dB relative to the signal (≈ −121 dBFS)** — far below audibility. For comparison, Apple's AudioToolbox AAC decoder differed from FFmpeg by **−67 dB** (around 16-bit rounding level).
- **Direct output & resampling:** Samples are delivered as float32 straight to the CoreAudio HAL. When the output device runs at 44.1 kHz (typical), samples bypass the resampler untouched. When the device runs at another rate (for example, 48 kHz), `src/resample.c` converts using a 32-tap, 128-phase polyphase windowed-sinc filter with a Kaiser window (`β = 8.6`, cutoff at `0.97` of the lower Nyquist, linear interpolation between adjacent phases). At 44.1 → 48 kHz, measured **THD+N is ≤ −88 dB** across 1–18 kHz tones with flat passband gain (**±0.01 dB**).
- **Gain & channel mapping:** Volume uses `gain = (percent / 100)²`; at 100%, `gain` is exact `1.0f`. Stereo output is routed to the device's `kAudioDevicePropertyPreferredChannelsForStereo` channel pair, and mono devices receive an `(L + R) * 0.5f` downmix.

---

## TLS & Trust Anchors

Loading macOS's `Security.framework` or system keychain pulls additional frameworks and memory into the process. Instead, `claude-radio` statically links **BearSSL** (`vendor/BearSSL`) and embeds only the root CAs needed to reach `clau.de` and Google/YouTube servers:

- Google Trust Services: `GTS Root R1`, `GTS Root R2`, `GTS Root R3`, `GTS Root R4` (expire in 2036)
- `GlobalSign Root CA` (expires 2028-01-28)

The PEM certificates live in `certs/` and are compiled into `src/trust_anchors.h` via BearSSL's `brssl` tool:

```sh
make bearssl-tools
./tools/gen-trust-anchors.sh
```

---

## Key Trade-offs

| Decision | Benefit | Cost |
| --- | --- | --- |
| **Raw CoreAudio HAL** instead of `AudioUnit` / `AudioQueue` | Avoids `AudioUnit`'s 4.2 MB output floor (vs 3.7 MB for raw HAL) and `AudioQueue`'s 6.6 MB total with AAC | macOS only; requires float32 output devices, explicit channel mapping, and a built-in resampler |
| **Decoding inside `IOProc`** instead of a decode thread + PCM ring | Eliminates one thread stack and the PCM ring, and cuts 6-minute soak dropouts from 3–10 to 0 because decoding runs on CoreAudio's real-time thread | Real-time callback must stay lock-free and avoid heavy setup (`faad2` is pre-opened and the resampler is pre-initialized in `audio_open()`) |
| **Dynamic `faad2`** instead of Apple `AudioConverter` / `AudioToolbox` | Avoids `AudioConverter`'s 3.9 MB decoder-alone footprint and AudioToolbox's −67 dB rounding difference; matches FFmpeg within −87 dB | Requires Homebrew `faad2` at runtime; `faad2` is GPL-2.0-or-later, so redistributed binaries fall under the GPL |
| **BearSSL + 5 embedded roots** instead of `Security.framework` | Keeps TLS in static buffers (~33 KB record buffer) with no keychain/framework overhead | Only trusts the embedded roots; requires regenerating `src/trust_anchors.h` if Google rotates CAs or before root expiry |
| **Streaming substring scanner** instead of a JSON parser | Zero heap allocation for the ~50 KB player payload; ignores unrelated schema churn | Assumes YouTube keeps the `"hlsManifestUrl"`, `"status"`, and `"reason"` field names |
| **Direct `/sq/<N>/` stepping** instead of `.m3u8` polling | No periodic playlist downloads (~18 KB each) and no playlist-refresh latency | Relies on YouTube's segment server long-polling unpublished sequence numbers |
