# 双方向会話（Two-Way Audio）実装計画

## 概要
AtomCamの公式アプリでサポートされている双方向会話機能を、atomcam_toolsのWebRTC/Web UI経由で実現する。

## 現状整理

### 実装済み
- `aplay`コマンドによるWAVファイル再生（`local_sdk_speaker_feed_pcm_data()`）
- マイクキャプチャ（`audio_callback.c`, ALSA loopback経由）
- エコーキャンセル（IMP SDK `IMP_AI_EnableAec()`）
- WebRTC配信（go2rtc v1.9.2, 受信専用）
- 音声処理（ノイズ抑制、AGC等）

### 未実装
- リアルタイム音声ストリーミング再生（ファイルではなくストリームからのスピーカー出力）
- WebRTCの双方向化（現在`recvonly`固定）
- go2rtcバックチャネル設定

## アーキテクチャ（最終目標）
```
ブラウザMIC → WebRTC(sendrecv) → go2rtc → exec:ffmpeg(Opus→PCM 8kHz) → FIFO → libcallback → スピーカー
```

## フェーズ分け

### フェーズ1: ストリーミング再生の基盤確立 — 完了 (2026-03-15)
**目標**: PCからATOMのスピーカーにリアルタイムで音声をストリーミング再生できることを検証

#### 1-1. `astream`コマンドの実装 — 完了
- 新規ファイル: `libcallback/audio_stream.c`
- FIFOからPCM(8kHz/16bit/mono)を読み続けてスピーカーに出力
- コマンド: `astream <fifo_path> [volume]` / `astream stop` / `astream`(状態確認)
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
- [ ] 遅延はどの程度か（目標: 500ms以下） → 未計測
- [ ] CPU負荷は許容範囲か → 未計測
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

#### 2-1. go2rtc.yaml設定変更
- `rtspserver.sh`でバックチャネル用exec:ソースを追加
- FFmpegによるOpus→PCM 8kHz変換パイプライン

#### 2-2. 確認ポイント
- [x] go2rtcの`exec:`+`#backchannel=1`がMIPSEL環境で動作するか → OK
- [x] go2rtcがPCMAをstdinに書き込む → OK
- [x] FIFO経由でastream(alawデコード)からスピーカー出力されるか → OK
- [x] 遅延・音質は実用レベルか → 遅延約2秒、音質はまあまあ（改善余地あり）
- [x] WebRTC接続中の安定性 → OK（映像停止・ハングなし）

### フェーズ3: WebRTC双方向化 — 完了 (2026-03-15)
**目標**: ブラウザから双方向音声通話ができるようにする（フェーズ2と同時に実装）

#### 3-1. webrtc.html変更
- `recvonly` → `sendrecv`
- `getUserMedia()`でブラウザマイク取得
- マイクON/OFFボタン

#### 3-2. Web UI統合
- Setting.vueにマイク制御UIを追加

### フェーズ4: 品質改善
- エコーキャンセル（AEC）パラメータ調整（`IMP_AI_EnableAec()`は動作確認済み）
- ノイズ抑制の最適化
- 遅延の最小化

## 技術メモ

### 重要なファイル
| ファイル | 役割 |
|---------|------|
| `libcallback/audio_play.c` | 既存のWAV再生実装。`local_sdk_speaker_feed_pcm_data()`の使い方の参考 |
| `libcallback/command.c` | コマンドインターフェース。新コマンド登録先 |
| `libcallback/audio_callback.c` | マイクキャプチャ。ALSAパターンの参考 |
| `libcallback/audio_control.c` | 音声処理制御（AEC, AGC, NS等） |
| `web/source/webrtc.html` | WebRTCクライアント |
| `overlay_rootfs/scripts/rtspserver.sh` | go2rtc設定生成・起動 |
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

## リスク・課題
| 課題 | 深刻度 | 対策 |
|------|--------|------|
| go2rtc exec:+backchannel動作確認 | 高 | フェーズ1でストリーミング基盤を先に確立し、段階的に検証 |
| Opus→PCM変換の遅延 | 中 | FFmpegの低遅延オプション |
| エコーキャンセル | ~~中~~ 解決 | `audio aec on`で動作確認済み。パラメータ調整はフェーズ4 |
| T31 CPU負荷 | 中 | PCM変換は軽量。実測で判断 |

