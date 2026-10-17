// BlackHole Windows Native Client UI Logic
// Integrates Native Winsock UDP/TCP Radar + Zero-Knowledge WebRTC Room Code Sharing

const SIGNALING_WS_URL = 'wss://share.sunilshahid.com/ws';

// Global State
let currentSettings = {
  alias: 'BlackHole-PC',
  destinationFolder: '',
  quickSave: true,
  port: 53317,
  encryptionEnabled: true
};

let selectedFiles = []; // Metadata: [{ name, size, path }]
let selectedFileObjects = []; // DOM File instances for WebRTC streaming
let activePeers = [];
let localIps = [];

// WebRTC Session State
let webrtcSenderWS = null;
let webrtcSenderManager = null;
let currentSenderCode = '';
let isSenderTransferring = false;
let senderCancelled = false;

let webrtcReceiverWS = null;
let webrtcReceiverManager = null;
let receiverEngine = null;
let isReceiverTransferring = false;

// Helper to communicate with C++ Win32 Backend
function sendToCpp(action, data = {}) {
  if (window.chrome && window.chrome.webview) {
    window.chrome.webview.postMessage(JSON.stringify({ action, ...data }));
  } else {
    console.log('[Dev Mock Webview]', action, data);
  }
}

// Byte formatting
function formatBytes(bytes) {
  if (!bytes || bytes === 0) return '0 B';
  const k = 1024;
  const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
  const i = Math.floor(Math.log(bytes) / Math.log(k));
  return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}

// Tab Switching
document.querySelectorAll('.tab-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
    document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));

    btn.classList.add('active');
    const tabId = 'tab-' + btn.getAttribute('data-tab');
    const target = document.getElementById(tabId);
    if (target) target.classList.add('active');
  });
});

// Dropzone & File Handling
const dropzone = document.getElementById('dropzone');
const filePicker = document.getElementById('file-picker');

dropzone.addEventListener('click', () => {
  filePicker.click();
});

filePicker.addEventListener('change', (e) => {
  if (e.target.files && e.target.files.length > 0) {
    addFileObjects(Array.from(e.target.files));
  }
});

dropzone.addEventListener('dragover', (e) => {
  e.preventDefault();
  dropzone.classList.add('dragover');
});

dropzone.addEventListener('dragleave', () => {
  dropzone.classList.remove('dragover');
});

dropzone.addEventListener('drop', (e) => {
  e.preventDefault();
  dropzone.classList.remove('dragover');
  if (e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files.length > 0) {
    addFileObjects(Array.from(e.dataTransfer.files));
  }
});

function addFileObjects(newFiles) {
  newFiles.forEach(f => {
    // Avoid duplicates
    if (!selectedFileObjects.some(existing => existing.name === f.name && existing.size === f.size)) {
      selectedFileObjects.push(f);
      selectedFiles.push({
        name: f.name,
        size: f.size,
        path: f.path || f.name // On WebView2/Chromium, f.path may be set or f.name
      });
    }
  });
  renderSelectedFiles();
}

function renderSelectedFiles() {
  const container = document.getElementById('selected-files-list');
  container.innerHTML = '';

  if (selectedFiles.length === 0) return;

  const totalBytes = selectedFiles.reduce((acc, f) => acc + (f.size || 0), 0);

  const header = document.createElement('div');
  header.style.display = 'flex';
  header.style.justifyContent = 'space-between';
  header.style.alignItems = 'center';
  header.style.padding = '8px 4px';
  header.style.fontSize = '0.85rem';
  header.style.color = 'var(--text-secondary)';
  header.innerHTML = `
    <span><strong>${selectedFiles.length}</strong> file(s) selected (${formatBytes(totalBytes)})</span>
    <span style="cursor: pointer; color: #ef4444;" onclick="clearAllFiles()">Clear All</span>
  `;
  container.appendChild(header);

  selectedFiles.forEach((f, idx) => {
    const item = document.createElement('div');
    item.className = 'file-item';
    item.innerHTML = `
      <div class="file-name" title="${f.name}">${f.name}</div>
      <div style="display: flex; align-items: center; gap: 10px;">
        <span class="file-size">${formatBytes(f.size)}</span>
        <span style="cursor: pointer; color: #ef4444; font-weight: bold;" onclick="removeSelectedFile(${idx})">✕</span>
      </div>
    `;
    container.appendChild(item);
  });
}

