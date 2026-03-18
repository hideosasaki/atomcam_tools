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
- PCMパスのdrain誤検出修正（フェーズ4のdrain機構追加以降、catやffmpegパイプでのPCM再生が動作しない）

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

8. 1回目セッションのFIFOバッファ蓄積問題の解決 — 完了 (2026-03-18)
   - **原因特定**: WebRTC接続時にgo2rtcがbackchannel.shを起動し、ddがFIFOに常時書き込み。astream未起動中に蓄積されたstaleデータがマイクON時にスピーカーバッファを詰まらせていた（feed_retry=100/100が24バッチ以上継続）
   - **試行した方式**:
     - フラグファイルでbackchannel.shの書き込み先をゲート → マイクON直後に最大1秒の音声欠落が発生（ddのcount単位でしかフラグチェックできない）
     - 時間ベーススキップ（first_read後500msデータ読み捨て） → 最初のreadがブロッキングで返った時点で既に500ms経過しスキップが機能しない（以前の知見と同じ）
   - **採用した方式**: stale検出+drainモード
     - feedでリトライ発生 → 即座にspeaker_clean_buf_data() + FIFOフラッシュ → drainモードに遷移
     - drainモード: read間隔が15ms超（リアルタイムデータ）に安定するまでデータを読み捨て続ける
     - staleデータをスピーカーに送り込まずに済むため、バッファ詰まりが解消
   - **検証結果**: feed_retry=0/100（以前は100/100）、astream側total_ms=10ms（以前は5209ms）
   - astream側のFIFO蓄積問題は解決。残る体感遅延はcmd.cgi HTTP POST遅延（別課題）

#### 試行して取りやめたステップ
- `audio_stream.c`: `feed_pcm_data`リトライ間隔を10ms→2msに短縮 → 効果なし、取りやめ
- `audio_stream.c`: FIFOフラッシュ後500ms時間ベーススキップ → readがブロッキングなので時間ベースが機能しない（read完了時点でskipUntilを過ぎている）、取りやめ
- `audio_stream.c`: バイト数ベーススキップ（8000B→2000B） → go2rtcからのデータ供給がリアルタイムなのでリアルタイムデータまで読み捨ててしまい音が出なくなる、取りやめ
- `audio_stream.c`: スキップ完全除去（FIFOフラッシュのみ） → ホットデプロイが効かず検証未完了

#### 現在の状態 (2026-03-18)
- go2rtcバッファパッチは不要と判断し除去。upstream状態（bufferSize=100）で運用
- 1回目セッションのFIFOバッファ蓄積問題は**解決済み**（astream側feed_retry=0）
- cmd.cgi HTTP POST遅延の計測を実施し、問題が解消していることを確認
- **Mac→ATOM音声遅延**: 1回目約1秒（fetch 420ms）、2回目以降約0.5秒（fetch 204ms）で実用レベル
- ATOM→Mac遅延: 0.5秒以下で良好
- **フェーズ4の遅延改善は完了**

#### cmd.cgi HTTP POST遅延の計測結果 (2026-03-18)
webrtc.htmlに計測コード（performance.now()ベース）を追加して検証:
| 操作 | fetch時間 | 体感遅延 |
|------|-----------|----------|
| 1回目 start | 420ms | ~1秒 |
| 1回目 stop | 5045ms | - |
| 2回目 start | 204ms | ~0.5秒 |
| 2回目 stop | 62ms | - |

前回セッションで「1回目15秒無音」だった原因は、squashfs内のwebrtc.htmlがステップ6/7の変更（nullトラックによるRTP送信制御、astream起動順序最適化）を含んでいなかったため。bind mountで最新版に差し替えたところ正常動作を確認。SDデプロイ（squashfs再構築）で恒久的に修正済み。

#### 成果
- Mac→ATOM音声遅延: 1回目約1秒、2回目以降約0.5秒 — 実用レベル達成
- ATOM→Mac遅延: 0.5秒以下で良好
- マイクOFF時: astream停止、RTP送信も停止（CPU・帯域節約）
- マイクON時: astream起動→stale自動リカバリ→即再生
- マイクボタンによるオンデマンド制御が正常動作

#### 課題
- ATOM起動直後のRTSP接続タイムアウト（運用上許容、起動後安定すれば問題なし）

