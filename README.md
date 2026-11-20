English | [日本語](README.ja.md)

# claude-radio

A lightweight terminal player for **Claude FM** — Anthropic's 24/7 lo-fi music live stream on YouTube ([clau.de/radio](https://clau.de/radio), the same stream opened by Claude Code's `/radio` command).

Built in C for macOS to keep memory use as low as practically possible (~4.5 MB) while preserving the same audio quality as the browser.

> Unofficial project. Not affiliated with or endorsed by Anthropic or YouTube/Google.

## Requirements

- **macOS** (uses the CoreAudio HAL). Built and tested on Apple Silicon, macOS 27.2.
- **Xcode Command Line Tools** (`cc`, `make`, `git`).
- **faad2** via Homebrew (`brew install faad2`), dynamically linked from Homebrew's prefix. If `faad2` is later uninstalled, `claude-radio` will stop working until it is reinstalled.
- **BearSSL** is vendored as a git submodule (`vendor/BearSSL`) and built automatically by `make`.

## Install

```sh
brew install faad2
git clone --recursive https://github.com/rbstxt/claude-radio.git
cd claude-radio
make
sudo make install        # installs /usr/local/bin/claude-radio
```

- If you cloned without `--recursive`, `make` runs `git submodule update --init` for BearSSL automatically.
- To install to a custom prefix: `make install PREFIX=$HOME/.local` (make sure `$HOME/.local/bin` is on your `PATH`).
- To uninstall: `sudo make uninstall` (pass the same `PREFIX` if you used a custom one).
- You can also run `./claude-radio` directly from the build directory without installing.

## Usage

Start listening:

```sh
claude-radio
```

While running, a single status line shows the `LIVE` indicator, playback state (`connecting`, `buffering`, `playing`, `paused`, or `reconnecting`), volume, buffered seconds, listening time, audio quality, and a dropout counter if any dropouts occurred.

### Keys

| Key | Action |
| --- | --- |
| `space` | Pause / resume (resuming jumps back to the live edge) |
| `+` / `-` | Adjust volume in 5% steps |
| `m` | Mute / unmute |
| `q` | Quit (`Ctrl-C` also quits) |

Volume follows a quadratic curve: `gain = (percent / 100)²`. At `100%`, gain is exact unity (`1.0`) with no attenuation.

### Options

| Option | Description |
| --- | --- |
| `-q, --quality high\|low` | `high`: AAC-LC ~128 kbps (default, same as the browser); `low`: HE-AAC ~48 kbps |
| `-V, --volume N` | Starting volume `0`–`100` (default `100`) |
| `--video ID` | Play a specific YouTube live video ID instead of following `clau.de/radio` |
| `--no-ui` | Disable the status line and key handling; log state changes to `stderr` (good for background use) |
| `--decode FILE` | Debug: decode an ADTS AAC file (or `-` for `stdin`) to raw float32 stereo on `stdout` |
| `-h, --help` | Show help text |
| `-v, --version` | Show version (`0.1.0`) |

<details>
<summary>Exact <code>claude-radio --help</code> output</summary>

```text
usage: claude-radio [options]

Play the Claude FM live stream (https://clau.de/radio) in the terminal.

options:
  -q, --quality high|low  high: AAC-LC ~128 kbps (default, same as the browser)
                          low:  HE-AAC ~48 kbps (less bandwidth)
  -V, --volume N          start volume 0-100 (default 100)
      --video ID          play this YouTube live video instead of clau.de/radio
      --no-ui             no status line or keys; log to stderr
      --decode FILE       decode an ADTS file to raw float32 stereo on stdout
  -h, --help              show this help
  -v, --version           show the version

keys: space pause/resume   + / - volume   m mute   q quit
```

</details>

## Memory & audio quality

Measured on Apple Silicon (macOS 27.2) with `footprint` — the same "Memory" metric Activity Monitor reports — while streaming Claude FM:

| Setup | Memory |
| --- | --- |
| `mpv --no-video` (audio only) + `yt-dlp` | 70 MB (plus `yt-dlp` peaking at 85 MB for ~3 s at startup) |
| `claude-radio` | **~4.5 MB**, < 1% CPU |
| Floor: C program outputting silence via CoreAudio HAL | 3.7 MB |
| Floor: empty C process | 1.0 MB |

