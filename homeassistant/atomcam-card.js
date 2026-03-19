// ATOMCAM WebRTC Card for Home Assistant
// Direct WebRTC connection via same-origin Caddy proxy
// go2rtc: /go2rtc/api/ws, cmd.cgi: /cgi-bin/cmd.cgi
// config: src (stream name), base_url (override origin), media (video+audio+microphone)

class AtomcamCard extends HTMLElement {
  setConfig(config) {
    this._config = config;
  }

  set hass(hass) {
    if (this._built) return;
    this._built = true;
    this._render();
    this._connect();
  }

  _render() {
    var src = this._config.src ? this._config.src : 'video0';
    this._src = src;

    this.innerHTML = '';
    var card = document.createElement('ha-card');

    var container = document.createElement('div');
    container.style.cssText = 'position:relative;width:100%;background:black;';

    var video = document.createElement('video');
    video.id = 'atomcam-video';
    video.autoplay = true;
    video.playsinline = true;
    video.muted = true;
    video.style.cssText = 'width:100%;display:block;';
    container.appendChild(video);
    this._video = video;

    // Mic button (bottom-center)
    var micControls = document.createElement('div');
    micControls.style.cssText = 'position:absolute;top:10px;right:10px;z-index:10;display:none;';
    this._micControls = micControls;

    var micBtn = document.createElement('button');
    micBtn.style.cssText = 'width:80px;height:80px;border-radius:50%;border:none;cursor:pointer;opacity:0.8;display:flex;align-items:center;justify-content:center;background:var(--primary-color, #03a9f4);';
    micControls.appendChild(micBtn);
    this._micBtn = micBtn;

    container.appendChild(micControls);
    card.appendChild(container);
    this.appendChild(card);


    // Mic state
    var micOnIcon = '<svg viewBox="0 0 24 24" width="36" height="36"><path d="M12 14c1.66 0 3-1.34 3-3V5c0-1.66-1.34-3-3-3S9 3.34 9 5v6c0 1.66 1.34 3 3 3z" fill="white"/><path d="M17 11c0 2.76-2.24 5-5 5s-5-2.24-5-5H5c0 3.53 2.61 6.43 6 6.92V21h2v-3.08c3.39-.49 6-3.39 6-6.92h-2z" fill="white"/></svg>';
    var micOffIcon = '<svg viewBox="0 0 24 24" width="36" height="36"><path d="M19 11h-1.7c0 .74-.16 1.43-.43 2.05l1.23 1.23c.56-.98.9-2.09.9-3.28zm-4.02.17c0-.06.02-.11.02-.17V5c0-1.66-1.34-3-3-3S9 3.34 9 5v.18l5.98 5.99zM4.27 3L3 4.27l6.01 6.01V11c0 1.66 1.33 3 2.99 3 .22 0 .44-.03.65-.08l1.66 1.66c-.71.33-1.5.52-2.31.52-2.76 0-5.3-2.1-5.3-5.1H5c0 3.41 2.72 6.23 6 6.72V21h2v-3.28c.91-.13 1.77-.45 2.54-.9L19.73 21 21 19.73 4.27 3z" fill="white"/></svg>';

    this._micOnIcon = micOnIcon;
    this._micOffIcon = micOffIcon;
    micBtn.innerHTML = micOffIcon;
  }

  _sendAstreamCmd(cmd) {
    var base = this._config.base_url ? this._config.base_url : '';
    var url = base + '/cgi-bin/cmd.cgi?port=socket';
    return fetch(url, {
      method: 'POST',
      body: JSON.stringify({exec: cmd})
    });
  }

  _cleanup() {
    if (this._disconnectTimer) { clearTimeout(this._disconnectTimer); this._disconnectTimer = null; }
    if (this._micEnabled) {
      this._micEnabled = false;
      if (this._micSender) this._micSender.replaceTrack(null);
      if (this._micTrack) this._micTrack.enabled = false;
      this._video.muted = true;
      this._video.volume = 0;
      this._updateMicButton();
      this._sendAstreamCmd('astream stop').catch(function() {});
    }
    this._micSender = null;
    if (this._ws) { try { this._ws.close(); } catch (e) {} this._ws = null; }
    if (this._pc) { try { this._pc.close(); } catch (e) {} this._pc = null; }
  }

  _scheduleRetry() {
    var self = this;
    if (this._retryTimer) return;
    console.log('[AtomcamCard] retry in ' + this._retryDelay + 'ms');
    this._retryTimer = setTimeout(function() {
      self._retryTimer = null;
      self._connect();
    }, this._retryDelay);
    this._retryDelay = Math.min(this._retryDelay * 2, 30000);
  }

  _updateMicButton() {
    var micBtn = this._micBtn;
    micBtn.style.background = this._micEnabled ? '#bc423a' : 'var(--primary-color, #03a9f4)';
    micBtn.innerHTML = this._micEnabled ? this._micOnIcon : this._micOffIcon;
    micBtn.title = this._micEnabled ? 'Microphone ON' : 'Microphone OFF';
  }

