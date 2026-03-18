# 双方向会話（Two-Way Audio）実装計画

## 概要
AtomCamの公式アプリでサポートされている双方向会話機能を、atomcam_toolsのWebRTC/Web UI経由で実現する。

## 現状整理

### 実装済み
- `astream`コマンドによるFIFOベースのストリーミング再生（`local_sdk_speaker_feed_pcm_data()`）
- a-lawデコード内蔵（go2rtcのPCMAをlibcallback内でs16leに変換）
- マイクキャプチャ（`audio_callback.c`, ALSA loopback経由）
- エコーキャンセル（IMP SDK `IMP_AI_EnableAec()`）
- WebRTC双方向配信（go2rtc v1.9.2 + backchannel）
- 音声処理（ノイズ抑制、AGC等）
- webrtc.htmlでのマイク送信（`getUserMedia()` + sendonly transceiver）
- Web UI統合（Setting.vueにマイク制御UI追加）

### 未実装
- 音質改善
- go2rtcバッファパッチの効果検証（現在upstreamバッファサイズに戻して検証中）

## アーキテクチャ（実装済み）
```
ブラウザMIC → WebRTC(sendrecv) → go2rtc → exec:backchannel.sh(PCMA raw) → FIFO → astream(a-law decode) → スピーカー
ATOMマイク → v4l2rtspserver(OPUS) → go2rtc → WebRTC → ブラウザスピーカー
```

## フェーズ分け

### フェーズ1: ストリーミング再生の基盤確立 — 完了 (2026-03-15)
**目標**: PCからATOMのスピーカーにリアルタイムで音声をストリーミング再生できることを検証

#### 1-1. `astream`コマンドの実装 — 完了
- 新規ファイル: `libcallback/audio_stream.c`
- FIFOからPCM(8kHz/16bit/mono)を読み続けてスピーカーに出力
- コマンド: `astream <fifo_path> [volume] [alaw]` / `astream stop` / `astream`(状態確認)
- `command.c`のCommandTableに登録

#### 1-2. 動作検証 — 完了
```bash
# ATOM上でFIFO作成＋astream開始
ssh atomcam 'mkfifo /tmp/audio_in.fifo; echo "astream /tmp/audio_in.fifo 40" | nc localhost 4000'

# 固定音（440Hzサイン波5秒）
ffmpeg -f lavfi -i "sine=frequency=440:duration=5" -f s16le -ar 8000 -ac 1 - 2>/dev/null | ssh atomcam 'cat > /tmp/audio_in.fifo'

# MP3ファイルをストリーミング再生
ffmpeg -i file.mp3 -f s16le -ar 8000 -ac 1 - 2>/dev/null | ssh atomcam 'cat > /tmp/audio_in.fifo'

# 停止
ssh atomcam 'echo "astream stop" | nc localhost 4000'
```

#### 1-3. 確認結果
- [x] FIFOからの連続読み込みでスピーカー出力が途切れないか → OK
- [x] `local_sdk_speaker_feed_pcm_data()`のストリーミング呼び出しが安定するか → OK
- [x] サイン波(440Hz)の再生 → OK
- [x] MP3ファイルのストリーミング再生 → OK
- [x] 停止コマンド → OK

### フェーズ1+: 双方向通話の基本検証 — 完了 (2026-03-15)
**目標**: PC↔ATOM間で双方向の音声通話が成立するか検証

#### 検証方法
- **ATOM→PC**: ブラウザWebRTC経由でATOMマイク音声を受信（イヤホンで聴取）
- **PC→ATOM**: `ffmpeg + SSH + astream`経由でMP3をATOMスピーカーに送信

#### 確認結果
- [x] 双方向の音声経路が同時に動作する → OK
- [x] エコーキャンセル（`audio aec on` = `IMP_AI_EnableAec()`）が有効に機能 → OK
- [x] 同じ部屋でもAEC有効でエコー軽減を確認 → OK
- 音質は8kHzサンプルレート制約 + SSHバッファリングにより低い → フェーズ2で改善見込み

### フェーズ2: go2rtcバックチャネル連携 — 完了 (2026-03-15)
**目標**: go2rtcのexec:ソース経由でバックチャネル音声をFIFOに流す

#### 実装内容
- `rtspserver.sh`: FIFO準備 + astream alaw自動起動 + go2rtc.yamlにbackchannel設定追加
- `backchannel.sh`: go2rtcのstdinからPCMA rawデータをFIFOにcat（新規作成）
- `audio_stream.c`: a-lawデコード機能追加、`alaw`オプション対応
- ffmpegは不要（libcallback内でa-lawデコード）