### フェーズ4+: WebRTC接続の高速化
- ~~外部STUNサーバー除去~~ — `iceServers`を空に変更。LAN内利用ではSTUN不要、DNS解決+UDP通信のオーバーヘッドを排除
- ~~go2rtcストリームソースからMJPEG除去~~ — `get_jpeg.cgi`（MJPEG）ソースを除去し、RTSPのみに。go2rtcが毎回MJPEGプロデューサーを起動→停止する無駄を排除
- AECパラメータ調整 → スキップ（`IMP_AI_EnableAec()`はon/offのみでパラメータ調整の余地なし）
- ノイズ抑制の最適化 → 保留（NS 0-3のレベル選択可能、起動時はNS無効。`audio ns`コマンドで運用時に調整可能）
- WebRTCストリームのSub(360p)対応 → 保留（CPU負荷計測の結果、MJPEG除去後はWebRTC接続時load~4.8/切断時~3.4で差が小さく、効果が限定的。コマ落ち等が発生すれば再検討）

#### 結果
- 映像表示時間: 体感で若干の改善（初回7-10秒→約5秒）。残りの遅延はRTSP接続確立+WebRTCネゴシエーション自体の所要時間
- CPU負荷: MJPEG除去の副次効果でWebRTC接続中のloadが~7→~4.8に改善

## フェーズ5: リファクタリング — 完了 (2026-03-18)
**目標**: フェーズ1〜4+で蓄積した技術的負債を解消し、保守性・信頼性を改善する

#### 完了したステップ
1. **audio_stream.c — 計測ログ除去** (~80行削減)
   - `/tmp/astream_measure.log`, `/tmp/astream_latency.log` への書き込みをすべて削除
   - 重要イベント（stale検出、drain完了）は `printf` でシステムログに残した
   - 不要になった変数・タイマー変数も削除（`openTime`, `startTime`, `t0`, `t2`, `logCount`等）

2. **audio_stream.c — 名前付き定数導入**
   - 12個のマジックナンバーを `#define` 定数に置換
   - `BUF_LENGTH`, `ALAW_BUF_LEN`, `DRAIN_THRESHOLD_MS`, `FEED_RETRY_US`, `FIFO_REOPEN_US`, `SOURCE_CLOSE_WAIT_US`, `SPEAKER_MODE_ACTIVE/OFF`, `DEFAULT_VOLUME`, `MAX_VOLUME`, `MAX_PATH_LEN`

3. **audio_stream.c — パラメータバリデーション追加**
   - volume を `[0, MAX_VOLUME]` にクランプ

4. **audio_stream.c — volatile修飾子追加**
   - `streamRunning`, `streamVolume`, `streamAlaw` に `volatile` を追加（スレッド間共有変数の安全性）

5. **webrtc.html — エラーハンドリング改善**
   - `fetch().catch(function(){})` → `console.warn` でエラーログ出力
   - WebSocket `error`/`close` ハンドラ追加（切断時に `astream stop` 送信）
   - ハードコード値を変数に抽出 (`GO2RTC_PORT`, `AUDIO_FIFO`, `DEFAULT_VOL`)

6. **rtspserver.sh / backchannel.sh — デッドコード除去**
   - rtspserver.sh: コメントアウト行 `#/usr/bin/go2rtc $option -daemon` を削除
   - backchannel.sh: FIFO存在チェック `[ ! -p "$FIFO" ] && exit 1` を追加

7. **audio_stream.c — alaw/PCMパス統合** (~30行削減)
   - read+decode部分のみ `if(streamAlaw)` で分岐し、drain・feed・stale検出等の共通ロジックを1箇所に統合
   - alaw（WebRTC経由）で動作確認済み

#### 既知の課題
- **PCMパスでdrain誤検出**: `cat`やffmpegパイプでPCMデータをFIFOに流すと、初回feedリトライ後にstale検出→drain→音声破棄される。フェーズ4のdrain機構追加時からの問題（リファクタリング起因ではない）。現在PCMパスの利用予定はないが、固定音再生等の将来用途に備え修正が必要

#### 成果
- audio_stream.c: 355行 → 244行（111行削減）

