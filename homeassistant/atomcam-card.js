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

    // Volume controls (bottom-left)
    var volControls = document.createElement('div');
    volControls.style.cssText = 'position:absolute;bottom:10px;left:10px;z-index:10;display:flex;align-items:center;gap:4px;';

    var volStyle = 'width:40px;height:40px;border-radius:50%;border:none;background:rgba(255,255,255,0.15);cursor:pointer;opacity:0.8;display:flex;align-items:center;justify-content:center;';

    var volIconBtn = document.createElement('button');
    volIconBtn.style.cssText = volStyle;
    volControls.appendChild(volIconBtn);
    this._volIconBtn = volIconBtn;

    var volDown = document.createElement('button');
    volDown.style.cssText = volStyle;
    volDown.innerHTML = '<svg viewBox="0 0 24 24" width="18" height="18"><path d="M19 13H5v-2h14v2z" fill="white"/></svg>';
    volControls.appendChild(volDown);

    var volUp = document.createElement('button');
    volUp.style.cssText = volStyle;
    volUp.innerHTML = '<svg viewBox="0 0 24 24" width="18" height="18"><path d="M19 13h-6v6h-2v-6H5v-2h6V5h2v6h6v2z" fill="white"/></svg>';
    volControls.appendChild(volUp);

    container.appendChild(volControls);

    // Mic button (bottom-center)
    var micControls = document.createElement('div');
    micControls.style.cssText = 'position:absolute;bottom:10px;left:50%;transform:translateX(-50%);z-index:10;display:none;';
    this._micControls = micControls;

    var micBtn = document.createElement('button');
    micBtn.style.cssText = 'width:40px;height:40px;border-radius:50%;border:none;cursor:pointer;opacity:0.8;display:flex;align-items:center;justify-content:center;background:#666;';
    micControls.appendChild(micBtn);
    this._micBtn = micBtn;

    container.appendChild(micControls);
    card.appendChild(container);
    this.appendChild(card);

    // Volume state
    var VOL_KEY = 'atomcam_card_volume';
    var volume = 1.0;
    var prevVolume = 0.5;

    var speakerBase = '<path d="M3 9v6h4l5 5V4L7 9H3z" fill="white"/>';
    var wave1 = '<path d="M14 8.5a4.5 4.5 0 0 1 0 7" fill="none" stroke="white" stroke-width="1.5" stroke-linecap="round"/>';
    var wave2 = '<path d="M15.5 5.5a8 8 0 0 1 0 13" fill="none" stroke="white" stroke-width="1.5" stroke-linecap="round" opacity="0.7"/>';
    var wave3 = '<path d="M17 3a11 11 0 0 1 0 18" fill="none" stroke="white" stroke-width="1.5" stroke-linecap="round" opacity="0.4"/>';
    var muteX = '<line x1="15" y1="8" x2="21" y2="16" stroke="white" stroke-width="2" stroke-linecap="round"/><line x1="21" y1="8" x2="15" y2="16" stroke="white" stroke-width="2" stroke-linecap="round"/>';
    var volIcons = [
      '<svg viewBox="0 0 24 24" width="18" height="18">' + speakerBase + muteX + '</svg>',
      '<svg viewBox="0 0 24 24" width="18" height="18">' + speakerBase + '</svg>',
      '<svg viewBox="0 0 24 24" width="18" height="18">' + speakerBase + wave1 + '</svg>',
      '<svg viewBox="0 0 24 24" width="18" height="18">' + speakerBase + wave1 + wave2 + '</svg>',
      '<svg viewBox="0 0 24 24" width="18" height="18">' + speakerBase + wave1 + wave2 + wave3 + '</svg>'
    ];

    function loadVolume() {
      try {
        var saved = localStorage.getItem(VOL_KEY);
        if (saved !== null) volume = parseFloat(saved);
      } catch(e) {}
    }

    function saveVolume() {
      try { localStorage.setItem(VOL_KEY, volume); } catch(e) {}
    }

    var self = this;
    function applyVolume() {
      video.volume = volume;
      video.muted = (volume === 0);
      var idx = volume === 0 ? 0 : Math.ceil(volume * 4);
      volIconBtn.innerHTML = volIcons[idx];
      saveVolume();
    }

    volIconBtn.addEventListener('click', function() {
      if (volume > 0) {
        prevVolume = volume;
        volume = 0;
      } else {
        volume = prevVolume > 0 ? prevVolume : 0.25;
      }
      applyVolume();
    });

    volDown.addEventListener('click', function() {
      volume = Math.max(0, Math.round((volume - 0.25) * 100) / 100);
      applyVolume();
    });

    volUp.addEventListener('click', function() {
      volume = Math.min(1, Math.round((volume + 0.25) * 100) / 100);
      applyVolume();
    });

    loadVolume();
    applyVolume();

    // Mic state
    var micOnIcon = '<svg viewBox="0 0 24 24" width="18" height="18"><path d="M12 14c1.66 0 3-1.34 3-3V5c0-1.66-1.34-3-3-3S9 3.34 9 5v6c0 1.66 1.34 3 3 3z" fill="white"/><path d="M17 11c0 2.76-2.24 5-5 5s-5-2.24-5-5H5c0 3.53 2.61 6.43 6 6.92V21h2v-3.08c3.39-.49 6-3.39 6-6.92h-2z" fill="white"/></svg>';
    var micOffIcon = '<svg viewBox="0 0 24 24" width="18" height="18"><path d="M19 11h-1.7c0 .74-.16 1.43-.43 2.05l1.23 1.23c.56-.98.9-2.09.9-3.28zm-4.02.17c0-.06.02-.11.02-.17V5c0-1.66-1.34-3-3-3S9 3.34 9 5v.18l5.98 5.99zM4.27 3L3 4.27l6.01 6.01V11c0 1.66 1.33 3 2.99 3 .22 0 .44-.03.65-.08l1.66 1.66c-.71.33-1.5.52-2.31.52-2.76 0-5.3-2.1-5.3-5.1H5c0 3.41 2.72 6.23 6 6.72V21h2v-3.28c.91-.13 1.77-.45 2.54-.9L19.73 21 21 19.73 4.27 3z" fill="white"/></svg>';

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

  _connect() {
    var self = this;
    var AUDIO_FIFO = '/tmp/audio_in.fifo';
    var DEFAULT_VOL = 40;
    var micTrack = null;
    var micSender = null;
    var micEnabled = false;
    var micBtn = this._micBtn;
    var micControls = this._micControls;

    function updateMicButton() {
      micBtn.style.background = micEnabled ? '#bc423a' : '#666';
      micBtn.innerHTML = micEnabled ? self._micOnIcon : self._micOffIcon;
      micBtn.title = micEnabled ? 'Microphone ON' : 'Microphone OFF';
    }

    // PeerConnection
    var pc;
    try {
      pc = new RTCPeerConnection({ iceServers: [] });
    } catch (e) {
      return;
    }

    pc.addEventListener('iceconnectionstatechange', function() {
      if (pc.iceConnectionState === 'connected') {
        self._video.play().catch(function() {});
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
      micSender = sendTransceiver.sender;
      micControls.style.display = '';
      updateMicButton();
    }

    // Mic button handler (lazy getUserMedia on first click)
    micBtn.addEventListener('click', function() {
      if (micEnabled) {
        micEnabled = false;
        if (micSender) micSender.replaceTrack(null);
        if (micTrack) micTrack.enabled = false;
        updateMicButton();
        self._sendAstreamCmd('astream stop').catch(function() {});
      } else {
        micBtn.style.background = '#bc423a';
        micBtn.style.opacity = '0.5';
        micBtn.innerHTML = self._micOnIcon;

        function startMic() {
          return self._sendAstreamCmd('astream ' + AUDIO_FIFO + ' ' + DEFAULT_VOL + ' alaw').then(function() {
            micEnabled = true;
            micTrack.enabled = true;
            if (micSender) micSender.replaceTrack(micTrack);
            updateMicButton();
            micBtn.style.opacity = '0.8';
          });
        }

        if (!micTrack) {
          navigator.mediaDevices.getUserMedia({audio: {
            echoCancellation: false,
            noiseSuppression: false,
            autoGainControl: false
          }}).then(function(micStream) {
            micTrack = micStream.getTracks()[0];
            micTrack.enabled = false;
            return startMic();
          }).catch(function() {
            updateMicButton();
            micBtn.style.opacity = '0.8';
          });
        } else {
          startMic().catch(function() {
            updateMicButton();
            micBtn.style.opacity = '0.8';
          });
        }
      }
    });

    // WebSocket to go2rtc via Caddy proxy
    var base = this._config.base_url ? this._config.base_url : (location.protocol + '//' + location.host);
    var wsUrl = base + '/go2rtc/api/ws?src=' + this._src;
    var ws;
    try {
      ws = new WebSocket(wsUrl.replace(/^http/, 'ws'));
    } catch (e) {
      return;
    }

    ws.addEventListener('close', function() {
      if (micEnabled) {
        micEnabled = false;
        updateMicButton();
        self._sendAstreamCmd('astream stop');
      }
    });

    ws.addEventListener('open', function() {
      pc.addEventListener('icecandidate', function(ev) {
        if (!ev.candidate) return;
        ws.send(JSON.stringify({type: 'webrtc/candidate', value: ev.candidate.candidate}));
      });

      pc.createOffer().then(function(offer) {
        return pc.setLocalDescription(offer);
      }).then(function() {
        ws.send(JSON.stringify({type: 'webrtc/offer', value: pc.localDescription.sdp}));
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