#### 確認結果
- [x] go2rtcの`exec:`+`#backchannel=1`がMIPSEL環境で動作するか → OK
- [x] go2rtcがPCMAをstdinに書き込む → OK
- [x] FIFO経由でastream(alawデコード)からスピーカー出力されるか → OK
- [x] 遅延・音質は実用レベルか → 遅延約2秒、音質はまあまあ（改善余地あり）
- [x] WebRTC接続中の安定性 → OK（映像停止・ハングなし）

### フェーズ3: WebRTC双方向化 — 完了 (2026-03-15〜16)
**目標**: ブラウザから双方向音声通話ができるようにする

#### 3-1. webrtc.html変更 — 完了 (2026-03-15)
- `getUserMedia()`でブラウザマイク取得
- sendonly transceiverでマイク音声をgo2rtcに送信

#### 3-2. Web UI統合 — 完了 (2026-03-16)
- Setting.vueに「双方向会話」ON/OFFスイッチ追加（`WEBRTC_TWOWAY`設定、デフォルトOFF）
- ONの場合、WebRTCUrlに`?media=video+audio+microphone`を付与
- WebRTCがOFFまたはRTSP音声がOPUS以外の場合はdisabled
- webrtc.htmlにマイクON/OFFボタン追加（SVGマイクアイコン、デフォルトミュート）
- ボタンクリックで`track.enabled`トグル（re-negotiation不要）
- WebSocket接続先URLのポート処理修正（`location.origin` → `location.hostname`）
- webrtc.htmlのデフォルトmediaを`video`に変更（オリジナル互換）
- i18n日英に双方向会話のラベル・ツールチップ追加
- 設計方針: デフォルトOFF、OFFなら改修前と完全に同じコードパス（upstream PR向け）

### フェーズ4: 遅延の最小化・安定性向上
**目標**: 遅延を約2秒→500ms以下に改善し、接続の安定性を向上させる

**重要**: 必ず1ステップずつ実施→ビルド→デプロイ→検証の順で進めること。複数ステップを同時に変更しない（問題発生時に原因の切り分けができなくなるため）。

**ビルド手順**: 本ドキュメントの「ビルド方法一覧」セクションを参照。
#### 完了したステップ
1. `backchannel.sh`: `cat`→`dd bs=320`でバッファリング除去 — 完了 (2026-03-17)
2. マイクボタンによるastream制御 — 完了 (2026-03-17)
   - `webrtc.html`: マイクON→`astream start`、OFF→`astream stop`をcmd.cgi経由で送信
   - `rtspserver.sh`: astream自動起動を削除（マイクボタンからのオンデマンド起動に変更）
   - `audio_stream.c`: FIFO open時に蓄積データをフラッシュ（O_NONBLOCKで読み捨て）
   - `webrtc.html`: fetchのURLをポート80に明示（go2rtc 1984ポートからのCORS問題回避）
3. ブラウザ側AEC/NS/AGC無効化 — 完了 (2026-03-17)
   - `webrtc.html`: `getUserMedia`に`echoCancellation/noiseSuppression/autoGainControl: false`を指定
   - ATOM側で処理済みのため二重処理を排除（音質改善目的、遅延への効果は軽微）
4. パイプライン遅延計測と初期バースト対策 — 完了 (2026-03-17)
   - `audio_stream.c`: 計測ログ追加（read_avg, feed_avg, feed_retry）→ `/tmp/astream_latency.log`に出力
   - 計測結果: astream側はread_avg=20ms, feed_avg=0.1ms, feed_retry=0で遅延なし
   - ボトルネック: 起動時にgo2rtcからデータが一気に到着しスピーカーバッファが詰まる（初回feed_retry=35/100, 650ms）
   - 対策: 時間ベースのバーストスキップ — read間隔が15ms以上に安定するまでデータを読み捨て
   - 結果: Mac→ATOM遅延が約2秒→約1秒に改善
5. go2rtcバッファ削減パッチ適用 → 除去（効果なし） (2026-03-17〜18)
   - `0005-go2rtc-reduce-audio-buffer.patch`: 音声送信バッファ100→5パケットに削減するパッチを作成
   - `build_squashfs.sh`: go2rtcバイナリのコピーステップを追加
   - **検証結果**: bufferSize=5とbufferSize=100（upstream）を比較。体感・ログとも有意な差なし
   - 1回目セッションの遅延はgo2rtcバッファではなく、backchannel.shがFIFOに常時書き込む問題が原因（別課題）
   - **結論: パッチは不要**。除去してupstream状態に戻した
