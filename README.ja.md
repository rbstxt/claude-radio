[English](README.md) | 日本語

# claude-radio

Anthropic の 24/7 ローファイ音楽ライブ配信 **Claude FM**（[clau.de/radio](https://clau.de/radio) — Claude Code の `/radio` コマンドで開く配信と同じもの）をターミナルから聴くための軽量プレイヤーです。

ブラウザと同等の音質を保ちつつ、macOS でのメモリ消費を実用上ぎりぎりまで抑える（約 4.5 MB）ことを目的に C で書かれています。

> 非公式プロジェクトです。Anthropic および YouTube / Google とは一切関係なく、公式の承認を受けたものでもありません。

## 動作要件

- **macOS**（CoreAudio HAL を直接使用）。Apple Silicon / macOS 27.2 でビルドおよび動作確認済み。
- **Xcode Command Line Tools**（`cc`, `make`, `git`）。
- **faad2**（Homebrew で導入: `brew install faad2`）。Homebrew のプレフィックスから動的リンクされます。後から `faad2` をアンインストールすると `claude-radio` は動作しなくなります。
- **BearSSL** は git サブモジュール（`vendor/BearSSL`）として同梱されており、`make` 実行時に自動でビルドされます。

## インストール

```sh
brew install faad2
git clone --recursive https://github.com/rbstxt/claude-radio.git
cd claude-radio
make
sudo make install        # /usr/local/bin/claude-radio にインストール
```

- `--recursive` を付けずにクローンした場合でも、`make` が自動的に BearSSL に対して `git submodule update --init` を実行します。
- インストール先を変更する場合: `make install PREFIX=$HOME/.local`（`$HOME/.local/bin` に `PATH` が通っていることを確認してください）。
- アンインストール: `sudo make uninstall`（カスタムの `PREFIX` を使った場合は同じ `PREFIX` を指定してください）。
- インストールせず、ビルドしたディレクトリから `./claude-radio` を直接実行することもできます。

## 使い方

再生を開始します:

```sh
claude-radio
```

再生中は 1 行のステータス表示に、`LIVE` インジケーター、状態（`connecting` / `buffering` / `playing` / `paused` / `reconnecting`）、音量、バッファ残量（秒）、再生時間、音質が表示されます。音途切れが発生した場合はドロップアウト回数も表示されます。

### キー操作

| キー | 動作 |
| --- | --- |
| `space` | 一時停止 / 再開（再開時はライブの最新位置に戻ります） |
| `+` / `-` | 音量を 5% 刻みで調整 |
| `m` | ミュート切り替え |
| `q` | 終了（`Ctrl-C` でも終了します） |

音量カーブは `gain = (percent / 100)²` です。`100%` のときは完全にゲイン `1.0`（無加工）で出力されます。

### オプション

| オプション | 説明 |
| --- | --- |
| `-q, --quality high\|low` | `high`: AAC-LC 約 128 kbps（デフォルト、ブラウザと同等） / `low`: HE-AAC 約 48 kbps |
| `-V, --volume N` | 起動時の音量 `0`〜`100`（デフォルト `100`） |
| `--video ID` | `clau.de/radio` を辿らず、指定した YouTube ライブ動画 ID を再生 |
| `--no-ui` | ステータス行とキー入力を無効化し、`stderr` にログ出力（バックグラウンド実行向け） |
| `--decode FILE` | デバッグ用: ADTS AAC ファイル（`-` で標準入力）を生の float32 ステレオ PCM として `stdout` へデコード |
| `-h, --help` | ヘルプを表示 |
| `-v, --version` | バージョン（`0.1.0`）を表示 |

<details>
<summary><code>claude-radio --help</code> の出力</summary>

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

## メモリ使用量と音質

Apple Silicon（macOS 27.2）上で実際に Claude FM を再生しながら、`footprint`（アクティビティモニタの「メモリ」欄と同じ指標）で計測した結果です。

| 構成 | メモリ |
| --- | --- |
| `mpv --no-video`（音声のみ）+ `yt-dlp` | 70 MB（加えて起動時に `yt-dlp` が約 3 秒間ピーク 85 MB） |
| `claude-radio` | **約 4.5 MB**、CPU 1% 未満 |
| 下限参考: CoreAudio HAL で無音を出力するだけの C プログラム | 3.7 MB |
| 下限参考: 何もしない空の C プロセス | 1.0 MB |

`claude-radio` の約 4.5 MB のうち約 3.7 MB は、macOS 上でプロセスを起動して CoreAudio に接続するだけで避けられない下限コストです。プレイヤー自身のコード、各種バッファ、デコーダが占めるメモリは 1 MB 未満に収まっています（6 分間の連続再生テストで `phys_footprint` 4,481 KB / ピーク 4,577 KB、ドロップアウト 0 回、デコードエラー 0 回）。

**音質について:** YouTube の音声専用 HLS ストリーム `itag 234`（AAC-LC、約 128 kbps、44.1 kHz ステレオ）を再生します。このライブ配信で YouTube が提供する最高音質であり、ブラウザと同じエンコードです。`faad2` の float 出力（FFmpeg の float AAC デコーダとの差分は −87 dB と可聴域をはるかに下回る水準）をそのまま CoreAudio HAL へ渡します。出力デバイスが 44.1 kHz であれば無変換で通し、異なるサンプルレートの場合は内蔵の窓付き sinc リサンプラー（THD+N ≤ −88 dB）で変換します。

## 仕組みの概要

1. **ストリーム解決:** `https://clau.de/radio` のリダイレクト先から YouTube ライブ動画 ID を取得し、`sw.js_data` から visitor ID を取得、YouTube の innertube `/youtubei/v1/player` を `VISIONOS` クライアントとして呼び出します（JS プレイヤー・署名解読・PO トークン不要）。約 50 KB の JSON を固定バッファでストリーム走査して `hlsManifestUrl` だけを抜き出し、マスター / 音声専用 `.m3u8` からセグメント URL を 1 つだけ保持します。
2. **セグメント取得:** 2 秒ごとのセグメントは先頭に ID3 タグが付いた生の ADTS AAC なので、MPEG-TS デマルチプレクサは不要です。プレイリストをポーリングする代わりに、URL 内の `/sq/<N>/` をインクリメントして 1 本の keep-alive BearSSL TLS 1.2 接続で連続取得します。
3. **デコードと再生:** ネットワークスレッドがロックフリーな 128 KB 圧縮ストリームバッファ（約 8 秒分）へ書き込み、CoreAudio のリアルタイム HAL `IOProc` がそこから直接 ADTS フレームを引き出して ID3 をスキップしつつ `faad2` でデコード（必要時はリサンプル）し、そのまま出力デバイスへ送出します。独立したデコードスレッドも PCM リングも持たないため、通常スレッドのスケジュールが遅れても音途切れしにくい構成です。
4. **安定動作:** ライブ先端の 2 セグメント後ろから再生を始め、ドロップアウト時は約 1 秒分の再バッファリングを行ってノイズを出さずに復帰します。約 6 時間で失効する署名付き URL の事前再解決も継ぎ目なく行い、ヘッドホン接続などデフォルト出力デバイスやサンプルレートの変更にも自動で追従します。

パイプラインの詳細、メモリ内訳、計測データ、設計上のトレードオフについては [docs/design.md](docs/design.md)（英語）を参照してください。

## 制限事項・トラブルシューティング

- **macOS 専用:** CoreAudio HAL を直接使用しており、Apple Silicon（macOS 27.2）でのみビルド・テストしています。出力デバイスは float32 に対応している必要があります（実質ほとんどの macOS オーディオデバイスが対応していますが、すべての機器での動作を保証するものではありません）。
- **YouTube API の仕様変更:** YouTube 側の変更で突然再生できなくなる可能性があります。innertube のクライアント名やバージョンは `yt-dlp`（`yt_dlp/extractor/youtube/_base.py`）に合わせているため、壊れた場合は `src/yt.c` の該当定数を更新するのが一般的な対処になります。
- **`YouTube refused the stream (LOGIN_REQUIRED: Sign in to confirm you're not a bot)`:** YouTube の bot チェックによる拒否です。ネットワーク環境や IP によっては発生することがあり、プレイヤーはバックオフ付きで自動リトライします。
- **`certificate not trusted`:** メモリ削減のため macOS のキーチェーンは読み込まず、Google Trust Services（GTS Root R1〜R4、2036 年失効）と GlobalSign Root CA（2028-01-28 失効）のみを埋め込んでいます。Google がルート CA を変更した場合は、`certs/` の PEM ファイルを更新して `make bearssl-tools && ./tools/gen-trust-anchors.sh` で `src/trust_anchors.h` を再生成してください。
- **`is Claude FM on air?`:** 配信がオフラインになっているか、動画 ID が変わった可能性があります。別のライブ動画 ID を直接指定したい場合は `--video ID` を使ってください。

## ライセンス・クレジット・免責事項

- **claude-radio:** [MIT License](LICENSE)（© 2026 rbstxt）
- **BearSSL**（`vendor/BearSSL`）: MIT License（Thomas Pornin）
- **faad2:** GPL-2.0-or-later（Homebrew から動的リンク）。`claude-radio` 本体のソースコードは MIT ライセンスですが、`faad2` とリンクしてビルドしたバイナリを再配布する場合は GPL の条件に従う必要があります。
- **免責事項:** 本プロジェクトは非公式であり、Anthropic および YouTube / Google とは無関係です。Claude FM で流れる楽曲の権利は各権利者に帰属し、`claude-radio` は公開されているストリームを再生するだけです。非公式クライアントから YouTube へアクセスする行為は YouTube の利用規約に抵触する可能性があります。個人利用の範囲で、自己責任にてご利用ください。