## 開発フロー（libcallback.soの差し替え手順）

ATOMのルートファイルシステムはsquashfs（読み取り専用）のため、以下の手順で差し替える。

### ビルド
```bash
cd /Users/sasaki/GitHub/atomcam_tools
docker compose up -d
docker compose exec builder bash -c \
  "cd /src/libcallback && CROSS_COMPILE=/atomtools/build/cross/mips-uclibc/bin/mipsel-ingenic-linux-uclibc- make"
```

### デプロイ（squashfs再構築）
```bash
# SDカードをMacに挿した状態で
docker compose exec builder bash -c "
  UNSQUASHFS=/atomtools/build/buildroot-2016.02/output/host/usr/bin/unsquashfs
  MKSQUASHFS=/atomtools/build/buildroot-2016.02/output/host/usr/bin/mksquashfs
  cd /tmp && rm -rf squashfs-root
  \$UNSQUASHFS /src/rootfs_hack.squashfs
  cp /src/libcallback/libcallback.so squashfs-root/lib/modules/libcallback.so
  rm -f /src/rootfs_hack_new.squashfs
  \$MKSQUASHFS squashfs-root /src/rootfs_hack_new.squashfs -comp xz -noappend
"
cp rootfs_hack_new.squashfs /Volumes/ATOMCAM/rootfs_hack.squashfs
```

### 注意事項
- デプロイ前に `rootfs_hack.squashfs` のバックアップを推奨
- SDカードをATOMに戻して電源ONで反映される
- SSH接続には `~/.ssh/config` に以下の設定が必要（OpenSSH 10.x + ATOMのOpenSSH 7.1の互換性問題）:
  ```
  Host atomcam
    PubkeyAcceptedAlgorithms +ssh-rsa
    HostkeyAlgorithms +ssh-rsa
  ```

## フェーズ2 実装ノート（2026-03-15 作業中）

### 実装済みの変更
1. **`overlay_rootfs/scripts/backchannel.sh`** — go2rtcバックチャネル用スクリプト（新規作成）
2. **`overlay_rootfs/scripts/rtspserver.sh`** — バックチャネル用exec:エントリとFIFO準備を追加
3. **`web/source/webrtc.html`** — マイク送信(sendonly)トランシーバー追加、WebSocket URL修正

### 動作確認済み
- go2rtcの`exec:#backchannel=1`でbackchannel.shが起動される → OK
- go2rtcがブラウザのマイク音声をPCMA(alaw 8kHz)としてexecプロセスのstdinに書き込む → OK
- ffmpegがstdinからPCMAを読み取り、s16leへの変換マッピングを認識する → OK
- WebRTC映像配信（RTSPソース経由）は正常動作 → OK
- ブラウザからのマイク許可はHTTPでは出ない → `atomcam.local`ホスト名でアクセスする必要あり

### 未解決の問題: ffmpegからFIFOへの書き込みがブロック

**症状**: ffmpegが入力(pipe:0/pipe:3)を認識しOutputのマッピングも正しいが、出力先のFIFO(`/tmp/audio_in.fifo`)をopenする段階でブロックし、データが流れない。

**原因分析**:
1. **go2rtcがstdoutをキャプチャする**: `exec:`で起動されたプロセスのstdoutはgo2rtcがパイプとして掴む。そのためシェルの`> /tmp/audio_in.fifo`リダイレクトが効かない（go2rtcのパイプが優先される）
2. **FIFO openのデッドロック**: astream側は`open(fifo, O_RDONLY)`（ブロッキング）で書き手待ち。ffmpegの`open(fifo, O_WRONLY)`は読み手待ち。タイミングによってはデッドロックする
3. **ffmpegのファイル存在チェック**: ffmpegは出力先が既に存在するとエラー終了する → `-y`フラグで回避可能
4. **`sleep <> fifo`でFIFO keeperを仕込む**: RWでopenし続けるプロセスを追加すればblockは解消されるはずだが、実験ではffmpegが起動すらしなかった（原因不明）