window.removeSelectedFile = function(idx) {
  selectedFiles.splice(idx, 1);
  selectedFileObjects.splice(idx, 1);
  renderSelectedFiles();
};

window.clearAllFiles = function() {
  selectedFiles = [];
  selectedFileObjects = [];
  if (filePicker) filePicker.value = '';
  renderSelectedFiles();
};

// ========================================================
// 🌐 WEBRTC ROOM CODE SHARING (SENDER)
// ========================================================
const btnStartWebShare = document.getElementById('btn-start-web-share');
const btnStopWebShare = document.getElementById('btn-stop-web-share');
const webShareIdle = document.getElementById('web-share-idle');
const webShareActive = document.getElementById('web-share-active');
const shareCodeDisplay = document.getElementById('share-code-display');
const shareUrlText = document.getElementById('share-url-text');
const qrcodeContainer = document.getElementById('qrcode-container');
const webShareStatus = document.getElementById('web-share-status');
const btnCopyCode = document.getElementById('btn-copy-code');
const btnCopyUrl = document.getElementById('btn-copy-url');

let qrcodeInstance = null;
let p2pEstablished = false;
let relayFallbackTimeout = null;

btnStartWebShare.addEventListener('click', () => {
  if (selectedFileObjects.length === 0) {
    filePicker.click();
    return;
  }
  startWebRTCShareSession();
});

btnStopWebShare.addEventListener('click', () => {
  stopWebRTCShareSession();
});

btnCopyCode.addEventListener('click', () => {
  if (currentSenderCode) {
    navigator.clipboard.writeText(currentSenderCode);
    btnCopyCode.innerText = 'Copied! ✓';
    setTimeout(() => { btnCopyCode.innerText = 'Copy Code'; }, 1800);
  }
});

btnCopyUrl.addEventListener('click', () => {
  if (currentSenderCode) {
    const url = `https://share.sunilshahid.com/?code=${currentSenderCode}`;
    navigator.clipboard.writeText(url);
    btnCopyUrl.innerText = 'Copied! ✓';
    setTimeout(() => { btnCopyUrl.innerText = 'Copy Direct Link'; }, 1800);
  }
});

function stopWebRTCShareSession() {
  clearTimeout(relayFallbackTimeout);
  relayFallbackTimeout = null;
  senderCancelled = true;
  isSenderTransferring = false;
  p2pEstablished = false;

  if (webrtcSenderWS) {
    try { webrtcSenderWS.close(); } catch {}
    webrtcSenderWS = null;
  }
  if (webrtcSenderManager) {
    webrtcSenderManager.close();
    webrtcSenderManager = null;
  }

  currentSenderCode = '';
  webShareActive.style.display = 'none';
  webShareIdle.style.display = 'flex';
  document.getElementById('transfer-overlay').classList.remove('active');
}