  _connect() {
    var self = this;
    var AUDIO_FIFO = '/tmp/audio_in.fifo';
    var DEFAULT_VOL = 100;
    var micBtn = this._micBtn;
    var micControls = this._micControls;

    if (!this._retryDelay) this._retryDelay = 3000;

    this._cleanup();

    // PeerConnection
    var pc;
    try {
      pc = new RTCPeerConnection({ iceServers: [] });
    } catch (e) {
      this._scheduleRetry();
      return;
    }
    this._pc = pc;

    pc.addEventListener('iceconnectionstatechange', function() {
      var state = pc.iceConnectionState;
      console.log('[AtomcamCard] ICE state: ' + state);
      if (state === 'connected') {
        self._retryDelay = 3000;
        if (self._disconnectTimer) { clearTimeout(self._disconnectTimer); self._disconnectTimer = null; }
        self._video.play().catch(function() {});
      } else if (state === 'disconnected') {
        if (!self._disconnectTimer) {
          self._disconnectTimer = setTimeout(function() {
            self._disconnectTimer = null;
            console.log('[AtomcamCard] disconnected timeout, reconnecting');
            self._cleanup();
            self._scheduleRetry();
          }, 5000);
        }
      } else if (state === 'failed') {
        self._cleanup();
        self._scheduleRetry();
      }
    });

    var tracks = ['video', 'audio'].map(function(kind) {
      return pc.addTransceiver(kind, {direction: 'recvonly'}).receiver.track;
    });
    var stream = new MediaStream(tracks);
    this._video.srcObject = stream;

    // Microphone: pre-create sendonly transceiver for SDP negotiation
    var media = this._config.media ? this._config.media : 'video+audio+microphone';
    if (media.indexOf('microphone') >= 0) {
      var sendTransceiver = pc.addTransceiver('audio', {direction: 'sendonly'});
      this._micSender = sendTransceiver.sender;
      micControls.style.display = '';
      this._updateMicButton();
    }

    // Mic button handler (only bind once)
    if (!this._micBound) {
      this._micBound = true;
      micBtn.addEventListener('click', function() {
        if (!self._pc) return;
        if (self._micEnabled) {
          self._micEnabled = false;
          if (self._micSender) self._micSender.replaceTrack(null);
          if (self._micTrack) self._micTrack.enabled = false;
          self._video.muted = true;
          self._video.volume = 0;
          self._updateMicButton();
          self._sendAstreamCmd('astream stop').catch(function() {});
        } else {
          micBtn.style.opacity = '0.5';
          micBtn.innerHTML = self._micOnIcon;

          function startMic() {
            return self._sendAstreamCmd('astream ' + AUDIO_FIFO + ' ' + DEFAULT_VOL + ' alaw').then(function() {
              self._micEnabled = true;
              self._micTrack.enabled = true;
              if (self._micSender) self._micSender.replaceTrack(self._micTrack);
              self._video.muted = false;
              self._video.volume = 1.0;
              self._updateMicButton();
              micBtn.style.opacity = '0.8';
            });
          }

          if (!self._micTrack) {
            navigator.mediaDevices.getUserMedia({audio: {
              echoCancellation: false,
              noiseSuppression: false,
              autoGainControl: false
            }}).then(function(micStream) {
              self._micTrack = micStream.getTracks()[0];
              self._micTrack.enabled = false;
              return startMic();
            }).catch(function() {
              self._updateMicButton();
              micBtn.style.opacity = '0.8';
            });
          } else {
            startMic().catch(function() {
              self._updateMicButton();
              micBtn.style.opacity = '0.8';
            });
          }
        }
      });
    }

    // WebSocket to go2rtc via Caddy proxy
    var base = this._config.base_url ? this._config.base_url : (location.protocol + '//' + location.host);
    var wsUrl = base + '/go2rtc/api/ws?src=' + this._src;
    var ws;
    try {
      ws = new WebSocket(wsUrl.replace(/^http/, 'ws'));
    } catch (e) {
      this._cleanup();
      this._scheduleRetry();
      return;
    }
    this._ws = ws;

    ws.addEventListener('close', function() {
      console.log('[AtomcamCard] WebSocket closed');
      self._cleanup();
      self._scheduleRetry();
    });

    ws.addEventListener('open', function() {
      pc.addEventListener('icecandidate', function(ev) {
        if (!ev.candidate) return;
        if (self._ws && self._ws.readyState === WebSocket.OPEN) {
          self._ws.send(JSON.stringify({type: 'webrtc/candidate', value: ev.candidate.candidate}));
        }
      });

      pc.createOffer().then(function(offer) {
        return pc.setLocalDescription(offer);
      }).then(function() {
        if (self._ws && self._ws.readyState === WebSocket.OPEN) {
          self._ws.send(JSON.stringify({type: 'webrtc/offer', value: pc.localDescription.sdp}));
        }
      });
    });

    ws.addEventListener('message', function(ev) {
      var msg = JSON.parse(ev.data);
      if (msg.type === 'webrtc/candidate') {
        pc.addIceCandidate({candidate: msg.value, sdpMid: '0'});
      } else if (msg.type === 'webrtc/answer') {
        pc.setRemoteDescription({type: 'answer', sdp: msg.value});
      }
    });
  }

  getCardSize() {
    return 5;
  }
}

customElements.define('atomcam-card', AtomcamCard);
