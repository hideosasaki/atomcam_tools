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

### フェーズ2: go2rtcバックチャネル連携
**目標**: go2rtcのexec:ソース経由でバックチャネル音声をFIFOに流す

#### 2-1. go2rtc.yaml設定変更
- `rtspserver.sh`でバックチャネル用exec:ソースを追加
- FFmpegによるOpus→PCM 8kHz変換パイプライン

#### 2-2. 確認ポイント
- [ ] go2rtcの`exec:`+`#backchannel=1`がMIPSEL環境で動作するか
- [ ] FFmpegのコーデック変換の遅延・品質

### フェーズ3: WebRTC双方向化
**目標**: ブラウザから双方向音声通話ができるようにする

#### 3-1. webrtc.html変更
- `recvonly` → `sendrecv`
- `getUserMedia()`でブラウザマイク取得
- マイクON/OFFボタン

#### 3-2. Web UI統合
- Setting.vueにマイク制御UIを追加

### フェーズ4: 品質改善
- エコーキャンセル（AEC）の有効化とパラメータ調整
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
| エコーキャンセル | 中 | IMP SDK AEC + webrtc_profile.ini調整 |
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

## 更新履歴
- 2026-03-15: 初版作成。調査完了、フェーズ1着手開始
- 2026-03-15: フェーズ1完了。astream コマンド実装・検証成功（サイン波、MP3ストリーミング再生確認）