6. RTP送信制御によるFIFO蓄積防止 — 完了 (2026-03-17)
   - **原因特定**: マイクOFF中もブラウザがsilenceフレームをRTP送信 → go2rtc → backchannel.sh(dd) → FIFOにデータ蓄積（最大1MB≒131秒分）→ マイクON時にastream起動で蓄積データに直面
   - `webrtc.html`: `addTransceiver(micTrack)` → `addTransceiver('audio')` に変更（nullトラックで開始、RTP送信なし）
   - `webrtc.html`: マイクON時に`sender.replaceTrack(micTrack)`、OFF時に`sender.replaceTrack(null)`（WebRTC標準API、re-negotiation不要）
   - 効果: skipped_bytes 122KB→800Bに激減、astream側total_ms 1425ms→51msに改善
7. astream起動順序の最適化 — 完了 (2026-03-17)
   - **原因特定**: マイクONボタンでreplaceTrack→sendAstreamCmd(HTTP POST)の順だと、cmd.cgi経路の遅延（初回約10秒）がボトルネック
   - `webrtc.html`: sendAstreamCmd→replaceTrackの順に変更（astream起動を先行、RTP送信を後で開始）
   - 効果: astreamがFIFO待機中にHTTPの遅延を吸収。体感12秒→1秒以下に改善

#### 試行して取りやめたステップ
- `audio_stream.c`: `feed_pcm_data`リトライ間隔を10ms→2msに短縮 → 効果なし、取りやめ
- `audio_stream.c`: FIFOフラッシュ後500ms時間ベーススキップ → readがブロッキングなので時間ベースが機能しない（read完了時点でskipUntilを過ぎている）、取りやめ
- `audio_stream.c`: バイト数ベーススキップ（8000B→2000B） → go2rtcからのデータ供給がリアルタイムなのでリアルタイムデータまで読み捨ててしまい音が出なくなる、取りやめ
- `audio_stream.c`: スキップ完全除去（FIFOフラッシュのみ） → ホットデプロイが効かず検証未完了

#### 現在の状態 (2026-03-18)
- go2rtcバッファパッチは不要と判断し除去。upstream状態（bufferSize=100）で運用
- **Mac→ATOM音声遅延: 0.5〜1秒**（2回目セッション以降）
- **1回目セッションの遅延問題が未解決**: ATOM起動後の最初のマイクONで大量のstaleデータがスピーカーバッファに流入（feed_retry=100/100）。2回目以降は正常
- 原因: backchannel.sh（dd）がFIFOに常時書き込んでおり、astream未起動中にデータが蓄積される

#### 次に試すべきアプローチ
1. **1回目セッションのFIFOバッファ蓄積問題の解決** — backchannel.shの書き込みタイミング制御、またはastream側のフラッシュ強化

#### 成果
- Mac→ATOM音声遅延: 1秒以下（初回・2回目とも）— 当初約5秒から大幅改善
- ATOM→Mac遅延: 0.5秒以下で良好
- マイクOFF時: astream停止、RTP送信も停止（CPU・帯域節約）
- マイクON時: astream起動→即再生
- マイクボタンによるオンデマンド制御が正常動作

#### 課題
- ATOM起動直後のRTSP接続タイムアウト（運用上許容、起動後安定すれば問題なし）
- **1回目セッションのFIFOバッファ蓄積問題**: backchannel.sh（dd）がastream未起動中もFIFOに書き込み続け、最初のマイクON時にstaleデータが大量にスピーカーに流入する。2回目以降は正常

### フェーズ4+: 音質・その他の品質改善
- エコーキャンセル（AEC）パラメータ調整（`IMP_AI_EnableAec()`は動作確認済み）
- ノイズ抑制の最適化
- 起動時間の最小化
- WebRTCストリームのSub(360p)対応 — CPU負荷軽減のため。go2rtcにvideo2ストリームを追加定義し、Setting.vueでMain/Sub選択UIを追加。実際の負荷差は要検証

## フェーズ5: リファクタリング
- コードのふりかえり

### フェーズ6: 運用構築
- HTTPS問題の解決(tailscaleを活用?)
- 最終的にHome Assistantから使えるように