function startWebRTCShareSession() {
  stopWebRTCShareSession();
  senderCancelled = false;

  webShareIdle.style.display = 'none';
  webShareActive.style.display = 'flex';
  shareCodeDisplay.innerText = '...';
  webShareStatus.innerText = 'Connecting to signaling network...';

  try {
    const ws = new WebSocket(SIGNALING_WS_URL);
    ws.binaryType = 'arraybuffer';
    webrtcSenderWS = ws;

    ws.onopen = () => {
      ws.send(JSON.stringify({ type: 'create-room' }));
    };

    ws.onmessage = async (event) => {
      // Binary ACK from receiver
      if (event.data instanceof ArrayBuffer) {
        const packet = decodePacket(event.data);
        if (packet.type === 'control') {
          if (packet.message.type === 'TRANSFER_ACK') {
            onTransferCompleted('Web Client');
          } else if (packet.message.type === 'TRANSFER_CANCEL') {
            alert('Transfer was cancelled by receiver.');
            stopWebRTCShareSession();
          }
        }
        return;
      }

      let msg;
      try {
        msg = JSON.parse(event.data);
      } catch {
        return;
      }

      if (msg.type === 'room-created') {
        currentSenderCode = msg.code;
        shareCodeDisplay.innerText = msg.code;
        const shareUrl = `https://share.sunilshahid.com/?code=${msg.code}`;
        shareUrlText.innerText = shareUrl;
        webShareStatus.innerText = 'Waiting for recipient to connect...';

        // Render QR Code
        qrcodeContainer.innerHTML = '';
        if (typeof QRCode !== 'undefined') {
          qrcodeInstance = new QRCode(qrcodeContainer, {
            text: shareUrl,
            width: 180,
            height: 180,
            colorDark: "#000000",
            colorLight: "#ffffff",
            correctLevel: QRCode.CorrectLevel.M
          });
        }

        // Initialize WebRTC Manager as Sender
        initWebRTCSender(msg.code, ws, msg.localIps || localIps);
      }
      else if (msg.type === 'peer-joined') {
        webShareStatus.innerText = 'Receiver connected! Opening secure tunnel...';

        if (webrtcSenderManager) {
          const offer = await webrtcSenderManager.createOffer();
          ws.send(JSON.stringify({ type: 'signal', data: { sdp: offer } }));
        }

        // Fallback to relay after 8 seconds if direct P2P does not open
        clearTimeout(relayFallbackTimeout);
        relayFallbackTimeout = setTimeout(() => {
          if (!p2pEstablished && !isSenderTransferring) {
            console.log('[WebRTC Sender] P2P timeout. Switching to Encrypted WebSocket Relay...');
            startRelaySender(currentSenderCode);
          }
        }, 8000);
      }
      else if (msg.type === 'signal') {
        if (msg.data?.requestRelay) {
          if (!p2pEstablished) {
            startRelaySender(currentSenderCode);
          }
        } else if (msg.data?.sdp) {
          await webrtcSenderManager?.handleAnswer(msg.data.sdp);
        } else if (msg.data?.candidate) {
          await webrtcSenderManager?.handleCandidate(msg.data.candidate);
        }
      }
      else if (msg.type === 'peer-disconnected') {
        webShareStatus.innerText = 'Receiver disconnected.';
      }
    };

    ws.onerror = () => {
      webShareStatus.innerText = 'Failed to connect to signaling server.';
    };
  } catch (err) {
    webShareStatus.innerText = 'Error: ' + (err.message || err);
  }
}

function initWebRTCSender(code, ws, knownIps) {
  const webrtc = new WebRTCManager(true, {
    onConnectionModeChange: (mode) => {
      updateModeBadge(mode);
    },
    onSignal: (data) => {
      if (ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ type: 'signal', data }));
      }
    },
    onDataChannelOpen: async () => {
      p2pEstablished = true;
      clearTimeout(relayFallbackTimeout);
      webShareStatus.innerText = 'Direct secure tunnel established! Streaming files...';

      // Verify PING / PONG
      const dc = webrtc.getDataChannel();
      let pongReceived = false;
      const onMsg = (ev) => {
        if (ev.data instanceof ArrayBuffer) {
          const pkt = decodePacket(ev.data);
          if (pkt.type === 'control' && pkt.message.type === 'RTC_PONG') {
            pongReceived = true;
          }
        }
      };

      if (dc) {
        dc.addEventListener('message', onMsg);
        try {
          await webrtc.sendPacket(encodeControlPacket({ type: 'RTC_PING' }));
        } catch {}
      }

      for (let i = 0; i < 20; i++) {
        if (pongReceived) break;
        await new Promise(r => setTimeout(r, 100));
      }
      if (dc) dc.removeEventListener('message', onMsg);

      startWebRTCDataTransfer(code, webrtc);
    },
    onDataChannelClose: () => {
      if (isSenderTransferring && !senderCancelled) {
        startRelaySender(code);
      }
    },
    onError: () => {
      if (isSenderTransferring && !senderCancelled) {
        startRelaySender(code);
      }
    }
  }, knownIps);

  webrtc.init();
  webrtcSenderManager = webrtc;
}

async function startWebRTCDataTransfer(code, transport) {
  isSenderTransferring = true;
  showTransferOverlay('Sending Files via WebRTC...', 'Web Client');

  try {
    const cryptoKey = await deriveKeyFromSecret(code);
    await sendFilesWebRTC(
      selectedFileObjects,
      transport,
      cryptoKey,
      (p) => {
        updateTransferOverlayProgress(p);
      },
      () => senderCancelled
    );

    onTransferCompleted('Web Client');
  } catch (err) {
    if (!senderCancelled) {
      console.warn('[WebRTC Sender] Fallback to relay:', err);
      startRelaySender(code);
    }
  }
}