### フェーズ6: 運用構築 — Home Assistant統合 + HTTPS対応 (2026-03-18)
**目標**: HAダッシュボードからカメラ映像+双方向会話を使えるようにする。raspi上の他サービスも含めてHTTPS化。

#### 環境
- HAサーバー: Raspberry Pi (192.168.0.2, Tailscale IP: 100.103.210.33)、HA Container (Docker, host network)、Tailscale稼働中
- 同居サービス: pihole (v6, :8080)、Music Assistant (:8095)、dvd-stream-box (:5000)
- ATOMカメラ: 同一LAN (192.168.0.12)、go2rtc v1.9.2、WebRTC双方向対応済み
- nginx: 停止済み（Caddyに移行完了）
- アクセス元: LAN内PC/スマホ、Tailscale経由（外出先）

#### 最終アーキテクチャ: Caddy + Tailscale HTTPS + パスベースルーティング
```
ブラウザ / HAアプリ / Fully Kiosk Browser
  │ HTTPS (Tailscale証明書 = Let's Encrypt)
  ▼
Caddy (192.168.0.2:443 + 100.103.210.33:443) ── TLS終端、リバースプロキシ
  ├── /                    → localhost:8123      (Home Assistant、ルート)
  ├── /go2rtc/*            → 192.168.0.12:1984   (ATOM go2rtc API/WebSocket)
  ├── /cgi-bin/*           → 192.168.0.12:80     (ATOM cmd.cgi)
  ├── /atomcam/*           → 192.168.0.12:80     (ATOM Web UI)
  ├── /ma/*                → localhost:8095      (Music Assistant)
  ├── /dvd/*               → localhost:5000      (dvd-stream-box)
  ├── /pihole/*            → localhost:8080      (pihole v6)
  └── /www/*               → /var/www/html       (静的サイト)

ドメイン: home.barn-alpha.ts.net
TLS証明書: tailscale cert（Let's Encrypt自動発行）
pihole Local DNS: home.barn-alpha.ts.net → 192.168.0.2（LAN内はLAN IP直接）
```

#### *.homeサブドメイン方式からの移行経緯
当初はCaddy自己署名証明書 + pihole Local DNS（*.home）でサブドメイン方式を採用したが、以下の理由で断念しTailscale HTTPSに移行:
- **Android HAアプリが自己署名CAを信頼しない**: Android 7.0+ではアプリがuser-installed CAを信頼しない（`network_security_config`が必要だがHA Companion Appは未対応、GitHub Issue #5735）
- **Mac HAアプリ（WKWebView）が制約多い**: クロスオリジンiframeで自己署名証明書を拒否。さらにRTCPeerConnection自体が存在しない
- **サブドメインとパスベースの混在は運用が複雑**: 一部サービスが*.homeで他がhome.barn-alpha.ts.netになると混乱する
- **Tailscale HTTPSなら全て解決**: 正規Let's Encrypt証明書、全プラットフォームで信頼される、外出先アクセスも可能

#### raspi上の設定ファイル
| ファイル | 役割 |
|---------|------|
| `~/homeassistant/docker-compose.yml` | HA, Music Assistant, Caddyのコンテナ定義 |
| `~/homeassistant/Caddyfile` | リバースプロキシ設定（handle_path方式） |
| `~/homeassistant/home.barn-alpha.ts.net.crt` | Tailscale HTTPS証明書 |
| `~/homeassistant/home.barn-alpha.ts.net.key` | Tailscale HTTPS秘密鍵 |
| `~/homeassistant/config/configuration.yaml` | HA設定（trusted_proxies: 127.0.0.1, ::1, 100.64.0.0/10） |
| `~/homeassistant/config/www/atomcam-card.js` | HAカスタムカード（デプロイ先） |
| `~/homeassistant/config/www/fullscreen.html` | Fully Kiosk Browser用ページ（デプロイ先） |

#### Tailscale HTTPS証明書の取得・配置
```bash
# raspi上で証明書を取得
sudo tailscale cert home.barn-alpha.ts.net
# 生成される: home.barn-alpha.ts.net.crt, home.barn-alpha.ts.net.key

# HAディレクトリにコピー（docker-compose.ymlでCaddyコンテナにマウント）
cp home.barn-alpha.ts.net.crt ~/homeassistant/
cp home.barn-alpha.ts.net.key ~/homeassistant/
chmod 644 ~/homeassistant/home.barn-alpha.ts.net.key  # Caddyコンテナからの読み取り用
```