## 技術メモ

### 重要なファイル
| ファイル | 役割 |
|---------|------|
| `libcallback/audio_stream.c` | FIFOストリーミング再生 + a-lawデコード |
| `libcallback/audio_play.c` | 既存のWAV再生実装。参考実装 |
| `libcallback/command.c` | コマンドインターフェース。astream登録先 |
| `libcallback/audio_callback.c` | マイクキャプチャ。ALSAパターンの参考 |
| `libcallback/audio_control.c` | 音声処理制御（AEC, AGC, NS等） |
| `overlay_rootfs/scripts/backchannel.sh` | go2rtcバックチャネル→FIFO |
| `overlay_rootfs/scripts/rtspserver.sh` | go2rtc設定生成・FIFO準備・起動 |
| `web/source/webrtc.html` | WebRTCクライアント（双方向対応 + マイクボタンUI） |
| `web/source/vue/Setting.vue` | 管理画面（双方向会話スイッチ + WebRTCUrl生成） |
| `web/source/vue/i18n-ja.yaml` | 日本語ローカライズ |
| `web/source/vue/i18n-en.yaml` | 英語ローカライズ |
| `custompackages/package/go2rtc/go2rtc.mk` | go2rtc v1.9.2ビルド設定 |

### スピーカー出力API
```c
extern int local_sdk_speaker_clean_buf_data();
extern int local_sdk_speaker_set_volume(int volume);       // -30〜120
extern int local_sdk_speaker_feed_pcm_data(unsigned char *buf, int size);  // PCM送出
extern int local_sdk_speaker_set_ap_mode(int mode);        // モード設定
extern int local_sdk_speaker_set_pa_mode(int mode);        // PA制御
// set_pa_mode(3) = アクティブ, set_pa_mode(0) = スタンバイ
```

### PCMフォーマット
- サンプルレート: 8000Hz (ATOMCam) / 16000Hz (WyzeCam)
- ビット幅: 16bit signed little-endian
- チャネル: 1 (mono)
- バッファサイズ: 640バイト単位

### go2rtcバックチャネルの仕組み
- go2rtc v1.9.2: `#backchannel=1`付きexec:のstdinにPCMA rawデータを書き込む
- WebRTCクライアントは`media=video+audio+microphone`パラメータでマイク有効化
- HOMEKIT_SOURCEは`rtsp://localhost:8554/video0_unicast`
- lighttpdはポート80、go2rtc APIはポート1984
- `getUserMedia()`はSecure Context必須 → `localhost`（SSHポートフォワード）、`atomcam.local`(mDNS)、またはChromeフラグで対応

## リスク・課題
| 課題 | 深刻度 | 状態 |
|------|--------|------|
| go2rtc exec:+backchannel動作確認 | ~~高~~ | 解決済み |
| Opus→PCM変換の遅延 | ~~中~~ | 解決（ffmpeg不要、libcallback内a-lawデコード） |
| エコーキャンセル | ~~中~~ | 解決（`audio aec on`で動作確認済み） |
| T31 CPU負荷 | 中 | WebRTC接続中load 7前後、切断後3前後に回復。許容範囲 |
| 遅延（約2秒） | 中 | フェーズ4で改善予定 |

## 開発フロー（libcallback.soの差し替え手順）

ATOMのルートファイルシステムはsquashfs（読み取り専用）のため、以下の手順で差し替える。

### ホットデプロイ（SD抜き差し不要）
**注意: libcallback.soのホットデプロイは機能しない。** iCameraはchroot(`/atom`)内から起動されるため、bind mountのパスが食い違う。`/lib/modules/libcallback.so`にbind mountしても、iCameraは`/atom/tmp/system/lib/modules/libcallback.so`（squashfs上、inode番号が異なる）を参照する。**libcallback.soの変更はSDデプロイ（squashfs再構築）が必須。**

スクリプトやHTMLは直接上書き可能:
```bash
scp overlay_rootfs/scripts/backchannel.sh atomcam:/scripts/backchannel.sh
scp web/source/webrtc.html atomcam:/var/www/webrtc.html
```

### SDカードのパーティション構成
upstream release (Ver.2.5.5) のSDカードは2パーティション構成:
- `BOOT` (FAT32): `factory_t31_ZMC6tiIDQN` のみ
- `ATOMTOOLS` (FAT32): `rootfs_hack.squashfs`, `authorized_keys`, `hostname` 等