async function startRelaySender(code) {
  if (!webrtcSenderWS || webrtcSenderWS.readyState !== WebSocket.OPEN) return;
  clearTimeout(relayFallbackTimeout);

  updateModeBadge('relay');
  webShareStatus.innerText = 'Connected via Encrypted Relay. Streaming...';
  showTransferOverlay('Streaming via Encrypted Relay...', 'Web Client');

  webrtcSenderWS.send(JSON.stringify({ type: 'signal', data: { switchToRelay: true } }));

  const relayTransport = {
    sendPacket: async (data) => {
      if (!webrtcSenderWS || webrtcSenderWS.readyState !== WebSocket.OPEN) {
        throw new Error('Relay closed');
      }
      while (webrtcSenderWS.bufferedAmount > 2 * 1024 * 1024) {
        if (senderCancelled) throw new Error('Cancelled');
        await new Promise(r => setTimeout(r, 15));
      }
      webrtcSenderWS.send(data.buffer);
    }
  };

  try {
    const cryptoKey = await deriveKeyFromSecret(code);
    await sendFilesWebRTC(
      selectedFileObjects,
      relayTransport,
      cryptoKey,
      (p) => {
        updateTransferOverlayProgress(p);
      },
      () => senderCancelled
    );

    onTransferCompleted('Web Client (Relay)');
  } catch (err) {
    if (!senderCancelled) {
      alert('Relay transfer error: ' + (err.message || err));
      stopWebRTCShareSession();
    }
  }
}

// ========================================================
// 📥 WEBRTC ROOM CODE RECEIVER (RECEIVE TAB)
// ========================================================
const inputReceiveCode = document.getElementById('input-receive-code');
const btnConnectCode = document.getElementById('btn-connect-code');
const receiveStatusMsg = document.getElementById('receive-status-msg');

inputReceiveCode.addEventListener('input', (e) => {
  let val = e.target.value.toUpperCase().replace(/[^A-Z0-9-]/g, '');
  if (!val.includes('-') && val.length > 3) {
    val = `${val.slice(0, 3)}-${val.slice(3, 6)}`;
  }
  if (val.length > 7) val = val.slice(0, 7);
  inputReceiveCode.value = val;
});

btnConnectCode.addEventListener('click', () => {
  const code = inputReceiveCode.value.trim().toUpperCase();
  if (code.length < 6) {
    alert('Please enter a valid 6-digit room code (e.g. 123-456)');
    return;
  }
  startWebRTCReceiverSession(code);
});