**試したアプローチと結果**:
| アプローチ | 結果 |
|-----------|------|
| `ffmpeg ... pipe:1 > /tmp/audio_in.fifo` | go2rtcがstdoutを掴むため、リダイレクトが効かない |
| `ffmpeg ... /tmp/audio_in.fifo` (直接出力) | `File already exists. Exiting.` → `-y`で解消 |
| `ffmpeg -y ... /tmp/audio_in.fifo` | FIFO openでブロック（読み手不在） |
| `exec 3<&0; ffmpeg -y -i pipe:3 ... /tmp/audio_in.fifo` | 同上、FIFO openでブロック |
| `sleep 86400 <> /tmp/audio_in.fifo &` (keeper) + 上記 | ffmpegが起動しなかった（原因不明） |

### 解決: 案A+案Cの組み合わせで実装 (2026-03-15)

**FIFOブロック問題（案A）**:
- `audio_stream.c`: `open(streamPath, O_RDONLY)` → `open(streamPath, O_RDONLY | O_NONBLOCK)` に変更
- open後に`fcntl()`でO_NONBLOCKを外してブロッキングreadに戻す
- これによりFIFOのopen()がデッドロックしなくなる

**PCMA→s16le変換（案C）**:
- `audio_stream.c`にa-lawデコード関数を追加
- `astream`コマンドに`alaw`オプションを追加: `astream <path> <vol> alaw`
- go2rtcのPCMAデータをffmpeg不要でそのままFIFOに流し、astream内でデコード
- `backchannel.sh`: `exec cat > /tmp/audio_in.fifo`（stdoutを/dev/nullに閉じてからcat）

**変更したファイル**:
1. `libcallback/audio_stream.c` — O_NONBLOCK open + a-lawデコード + alawオプション
2. `overlay_rootfs/scripts/backchannel.sh` — stdout閉じてcat→FIFO
3. `overlay_rootfs/scripts/rtspserver.sh` — astream起動に`alaw`オプション追加

### go2rtc関連の知見
- go2rtc v1.9.2 バックチャネルの仕組み: `#backchannel=1`付きexec:のstdinにPCMA rawデータを書き込む
- WebRTCクライアントは`media=video+audio+microphone`パラメータでマイク有効化
- HOMEKIT_SOURCEは`rtsp://localhost:8554/video0_unicast`（`video0`ではない）
- WebRTC接続中はATOMのCPU/ネットワーク負荷が高くSSHがタイムアウトしやすい
- go2rtcのstatic_dirを`/tmp/www-backchannel`に変えてテスト用HTMLを配信可能（/var/wwwはsquashfs読み取り専用）
- lighttpdはポート80、go2rtc APIはポート1984
- `getUserMedia()`はHTTPS必須 → `atomcam.local`ホスト名か、Chromeフラグ`#unsafely-treat-insecure-origin-as-secure`で回避

### ATOMデプロイのテスト手順（squashfs不要の方法）
```bash
# SDカードにスクリプト配置
cat backchannel.sh | ssh atomcam 'cat > /media/mmc/backchannel.sh && chmod +x /media/mmc/backchannel.sh'
# webrtc.htmlを/tmp/www-backchannel/に配置
cat webrtc.html | ssh atomcam 'cat > /tmp/www-backchannel/index.html'
# go2rtc設定を/tmp/に書いて起動（static_dir=/tmp/www-backchannel）
# rtspserver.shの変更はgo2rtc.yamlを直接書くことで代替可能
```

## 更新履歴
- 2026-03-15: 初版作成。調査完了、フェーズ1着手開始
- 2026-03-15: フェーズ1完了。astream コマンド実装・検証成功（サイン波、MP3ストリーミング再生確認）
- 2026-03-15: フェーズ1+完了。双方向通話の基本検証成功（PC↔ATOM同時通話、AECによるエコー軽減確認）
- 2026-03-15: フェーズ2完了。go2rtcバックチャネル連携実装。ブラウザ→ATOM双方向音声通話成功（遅延約2秒、安定動作確認）