デプロイ先は `/Volumes/ATOMTOOLS/rootfs_hack.squashfs`。

### SDカードデプロイ時の注意
- コピー後、`diskutil unmount /Volumes/ATOMTOOLS` と `diskutil unmount /Volumes/BOOT` の両方をアンマウント
- `authorized_keys`は`docs/authorized_keys`からコピーすること

### ビルド方法一覧

**重要**: すべてのビルドはDockerコンテナ内で行う。ホスト（Mac）上ではクロスコンパイルできない。

```bash
docker start atomcam_tools-builder-1  # コンテナが停止している場合
```

#### 1. フルビルド（カーネル + rootfs + 全パッケージ）
初回ビルドやパッケージ追加・変更時に使用。成果物は `target/rootfs_hack.squashfs`。
```bash
# ホスト側（Mac）
make              # Docker image pull + フルビルド
make build-local  # Docker image更新なし
```
内部動作: `buildscripts/build_all` → buildroot make → `post_fakeroot.sh`（libcallback.so + web frontend）→ `post_image.sh`（squashfs生成）

#### 2. libcallback.soのみ再ビルド
`libcallback/*.c` を変更した場合。成果物は `libcallback/libcallback.so`（ホスト側に直接コピーされる）。
```bash
docker exec atomcam_tools-builder-1 sh /src/build_libcallback.sh
```
- クロスコンパイラ: `/atomtools/build/cross/mips-uclibc/bin/mipsel-ingenic-linux-uclibc-`（uClibc環境）
- glibc版gcc（`mipsel-ingenic-linux-gnu-`）を使うとヘッダー不一致でエラーになる
- 前提: フルビルドが一度実行済みであること（ビルドディレクトリが存在する必要がある）

#### 3. squashfs簡易再構築（build_squashfs.sh）
libcallback.so、スクリプト、HTML等を変更した場合の高速デプロイ用。成果物は `rootfs_hack_new.squashfs`。
```bash
docker exec atomcam_tools-builder-1 sh /src/build_squashfs.sh
```
前提:
- `rootfs_hack_upstream.squashfs` がプロジェクトルートにあること（upstream releaseからコピーしたオリジナル）
- `libcallback/libcallback.so` がビルド済みであること
- go2rtcバイナリが buildroot output（`/atomtools/build/.../output/target/usr/bin/go2rtc`）にあること
- Webフロントエンド（`web/frontend/`）がビルド済みであること

#### 4. go2rtcの個別リビルド
go2rtcパッチを変更した場合。`build_all`はパッケージの差分を検出して自動で`dirclean`→再ビルドするが、手動で行う場合:
```bash
docker exec -w /atomtools/build/buildroot-2016.02 atomcam_tools-builder-1 make go2rtc-dirclean
docker exec -w /atomtools/build/buildroot-2016.02 atomcam_tools-builder-1 make go2rtc
```
- パッチファイル（`custompackages/package/go2rtc/0001-*.patch`〜）はgo2rtc v1.9.2（commit b2399f3）のソースに合わせること
- 成果物: `/atomtools/build/.../output/target/usr/bin/go2rtc`

#### 5. Webフロントエンドビルド（Setting.vue等を変更した場合）
```bash
cd /Users/sasaki/GitHub/atomcam_tools/web
rm -rf frontend
./node_modules/.bin/webpack --mode production --progress
```

### 典型的なデプロイ手順（libcallback.so変更時）
```bash
docker start atomcam_tools-builder-1
docker exec atomcam_tools-builder-1 sh /src/build_libcallback.sh
docker exec atomcam_tools-builder-1 sh /src/build_squashfs.sh
cp rootfs_hack_new.squashfs /Volumes/ATOMTOOLS/rootfs_hack.squashfs
md5 rootfs_hack_new.squashfs
md5 /Volumes/ATOMTOOLS/rootfs_hack.squashfs
diskutil unmount /Volumes/ATOMTOOLS
diskutil unmount /Volumes/BOOT
```