docker-compose.yml の Caddy volumes:
```yaml
volumes:
  - ./Caddyfile:/etc/caddy/Caddyfile
  - caddy_data:/data
  - caddy_config:/config
  - /var/www/html:/var/www/html:ro
  - ./home.barn-alpha.ts.net.crt:/etc/caddy/cert.crt:ro
  - ./home.barn-alpha.ts.net.key:/etc/caddy/cert.key:ro
```

#### Caddyfile（現行）
```caddyfile
{
    https_port 443
    http_port 8443
    default_bind 192.168.0.2 100.103.210.33
}

home.barn-alpha.ts.net {
    tls /etc/caddy/cert.crt /etc/caddy/cert.key

    # Trailing slash redirects
    @atomcam-redir path /atomcam
    redir @atomcam-redir /atomcam/ 301
    @dvd-redir path /dvd
    redir @dvd-redir /dvd/ 301
    @ma-redir path /ma
    redir @ma-redir /ma/ 301
    @pihole-redir path /pihole
    redir @pihole-redir /pihole/ 301
    @www-redir path /www
    redir @www-redir /www/ 301

    handle_path /go2rtc/* {
        reverse_proxy http://192.168.0.12:1984
    }
    handle /cgi-bin/* {
        reverse_proxy http://192.168.0.12:80
    }
    handle_path /atomcam/* {
        reverse_proxy http://192.168.0.12:80
    }
    handle_path /ma/* {
        reverse_proxy localhost:8095
    }
    handle_path /dvd/* {
        reverse_proxy localhost:5000
    }
    handle_path /pihole/* {
        reverse_proxy localhost:8080
    }
    handle_path /www/* {
        root * /var/www/html
        file_server browse
    }
    handle {
        reverse_proxy localhost:8123
    }
}
```
注: `handle_path`は自動でprefixをstripする。`handle`+`uri strip_prefix`の組み合わせは不要。
注: `/cgi-bin/*`はstrip不要なのでそのまま`handle`を使用。

#### 完了したステップ
1. **pihole Local DNSにレコード追加** — 完了
   - `home.barn-alpha.ts.net → 192.168.0.2`（LAN内クライアントがLAN IPで直接アクセスするため）
   - 旧*.homeレコード（ha.home, atomcam.home等）は廃止
2. **CaddyをDockerで起動** — 完了
   - `~/homeassistant/docker-compose.yml` にcaddyサービス追加（`caddy:2`, host network）
   - Tailscale証明書をコンテナにマウント
3. **Tailscale Serve競合の解消** — 完了
   - tailscaledがTailscale IP:443をlistenしていたためCaddyがbindできず
   - `sudo tailscale serve reset`で解決
   - Caddyの`default_bind`にLAN IP(192.168.0.2) + Tailscale IP(100.103.210.33)を指定
4. **HA configuration.yaml修正** — 完了
   - `trusted_proxies` に `127.0.0.1`, `::1`, `100.64.0.0/10`（Tailscale CGNAT範囲）
5. **HAカスタムカード（atomcam-card.js）** — 完了
   - 直接WebRTC接続（iframeなし）。カード内でRTCPeerConnection + go2rtc WebSocket
   - 同一オリジン（Caddy経由）なのでCORS/Permissions Policy問題なし
   - lazy getUserMedia（マイクボタン初回押下時にリクエスト）
   - sendonly transceiver事前作成（SDP negotiationにバックチャネルを含める）
   - `base_url`設定でWebSocket/fetch先オリジンを明示指定可能（HAアプリのWebViewではlocation.hostが内部アドレスを返すため）
   - HA Lovelace設定例:
     ```yaml
     type: custom:atomcam-card
     src: video0
     base_url: https://home.barn-alpha.ts.net
     ```
6. **fullscreen.html（Fully Kiosk Browser用）** — 完了
   - フルスクリーンWebRTCページ。マイクON=音量100%+スピーカーON、OFF=ミュート
   - 大きなマイクボタン(112px)、音量ボタンなし（シンプルUI）
   - URL: `https://home.barn-alpha.ts.net/local/fullscreen.html`