function startWebRTCReceiverSession(code) {
  receiveStatusMsg.style.display = 'block';
  receiveStatusMsg.innerText = `Connecting to room [${code}]...`;
  btnConnectCode.disabled = true;

  try {
    const ws = new WebSocket(SIGNALING_WS_URL);
    ws.binaryType = 'arraybuffer';
    webrtcReceiverWS = ws;

    const packetQueue = [];

    // Initialize Receiver WebRTC Engine
    deriveKeyFromSecret(code).then(cryptoKey => {
      receiverEngine = new FileReceiverWebRTC(
        cryptoKey,
        (p) => {
          updateTransferOverlayProgress(p);
        },
        (file) => {
          // File received! Trigger auto-download (which C++ DownloadStarting saves to destinationFolder)
          triggerFileSave(file);
        },
        () => {
          // Transfer Complete!
          onTransferCompleted(`Sender [${code}]`);
          try {
            const ack = encodeControlPacket({ type: 'TRANSFER_ACK' });
            if (webrtcReceiverManager) webrtcReceiverManager.sendPacket(ack).catch(() => {});
            if (ws.readyState === WebSocket.OPEN) ws.send(ack.buffer);
          } catch {}
          btnConnectCode.disabled = false;
        }
      );
    });

    const webrtc = new WebRTCManager(false, {
      onConnectionModeChange: (mode) => {
        updateModeBadge(mode);
      },
      onSignal: (data) => {
        if (ws.readyState === WebSocket.OPEN) {
          ws.send(JSON.stringify({ type: 'signal', data }));
        }
      },
      onDataChannelOpen: () => {
        receiveStatusMsg.innerText = 'Connected! Receiving stream...';
        showTransferOverlay('Receiving Files via WebRTC...', `Sender [${code}]`);
      },
      onDataReceived: (buffer) => {
        if (receiverEngine) {
          receiverEngine.handlePacket(buffer);
        } else {
          packetQueue.push(buffer);
        }
      }
    }, localIps);

    webrtc.init();
    webrtcReceiverManager = webrtc;

    ws.onopen = () => {
      receiveStatusMsg.innerText = 'Contacting sender...';
      ws.send(JSON.stringify({ type: 'join-room', code }));
    };

    ws.onmessage = async (event) => {
      // Binary packet over WebSocket relay
      if (event.data instanceof ArrayBuffer) {
        showTransferOverlay('Receiving Files via Relay...', `Sender [${code}]`);
        updateModeBadge('relay');
        if (receiverEngine) {
          receiverEngine.handlePacket(event.data);
        } else {
          packetQueue.push(event.data);
        }
        return;
      }

      let msg;
      try {
        msg = JSON.parse(event.data);
      } catch {
        return;
      }

      if (msg.type === 'room-joined') {
        receiveStatusMsg.innerText = 'Room joined! Establishing P2P data tunnel...';
      }
      else if (msg.type === 'signal') {
        if (msg.data?.switchToRelay) {
          updateModeBadge('relay');
          receiveStatusMsg.innerText = 'Connected via Encrypted Relay!';
          showTransferOverlay('Receiving via Encrypted Relay...', `Sender [${code}]`);
        } else if (msg.data?.sdp) {
          if (msg.data.sdp.type === 'offer') {
            const answer = await webrtc.createAnswer(msg.data.sdp);
            ws.send(JSON.stringify({ type: 'signal', data: { sdp: answer } }));
          }
        } else if (msg.data?.candidate) {
          await webrtc.handleCandidate(msg.data.candidate);
        }
      }
      else if (msg.type === 'error') {
        alert(msg.message || 'Room error');
        btnConnectCode.disabled = false;
        receiveStatusMsg.style.display = 'none';
      }
    };

    ws.onerror = () => {
      receiveStatusMsg.innerText = 'Could not reach BlackHole signaling server.';
      btnConnectCode.disabled = false;
    };
  } catch (err) {
    receiveStatusMsg.innerText = 'Error: ' + (err.message || err);
    btnConnectCode.disabled = false;
  }
}

// Save Received File directly to disk
function triggerFileSave(receivedFile) {
  // 1. Programmatically trigger link download
  // In WebView2, this fires C++ DownloadStarting, which puts resultFilePath into destinationFolder!
  try {
    const a = document.createElement('a');
    a.href = receivedFile.downloadUrl;
    a.download = receivedFile.metadata.name;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
  } catch (e) {
    console.warn('[Auto-Download Failed]', e);
  }

  // 2. Also send binary base64 to C++ for direct file write if under 50 MB
  if (receivedFile.blob.size < 50 * 1024 * 1024) {
    const reader = new FileReader();
    reader.onload = () => {
      const base64 = reader.result.split(',')[1];
      sendToCpp('save_file', {
        filename: receivedFile.metadata.name,
        dataBase64: base64
      });
    };
    reader.readAsDataURL(receivedFile.blob);
  }

  // Add to transfer history
  addTransferHistoryItem(receivedFile.metadata.name, receivedFile.metadata.size, 'Success');
}

// ========================================================
// 📡 NATIVE OFFLINE UDP / TCP RADAR & SETTINGS
// ========================================================

// Render Discovered Peers
function renderPeers() {
  const grid = document.getElementById('peer-grid');
  grid.innerHTML = '';

  if (!activePeers || activePeers.length === 0) {
    grid.innerHTML = `
      <div class="no-peers">
        Searching for nearby BlackHole devices on your Hotspot / Wi-Fi...
      </div>
    `;
    return;
  }

  activePeers.forEach(peer => {
    const card = document.createElement('div');
    card.className = 'peer-card';
    const initial = (peer.alias && peer.alias.length > 0) ? peer.alias[0].toUpperCase() : 'P';

    card.innerHTML = `
      <div class="peer-info">
        <div class="peer-avatar">${initial}</div>
        <div class="peer-details">
          <h4>${peer.alias || 'Device'}</h4>
          <span>${peer.ip}:${peer.port}</span>
        </div>
      </div>
      <button class="send-action-btn" onclick="sendToPeer('${peer.ip}', ${peer.port})">
        Send
      </button>
    `;
    grid.appendChild(card);
  });
}