### 注意事項
- **`build_squashfs.sh`は必ず`rm -rf /tmp/squashfs-root`してからunsquashfsすること**（`-f`で上書き展開すると前回のゴミファイルが残る。これが原因で起動不能になった）
- **`lib32/modules/libcallback.so`にもコピーが必要**。iCameraはchroot内の`lib32`パスを参照する場合がある。`build_squashfs.sh`で`lib/modules`と`lib32/modules`の両方にコピーしている
- **squashfs再構築時は必ず全ファイルをコピーすること**。libcallback.so、スクリプト、**Webフロントエンド（bundle*）**の3種を毎回コピーする。フロントエンドのコピーを忘れるとWebUIが動作しない
- **ベースファイルは`rootfs_hack_upstream.squashfs`を使うこと**。これはupstream releaseからコピーしたオリジナル。`rootfs_hack_new.squashfs`（出力ファイル）をベースにすると、ゴミが蓄積して起動不能になる
- **squashfsの圧縮形式は必ず gzip を使うこと**（`-comp gzip`）。カメラのカーネルがxzをサポートしていないため、xzで構築すると起動しない
- デプロイ前に `rootfs_hack.squashfs` のバックアップを推奨
- SDカードをATOMに戻して電源ONで反映される
- SSH接続には `~/.ssh/config` に以下の設定が必要（OpenSSH 10.x + ATOMのOpenSSH 7.1の互換性問題）:
  ```
  Host atomcam
    PubkeyAcceptedAlgorithms +ssh-rsa
    HostkeyAlgorithms +ssh-rsa
  ```

## 実装ノート

### ffmpeg→FIFOの書き込みブロック問題と解決

初期実装ではbackchannel.shでffmpegを使いPCMA→s16le変換を行う予定だったが、
go2rtcのstdoutキャプチャとFIFOのデッドロック問題により断念。

**解決策**: ffmpegを使わず、libcallback内にa-lawデコードを実装。
backchannel.shは単純な`cat`でPCMA rawをFIFOに流し、astreamのalawオプションでデコード。

### astreamのreopenループ負荷

FIFOの書き手がいない状態でread()→EOF→close→reopen→...のループが
CPU負荷を上げる問題があった。reopenの待機時間を5秒に設定して解決。

### WebRTC接続中のSSH不安定

WebRTC配信中はATOMのCPU/ネットワーク負荷が上がり、SSHがタイムアウトしやすい。
デバッグ時はブラウザのタブを閉じてからSSH操作する。

### squashfsデプロイ時のoverlay反映

squashfs再構築時は `rootfs_hack.squashfs`（元のベース）を展開した後、
`overlay_rootfs/` のファイルを手動でコピーする必要がある。
libcallback.soだけでなくスクリプトやHTMLも忘れずにコピーすること。

## 更新履歴
- 2026-03-15: 初版作成。調査完了、フェーズ1着手開始
- 2026-03-15: フェーズ1完了。astream コマンド実装・検証成功（サイン波、MP3ストリーミング再生確認）
- 2026-03-15: フェーズ1+完了。双方向通話の基本検証成功（PC↔ATOM同時通話、AECによるエコー軽減確認）
- 2026-03-15: フェーズ2+3完了。go2rtcバックチャネル連携 + WebRTC双方向化。ブラウザ↔ATOM双方向音声通話成功（遅延約2秒、安定動作確認）
- 2026-03-16: フェーズ3-2完了。Setting.vueに双方向会話スイッチ追加、webrtc.htmlにマイクON/OFFボタン追加（SVGアイコン）、デフォルトOFF設計
- 2026-03-17: フェーズ4開始。ステップ1完了: backchannel.shのバッファリング除去（cat→dd bs=320）。ATOM→Mac遅延0.5秒以下、Mac→ATOM遅延約1秒。CPU load 3.3、idle 8%
- 2026-03-17: フェーズ4ステップ2完了: マイクボタンによるastream制御。Mac→ATOM遅延約2秒（FIFOフラッシュで10秒→2秒に改善）。マイクOFF時load 3.72、ON時load 4.00。hack_ini.cgiのCONFIG_VER消失バグも修正
- 2026-03-17: フェーズ4ステップ3完了: ブラウザ側AEC/NS/AGC無効化（音質改善、遅延効果は軽微）。feed_pcmリトライ間隔短縮は効果なく取りやめ
- 2026-03-17: フェーズ4ステップ4完了: 遅延計測＋初期バーストスキップ。astream側遅延ゼロ確認、起動時バースト対策で約2秒→約1秒に改善。残り1秒はWebRTC/go2rtc区間
- 2026-03-18: go2rtcバッファパッチ(0005)を除去。bufferSize=5 vs 100を比較し、パッチは不要と結論。ビルド手順を精査・整理。1回目セッションのFIFOバッファ蓄積問題を新たに特定（backchannel.shの常時書き込みが原因）