7. **Caddyfile handle→handle_path移行** — 完了
   - 末尾スラッシュなしURL（/atomcam, /dvd等）の404問題を修正

#### プラットフォーム別動作状況
| プラットフォーム | 映像 | 音声(受信) | マイク(送信) | 備考 |
|----------------|------|-----------|------------|------|
| Chrome (Mac/Android) | ✓ | ✓ | ✓ | HAダッシュボード経由で全機能OK |
| Safari (Mac) | ✓ | ✓ | ✓ | 初回カメラ/マイク許可ダイアログあり |
| Fully Kiosk Browser (Android) | ✓ | ✓ | ✓ | PLUS版 + Enable Microphone Access設定が必要 |
| Android HAアプリ (WebView) | ✓ | ✓ | ✗ | getUserMediaがpending（WebView内でマイク許可ダイアログが出ない） |
| Mac HAアプリ (WKWebView) | ✗ | ✗ | ✗ | RTCPeerConnectionが存在しない |

#### Fully Kiosk Browser設定要件
- **PLUS版ライセンス**が必要（無料版ではマイクアクセス不可）
- Settings → Advanced Web Settings → **Enable Microphone Access**: ON
- Settings → Advanced Web Settings → **Enable Webcam Access**: ON（任意）
- Androidの権限設定で「マイク」を許可
- URL: `https://home.barn-alpha.ts.net/local/fullscreen.html`