window.sendToPeer = function(peerIp, peerPort) {
  if (selectedFiles.length === 0) {
    alert('Please select files first by clicking the dropzone!');
    return;
  }
  const filePaths = selectedFiles.map(f => f.path);
  sendToCpp('send_files', { peerIp, peerPort, files: filePaths });
};

// Direct IP Send
document.getElementById('btn-direct-send').addEventListener('click', () => {
  const ip = document.getElementById('direct-ip').value.trim();
  const port = parseInt(document.getElementById('direct-port').value, 10) || 53317;

  if (!ip) {
    alert('Please enter a target IP address (e.g. 192.168.43.1)');
    return;
  }
  window.sendToPeer(ip, port);
});

// Destination Folder Browse
document.getElementById('btn-browse-dest').addEventListener('click', () => {
  sendToCpp('browse_folder');
});

// Open Received Folder
document.getElementById('btn-open-download-dir').addEventListener('click', () => {
  sendToCpp('open_folder', { path: currentSettings.destinationFolder });
});

// Save Settings
document.getElementById('btn-save-settings').addEventListener('click', () => {
  const updated = {
    alias: document.getElementById('setting-alias').value.trim() || 'BlackHole-PC',
    destinationFolder: document.getElementById('setting-dest-folder').value.trim(),
    quickSave: document.getElementById('setting-quick-save').checked,
    port: parseInt(document.getElementById('setting-port').value, 10) || 53317,
    encryptionEnabled: document.getElementById('setting-encryption').checked
  };

  currentSettings = updated;
  sendToCpp('save_settings', { data: updated });

  const btn = document.getElementById('btn-save-settings');
  btn.innerText = 'Saved! ✓';
  btn.style.background = '#10b981';
  setTimeout(() => {
    btn.innerText = 'Save Settings';
    btn.style.background = '';
  }, 1800);
});

// Cancel Transfer
document.getElementById('btn-cancel-transfer').addEventListener('click', () => {
  sendToCpp('cancel_transfer');
  stopWebRTCShareSession();
  document.getElementById('transfer-overlay').classList.remove('active');
});

// ========================================================
// 📊 TRANSFER MODAL OVERLAY & PROGRESS UPDATES
// ========================================================
function showTransferOverlay(title, peerName) {
  const overlay = document.getElementById('transfer-overlay');
  overlay.classList.add('active');
  document.getElementById('transfer-modal-title').innerText = title || 'Transferring Files...';
  document.getElementById('transfer-modal-file').innerText = peerName || '';
  document.getElementById('transfer-fill').style.width = '0%';
  document.getElementById('transfer-percent').innerText = '0%';
  document.getElementById('transfer-speed').innerText = '0.0 MB/s';
  document.getElementById('transfer-bytes').innerText = '0 / 0 MB';
}

function updateModeBadge(mode) {
  const badge = document.getElementById('transfer-mode-badge');
  if (!badge) return;

  if (mode === 'lan') {
    badge.className = 'connection-pill pill-lan';
    badge.innerText = '⚡ Direct LAN Wire Speed (0 Internet Bandwidth)';
  } else if (mode === 'relay') {
    badge.className = 'connection-pill pill-relay';
    badge.innerText = '🛡️ Encrypted WebSocket Relay';
  } else if (mode === 'wan') {
    badge.className = 'connection-pill pill-wan';
    badge.innerText = '🌐 Internet WAN P2P';
  } else {
    badge.className = 'connection-pill pill-connecting';
    badge.innerText = 'Connecting...';
  }
}

function updateTransferOverlayProgress(p) {
  document.getElementById('transfer-overlay').classList.add('active');
  document.getElementById('transfer-modal-file').innerText = p.currentFileName || '';
  document.getElementById('transfer-fill').style.width = (p.percentage || 0) + '%';
  document.getElementById('transfer-percent').innerText = (p.percentage || 0) + '%';
  const speedMB = (p.speedBytesPerSec / (1024 * 1024)).toFixed(1);
  document.getElementById('transfer-speed').innerText = `${speedMB} MB/s`;
  document.getElementById('transfer-bytes').innerText = `${formatBytes(p.transferredBytes)} / ${formatBytes(p.totalBytes)}`;
}