About 3.7 MB of `claude-radio`'s 4.5 MB is the baseline cost of running a macOS process attached to CoreAudio; the player's own code, buffers, and AAC decoder account for under 1 MB (4,481 KB `phys_footprint` / 4,577 KB peak over a 6-minute playback run with 0 dropouts and 0 decode errors).

**Audio quality:** `claude-radio` plays YouTube's audio-only HLS rendition `itag 234` (AAC-LC, ~128 kbps, 44.1 kHz stereo) — the highest quality audio YouTube serves for this live stream and the same encode the browser plays. It decodes to float32 via `faad2` (−87 dB difference vs FFmpeg's float AAC decoder, far below audibility) and writes float32 straight to the CoreAudio HAL, passing samples untouched when the output device runs at 44.1 kHz and using a built-in windowed-sinc resampler (THD+N ≤ −88 dB) otherwise.

## How it works

1. **Resolve:** Follows `https://clau.de/radio` to the current YouTube live video ID, fetches a visitor ID from `sw.js_data`, and calls YouTube's innertube `/youtubei/v1/player` endpoint as the `VISIONOS` client (no JS player, no signature deciphering, no PO token). Only `hlsManifestUrl` is scanned out of the streaming JSON response into a small fixed buffer, then the master and audio-only media `.m3u8` playlists yield a single segment URL.
2. **Stream:** Fetches 2-second raw ADTS AAC segments (with a leading ID3 tag, no MPEG-TS demuxing needed) by incrementing `/sq/<N>/` directly over one keep-alive BearSSL TLS 1.2 connection — no playlist polling.
3. **Decode & play:** A network thread fills a lock-free 128 KB compressed stream buffer (~8 s). CoreAudio's real-time HAL `IOProc` pulls ADTS frames directly from that buffer, skips ID3 tags, decodes with `faad2` (and resamples if needed), and writes straight to the output device — no separate decode thread and no PCM ring, so playback stays glitch-free even when ordinary threads are delayed.
4. **Stay alive:** Starts 2 segments behind the live edge, re-buffers cleanly (~1 s prime) on dropouts, re-resolves smoothly before the ~6-hour signed URLs expire, and automatically follows changes to the default output device and sample rate (such as plugging in headphones).

For the full pipeline, memory breakdown, measurements, and trade-offs, see [docs/design.md](docs/design.md).

## Limitations & troubleshooting

- **macOS only:** Uses CoreAudio HAL directly and has been built and tested on Apple Silicon (macOS 27.2). Output devices must accept float32 samples (virtually all macOS devices do), though not every hardware/virtual device has been tested.
- **YouTube API changes:** YouTube can change its innertube API at any time. The client name and version mirror `yt-dlp` (`yt_dlp/extractor/youtube/_base.py`); if playback stops working, updating those constants in `src/yt.c` is usually the fix.
- **`YouTube refused the stream (LOGIN_REQUIRED: Sign in to confirm you're not a bot)`:** YouTube's bot check, which can happen on some networks or IP ranges. The player retries automatically with backoff.
- **`certificate not trusted`:** `claude-radio` embeds only Google Trust Services roots (GTS Root R1–R4, expiring 2036) and GlobalSign Root CA (expiring 2028-01-28) instead of loading macOS's keychain. If Google changes its CA chain, update the PEM files in `certs/` and regenerate `src/trust_anchors.h` with `make bearssl-tools && ./tools/gen-trust-anchors.sh`.
- **`is Claude FM on air?`:** The live stream may be offline or the video ID may have changed. You can pass `--video ID` to play a specific YouTube live video ID directly.

## License, credits & disclaimer

- **claude-radio:** [MIT License](LICENSE), © 2026 rbstxt.
- **BearSSL** (`vendor/BearSSL`): MIT License, by Thomas Pornin.
- **faad2:** GPL-2.0-or-later, dynamically linked from Homebrew. While `claude-radio`'s source code is MIT-licensed, compiled binaries linked against `faad2` that you build and redistribute are subject to the GPL as well.
- **Disclaimer:** This is an unofficial project and is not affiliated with or endorsed by Anthropic or YouTube/Google. Claude FM's music belongs to its respective rights holders; `claude-radio` only plays the public stream. Accessing YouTube with a non-official client may conflict with YouTube's Terms of Service — use for personal listening at your own risk.