#### 今後の検討事項
- aiseg2のリバースプロキシ（Digest認証+X-Frame-Options問題のため保留）
- nginx除去（停止中だが未削除）
- pihole Docker化

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
| `web/source/webrtc.html` | WebRTCクライアント（ATOM直接アクセス用、双方向対応 + マイクボタンUI） |
| `web/source/vue/Setting.vue` | 管理画面（双方向会話スイッチ + WebRTCUrl生成） |
| `web/source/vue/i18n-ja.yaml` | 日本語ローカライズ |
| `web/source/vue/i18n-en.yaml` | 英語ローカライズ |
| `custompackages/package/go2rtc/go2rtc.mk` | go2rtc v1.9.2ビルド設定 |
| `homeassistant/atomcam-card.js` | HAカスタムカード（WebRTC直接接続、Caddy経由同一オリジン） |
| `homeassistant/fullscreen.html` | Fully Kiosk Browser用フルスクリーンページ |

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
- `getUserMedia()`はSecure Context必須（HTTPSまたはlocalhost）。Tailscale HTTPS（Let's Encrypt証明書）で対応。自己署名証明書はAndroidアプリで使えないため不採用
- HAアプリ（WKWebView/WebView）ではgetUserMediaが制限される。Fully Kiosk Browser（PLUS版）で代替

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
- 2026-03-18: フェーズ4ステップ8完了: 1回目セッションのFIFOバッファ蓄積問題を解決。stale検出+drainモード方式を採用（feedリトライ検出→speaker_clean+FIFOフラッシュ→read間隔安定まで読み捨て）。astream側feed_retry=0、total_ms=10msに改善。残る体感遅延（1回目15秒、2回目5秒）はcmd.cgi HTTP POST遅延が原因と特定
- 2026-03-18: cmd.cgi遅延を計測し問題解消を確認。前回の「1回目15秒無音」はsquashfs内のwebrtc.htmlがステップ6/7未反映だったことが原因。SDデプロイで恒久修正。フェーズ4完了（1回目~1秒、2回目以降~0.5秒）
- 2026-03-18: フェーズ4+: WebRTC接続高速化。STUNサーバー除去（LAN内不要）、go2rtcストリームからMJPEGソース除去（無駄なプロデューサー起動・停止を排除）。映像表示時間が若干改善
- 2026-03-18: フェーズ5前半: リファクタリング（Step 3以外）。audio_stream.c: 計測ログ除去(~80行)、名前付き定数導入、パラメータバリデーション、volatile追加（355→276行）。webrtc.html: エラーハンドリング改善、定数抽出。rtspserver.sh/backchannel.sh: デッドコード除去・FIFOチェック追加
- 2026-03-18: フェーズ6着手: raspi上にCaddy（リバースプロキシ）+ pihole Local DNSで全サービスHTTPS化。pihole DNSにha/atomcam/ma/dvd/pihole.home登録、Caddyfile作成・Docker起動、HA trusted_proxiesに::1追加。ha/ma/dvd/pihole.homeの動作確認OK。atomcam.homeはATOM電源OFF中のため未確認。webrtc.htmlの変更不要（サブドメイン方式でlocation.protocol/hostnameがそのまま機能）
- 2026-03-18: フェーズ6完了: CaddyルートCA証明書をMacに登録（保護されていない通信の警告解消）。ATOM電源ON→atomcam.homeでWebRTC双方向会話動作確認。静的サイトをwww.homeに移行（file_server browse）。HAダッシュボード統合はカスタムカード（atomcam-card.js）で実現（標準iframeカードはallow="microphone"未対応、panel_iframeはHA 2026.2で廃止）。HAから映像+双方向会話の動作確認完了
- 2026-03-18: フェーズ6+: HAアプリ対応の試行と最終アーキテクチャ決定。以下の経緯を経て構成を確定:
  1. **自己署名証明書+サブドメインの限界**: Caddy自己署名CA証明書はMacでは手動インストールで動作したが、Android HAアプリはuser-installed CAを信頼しない（Android 7.0+のnetwork_security_config制約、HA Companion AppのGitHub Issue #5735）。Mac HAアプリのWKWebViewはクロスオリジンiframeで自己署名証明書を拒否
  2. **Tailscale HTTPS移行**: `tailscale cert home.barn-alpha.ts.net`で正規Let's Encrypt証明書を取得。サブドメイン(*.home)を廃止し、パスベースルーティングに統一。Caddyのdefault_bindにLAN IP + Tailscale IPを追加。pihole Local DNSでLAN内はLAN IP直接アクセス
  3. **Tailscale Serve競合**: tailscaled が Tailscale IP:443をlistenしていたためCaddyがbindできず。`tailscale serve reset`で解決
  4. **HAカスタムカード方式の試行**:
     - HA標準iframeカード → `allow="microphone"`属性なしでgetUserMediaブロック
     - `panel_iframe` → HA 2026.2.3で廃止済み
     - iframe付きカスタムカード(atomcam-card.js) → Chromeで動作したがWKWebView/WebViewで制約あり
     - AlexxIT/WebRTCカード → マイクトグルUIなし（常時ON）、音量制御なし、UX不適合
     - **最終採用: 直接WebRTCカスタムカード** — iframeを廃止し、カード内で直接RTCPeerConnection+WebSocket接続。同一オリジンなのでCORS問題なし
  5. **プラットフォーム別の制約と結果**:
     - Chrome (Mac/Android): 映像+音声+マイク 全てOK ✓
     - Mac HAアプリ (WKWebView): RTCPeerConnectionが存在しない → WebRTC不可 ✗
     - Android HAアプリ (WebView): WebRTCは動作するがgetUserMediaがpending（WebView内のマイク許可ダイアログが出ない）→ 映像+音声のみ、マイク不可 △
     - Fully Kiosk Browser (Android): Enable Microphone Access (PLUS)設定で全機能動作 ✓
  6. **getUserMedia関連の修正**:
     - awaitをやめてPromise(.then)に変更 — WKWebViewでawaitが永遠に返らない問題を回避
     - さらにlazy方式に変更 — マイクボタン初回押下時にgetUserMediaを呼び出し。接続確立をブロックしない
     - sendonly transceiverを事前作成 — SDP negotiation時にバックチャネルのメディア記述を含める。後からaddTransceiverするとSDP再ネゴシエーションが必要で音が出なかった
  7. **base_url設定追加**: HAアプリのWebViewではlocation.hostがHA内部アドレス(192.168.0.2:8123)を返す。config.base_urlでWebSocket/fetch先のオリジンを明示的に指定可能に
  8. **fullscreen.html作成**: Fully Kiosk Browser用のフルスクリーンページ。音量ボタン廃止、マイクON=音量100%/OFF=ミュートのシンプルUI。大きなマイクボタン(112px)
  9. **ATOMスピーカー音量**: DEFAULT_VOL=40→100に変更（local_sdk_speaker_set_volume、範囲0-100）
  10. **Caddyfile handle→handle_path移行**: 末尾スラッシュなしURL(/atomcam, /dvd等)の404問題を修正
