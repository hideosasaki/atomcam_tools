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

### フェーズ1: ストリーミング再生の基盤確立 ← 現在ここ
**目標**: PCからATOMのスピーカーにリアルタイムで音声をストリーミング再生できることを検証

#### 1-1. `astream`コマンドの実装
- 新規ファイル: `libcallback/audio_stream.c`
- FIFOまたはソケットからPCM(8kHz/16bit/mono)を読み続けてスピーカーに出力
- コマンド: `astream <fifo_path> [volume]` / `astream stop`
- `command.c`のCommandTableに登録

#### 1-2. 動作検証
```bash
# ATOM上
mkfifo /tmp/audio_in.fifo
echo "astream /tmp/audio_in.fifo 40" | nc localhost 4000

# Macから
ffmpeg -f avfoundation -i ":0" -f s16le -ar 8000 -ac 1 - | ssh root@atomcam 'cat > /tmp/audio_in.fifo'
```

#### 1-3. 確認ポイント
- [ ] FIFOからの連続読み込みでスピーカー出力が途切れないか
- [ ] `local_sdk_speaker_feed_pcm_data()`のストリーミング呼び出しが安定するか
- [ ] 遅延はどの程度か（目標: 500ms以下）
- [ ] CPU負荷は許容範囲か

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

## 更新履歴
- 2026-03-15: 初版作成。調査完了、フェーズ1着手開始