function onTransferCompleted(peerLabel) {
  const overlay = document.getElementById('transfer-overlay');
  document.getElementById('transfer-fill').style.width = '100%';
  document.getElementById('transfer-percent').innerText = '100%';
  document.getElementById('transfer-modal-title').innerText = 'Transfer Complete! 🎉';

  addTransferHistoryItem(peerLabel, 0, 'Success');

  setTimeout(() => {
    overlay.classList.remove('active');
  }, 1600);
}

function addTransferHistoryItem(title, size, status) {
  const hist = document.getElementById('transfer-history');
  if (hist.innerText.includes('No recent')) hist.innerHTML = '';
  const item = document.createElement('div');
  item.style.padding = '8px 0';
  item.style.borderBottom = '1px solid rgba(255,255,255,0.06)';
  item.style.textAlign = 'left';
  const sizeText = size > 0 ? ` — ${formatBytes(size)}` : '';
  item.innerHTML = `<strong>${title}</strong>${sizeText} <span style="color:#10b981; float:right;">${status}</span>`;
  hist.prepend(item);
}

// Messages from Native C++ backend
if (window.chrome && window.chrome.webview) {
  window.chrome.webview.addEventListener('message', (event) => {
    const data = typeof event.data === 'string' ? JSON.parse(event.data) : event.data;

    if (data.type === 'init_data') {
      if (data.settings) {
        currentSettings = data.settings;
        document.getElementById('setting-alias').value = currentSettings.alias;
        document.getElementById('receive-alias-label').innerText = currentSettings.alias;
        document.getElementById('setting-dest-folder').value = currentSettings.destinationFolder;
        document.getElementById('setting-quick-save').checked = currentSettings.quickSave;
        document.getElementById('setting-port').value = currentSettings.port;
        document.getElementById('setting-encryption').checked = currentSettings.encryptionEnabled;
      }
      if (data.localIps && data.localIps.length > 0) {
        localIps = data.localIps;
        document.getElementById('header-ip').innerText = localIps[0];
      }
      if (data.peers) {
        activePeers = data.peers;
        renderPeers();
      }
    }
    else if (data.type === 'peers_updated') {
      activePeers = data.peers || [];
      renderPeers();
    }
    else if (data.type === 'files_selected') {
      const files = data.files || [];
      files.forEach(f => {
        if (!selectedFiles.some(existing => existing.name === f.name)) {
          selectedFiles.push(f);
        }
      });
      renderSelectedFiles();
    }
    else if (data.type === 'folder_selected') {
      currentSettings.destinationFolder = data.path;
      document.getElementById('setting-dest-folder').value = data.path;
      sendToCpp('save_settings', { data: currentSettings });
    }
    else if (data.type === 'transfer_event') {
      const ev = data;
      if (ev.eventType === 'progress') {
        document.getElementById('transfer-overlay').classList.add('active');
        document.getElementById('transfer-modal-title').innerText = 'Transferring Files...';
        document.getElementById('transfer-modal-file').innerText = ev.currentFileName || '';
        document.getElementById('transfer-fill').style.width = (ev.progressPercent || 0) + '%';
        document.getElementById('transfer-percent').innerText = (ev.progressPercent || 0) + '%';
        document.getElementById('transfer-speed').innerText = (ev.speedMBs || 0).toFixed(1) + ' MB/s';
        document.getElementById('transfer-bytes').innerText = 
          formatBytes(ev.bytesTransferred) + ' / ' + formatBytes(ev.totalBytes);
      } else if (ev.eventType === 'complete') {
        onTransferCompleted(ev.peerAlias || ev.peerIp);
      } else if (ev.eventType === 'error') {
        alert(ev.message || 'Transfer failed.');
        document.getElementById('transfer-overlay').classList.remove('active');
      }
    }
  });
}

// Request Initial Data on Load
window.addEventListener('DOMContentLoaded', () => {
  sendToCpp('get_init_data');
});
