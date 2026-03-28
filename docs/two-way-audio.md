# 双方向WebRTC音声通話（Two-Way Audio）

## 概要
ATOMCamの公式アプリでサポートされている双方向会話機能を、atomcam_toolsのWebRTC経由で実現する。ブラウザからATOMCamのスピーカーへ音声を送信し、ATOMCamのマイク音声をブラウザで受信できる。

## アーキテクチャ

### WebRTC双方向音声
```
ブラウザMIC → WebRTC(sendrecv) → go2rtc → exec:backchannel.sh(PCMA raw)
  → FIFO(/tmp/audio_in.fifo) → astream(a-law decode) → スピーカー

ATOMマイク → v4l2rtspserver(OPUS) → go2rtc → WebRTC → ブラウザスピーカー
```

### Home Assistant media_player（TTS再生）
```
HA TTS → media_player.play_media(media-source://tts/...)
  → async_resolve_media() → HTTP URL取得
  → aiohttp GET: MP3/WAVダウンロード → 一時ファイル
  → ffmpeg: MP3/WAV → raw PCM (s16le, 8kHz, mono) → 一時ファイル
  → cmd.cgi POST: astream /tmp/audio_in.fifo <volume> (非同期)
  → stream.cgi POST: PCMデータ → cat > /tmp/audio_in.fifo
  → astream: FIFO → speaker
```

## コンポーネント

### カメラ側（ATOMCam）

| ファイル | 役割 |
|---------|------|
| `libcallback/audio_stream.c` | `astream`コマンド実装。FIFOからPCM/a-lawを読み取りスピーカーに出力 |
| `libcallback/command.c` | `astream`コマンドをCommandTableに登録 |
| `overlay_rootfs/scripts/backchannel.sh` | go2rtcバックチャネルハンドラ。PCMA rawデータをFIFOに書き込み |
| `overlay_rootfs/scripts/rtspserver.sh` | go2rtc/v4l2rtspserverの起動。FIFO作成・バックチャネル設定 |
| `overlay_rootfs/var/www/cgi-bin/stream.cgi` | HTTP POST経由でPCMデータをFIFOに書き込むCGI |

### Home Assistant側

| ファイル | 役割 |
|---------|------|
| `homeassistant/custom_components/atomcam/media_player.py` | media_playerエンティティ。TTS→PCM変換→ATOMCam送信 |
| `homeassistant/custom_components/atomcam/config_flow.py` | 設定フロー（host, base_url） |
| `homeassistant/custom_components/atomcam/manifest.json` | HAコンポーネント定義 |
| `homeassistant/custom_components/atomcam/strings.json` | UI文字列 |
| `homeassistant/custom_components/atomcam/translations/` | 翻訳（en, ja） |

### フロントエンド

| ファイル | 役割 |
|---------|------|
| `atomcam/atomcam.html` | Fully Kiosk Browser向けフルスクリーンWebRTCプレーヤー。マイクボタン・音量制御付き |

## ビルド

### libcallback（クロスコンパイル）
```bash
# Dockerビルド環境（atomtools）内で実行
./build_libcallback.sh
```
MIPS向けクロスコンパイルで`libcallback.so`を生成する。

### ファームウェアイメージ（squashfs）
```bash
# Dockerビルド環境（atomtools）内で実行
./build_squashfs.sh
```
`rootfs_hack_upstream.squashfs`をベースに、変更ファイル（libcallback.so, scripts, cgi, go2rtc, web frontend）を差し替えて`rootfs_hack_new.squashfs`を生成する。

### Webフロントエンド
```bash
cd web
./node_modules/.bin/webpack --mode production --progress
```
`web/source/`のソースから`web/frontend/`にビルド成果物を生成する。

## 音声仕様

- サンプルレート: **8kHz**（ATOMCam SDKの制約により固定。16kHzを試みるとスピーカーバッファが飽和する）
- ビット深度: 16bit
- チャンネル: mono
- WebRTCコーデック: PCMA (G.711 a-law)
- エコーキャンセル: IMP SDK `IMP_AI_EnableAec()` で有効

## 既知の制限事項

- サンプルレートは8kHz固定（SDK制約）。音質は電話品質程度
- HAカスタムコンポーネントのTTS再生は一括POST方式（lighttpdのContent-Length必須制約による）。数秒〜十数秒のTTS用途では十分だが、長時間音声のストリーミングには別アプローチが必要
- HAアプリ（WKWebView/Android WebView）ではgetUserMediaが制限されるため、WebRTC双方向音声にはFully Kiosk Browser（PLUS版）を使用する
