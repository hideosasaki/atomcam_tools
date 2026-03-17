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
- 遅延の最適化（現状約2秒）
- 音質改善

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

**ビルド手順**: プロジェクトルートの `build.md` を参照。libcallback.soはuClibc環境（`/atomtools/build/cross/mips-uclibc/bin/mipsel-ingenic-linux-uclibc-`）でビルドすること。glibc版gcc（`mipsel-ingenic-linux-gnu-`）を使うとヘッダー不一致でエラーになる。
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

#### 試行して取りやめたステップ
- `audio_stream.c`: `feed_pcm_data`リトライ間隔を10ms→2msに短縮 → 効果なし、取りやめ

#### 成果
- Mac→ATOM音声遅延: 約1秒（ベースライン2秒から改善）
- ATOM→Mac遅延: 0.5秒以下で良好
- マイクOFF時: astream停止、load 3.72（CPU節約）
- マイクON時: astream起動、load 4.00
- マイクボタンによるオンデマンド制御が正常動作

#### 課題
- Mac→ATOM遅延が目標500ms未達（現状約1秒）。残りはブラウザ→WebRTC→go2rtcの区間
- ATOM側（astream）の遅延はほぼゼロに最適化済み

#### 今後の検討ステップ
1. go2rtcバックチャネルのバッファ設定調査（内部バッファリングの可能性）
2. WebRTCのjitter buffer調査（ブラウザ側のバッファ）
3. go2rtcを介さない直接パス（WebSocket等）の検討

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
squashfsは読み取り専用だが、overlayfs上のファイルやtmpfsにコピーして差し替え可能。
SDカード経由のデプロイはTrendMicroのスキャンでアンマウントが遅延するため、可能な限りSCP経由で直接差し替えること。

```bash
# libcallback.so: /tmp経由で差し替え（実行中のiCameraは再起動必要）
scp libcallback/libcallback.so atomcam:/tmp/
ssh atomcam "cp /tmp/libcallback.so /lib/modules/libcallback.so"
# iCamera再起動（WebRTCセッション切断に注意）
ssh atomcam "killall iCamera_app"  # 自動再起動される

# スクリプト: 直接上書き可能（overlayfs上）
scp overlay_rootfs/scripts/backchannel.sh atomcam:/scripts/backchannel.sh
scp web/source/webrtc.html atomcam:/var/www/webrtc.html
```

**注意**: ホットデプロイは再起動すると元に戻る（squashfsが読み直されるため）。恒久化するにはSDカード経由でsquashfsを再構築すること。

### SDカードデプロイ時の注意
- コピー後、**10秒待ってから`diskutil unmount`する**（TrendMicroのスキャン完了を待つ）
- 通常のunmountが失敗する場合は再度待ってリトライ。強制アンマウント(`force`)は最終手段

### ビルド
```bash
cd /Users/sasaki/GitHub/atomcam_tools
docker compose up -d
docker compose exec builder bash -c \
  "cd /src/libcallback && CROSS_COMPILE=/atomtools/build/cross/mips-uclibc/bin/mipsel-ingenic-linux-uclibc- make"
```

### Webフロントエンドビルド（Setting.vue等を変更した場合）
```bash
cd /Users/sasaki/GitHub/atomcam_tools/web
rm -rf frontend
./node_modules/.bin/webpack --mode production --progress
```

### デプロイ（squashfs再構築）
```bash
# SDカードをMacに挿した状態で
docker compose exec builder bash -c "
  UNSQUASHFS=/atomtools/build/buildroot-2016.02/output/host/usr/bin/unsquashfs
  MKSQUASHFS=/atomtools/build/buildroot-2016.02/output/host/usr/bin/mksquashfs
  cd /tmp && rm -rf squashfs-root
  \$UNSQUASHFS /src/rootfs_hack.squashfs
  # libcallback
  cp /src/libcallback/libcallback.so squashfs-root/lib/modules/libcallback.so
  # スクリプト
  cp /src/overlay_rootfs/scripts/rtspserver.sh squashfs-root/scripts/rtspserver.sh
  cp /src/overlay_rootfs/scripts/backchannel.sh squashfs-root/scripts/backchannel.sh
  chmod +x squashfs-root/scripts/backchannel.sh
  # Webフロントエンド（Setting.vue等のビルド済みファイル）
  rm -f squashfs-root/var/www/bundle*
  cp -pr /src/web/frontend/* squashfs-root/var/www/
  # squashfs構築
  rm -f /src/rootfs_hack_new.squashfs
  \$MKSQUASHFS squashfs-root /src/rootfs_hack_new.squashfs -comp gzip -noappend
"
cp rootfs_hack_new.squashfs /Volumes/ATOMCAM/rootfs_hack.squashfs
```

### 注意事項
- **`atom_root.squashfs`は絶対に上書きしないこと**。これはATOM本体の公式ファームウェア（SPI Flashからコピーされたもの）であり、上書きするとATOMが起動しなくなる。デプロイ先は`rootfs_hack.squashfs`のみ。
- **squashfsの圧縮形式は必ず gzip を使うこと**（`-comp gzip`）。元のrootfs_hack.squashfsがgzip圧縮であり、カメラのカーネルがxzをサポートしていないため、xzで構築すると起動しない。
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
