// BlackHole Windows Native Client UI Logic
// Uncluttered LocalSend UX + Cosmic Theme + Live In-App Receiving Display

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

// QR Code Instances
let localQrInstance = null;
let remoteQrInstance = null;
let livePanelHideTimeout = null;

// ========================================================
// 🪟 GLOBAL MODAL CONTROLLERS
// ========================================================
window.openModal = function(id) {
  const modal = document.getElementById(id);
  if (modal) {
    modal.classList.add('active');
  }
};

window.closeModal = function(id) {
  const modal = document.getElementById(id);
  if (modal) {
    modal.classList.remove('active');
  }
};

// Close modal when clicking outside box
document.addEventListener('click', (e) => {
  if (e.target.classList && e.target.classList.contains('modal-overlay')) {
    e.target.classList.remove('active');
  }
});

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

function formatTime(seconds) {
  if (!seconds || seconds <= 0 || !isFinite(seconds)) return '--';
  if (seconds < 60) return `${Math.ceil(seconds)}s`;
  const m = Math.floor(seconds / 60);
  const s = Math.ceil(seconds % 60);
  return `${m}m ${s}s`;
}

// ========================================================
// 🧭 TAB NAVIGATION (Sidebar Rail)
// ========================================================
document.querySelectorAll('.nav-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.nav-btn').forEach(b => b.classList.remove('active'));
    document.querySelectorAll('.tab-pane').forEach(c => c.classList.remove('active'));

    btn.classList.add('active');
    const tabName = btn.getAttribute('data-tab');
    const target = document.getElementById('tab-' + tabName);
    if (target) target.classList.add('active');
  });
});

// ========================================================
// 📁 FILE SELECTION & DRAG-AND-DROP (LocalSend Parity)
// ========================================================
const filePicker = document.getElementById('file-picker');
const selectionGridEmpty = document.getElementById('selection-grid-empty');
const selectionCardActive = document.getElementById('selection-card-active');
const compactFilesCount = document.getElementById('compact-files-count');
const compactFilesSize = document.getElementById('compact-files-size');
const compactFilesPreview = document.getElementById('compact-files-preview');
const compactSyncStatus = document.getElementById('compact-sync-status');
const modalFileListItems = document.getElementById('modal-file-list-items');

// Empty State Tiles
const tileFile = document.getElementById('tile-file');
const tileFolder = document.getElementById('tile-folder');
const tileText = document.getElementById('tile-text');
const tilePaste = document.getElementById('tile-paste');

// Active Card Actions
const btnCompactClear = document.getElementById('btn-compact-clear');
const btnCompactAdd = document.getElementById('btn-compact-add');
const btnCompactEdit = document.getElementById('btn-compact-edit');

if (tileFile) {
  tileFile.addEventListener('click', () => {
    sendToCpp('browse_files');
  });
}

if (tileFolder) {
  tileFolder.addEventListener('click', () => {
    sendToCpp('browse_folder_files');
  });
}

if (tileText) {
  tileText.addEventListener('click', () => {
    const text = prompt('Enter a short text message or link to share:');
    if (text && text.trim().length > 0) {
      const blob = new Blob([text], { type: 'text/plain;charset=utf-8' });
      const file = new File([blob], `note_${Date.now()}.txt`, { type: 'text/plain' });
      addFileObjects([file]);
    }
  });
}

if (tilePaste) {
  tilePaste.addEventListener('click', async () => {
    try {
      if (navigator.clipboard && navigator.clipboard.readText) {
        const text = await navigator.clipboard.readText();
        if (text && text.trim().length > 0) {
          const blob = new Blob([text], { type: 'text/plain;charset=utf-8' });
          const file = new File([blob], `clipboard_${Date.now()}.txt`, { type: 'text/plain' });
          addFileObjects([file]);
          return;
        }
      }
    } catch (err) {
      console.warn('Clipboard read error:', err);
    }
    alert('Clipboard is empty or does not contain text.');
  });
}

if (btnCompactClear) {
  btnCompactClear.addEventListener('click', () => {
    clearAllFiles();
  });
}

if (btnCompactAdd) {
  btnCompactAdd.addEventListener('click', () => {
    sendToCpp('browse_files');
  });
}

if (btnCompactEdit) {
  btnCompactEdit.addEventListener('click', () => {
    window.openModal('modal-file-list');
  });
}

if (filePicker) {
  filePicker.addEventListener('change', (e) => {
    if (e.target.files && e.target.files.length > 0) {
      addFileObjects(Array.from(e.target.files));
    }
  });
}

// Window Drag and Drop
window.addEventListener('dragover', (e) => {
  e.preventDefault();
});

window.addEventListener('drop', (e) => {
  e.preventDefault();
  if (e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files.length > 0) {
    addFileObjects(Array.from(e.dataTransfer.files));
  }
});

function syncSharedFilesToCpp() {
  const paths = selectedFiles.map(f => f.path).filter(p => !!p);
  sendToCpp('set_shared_files', { files: paths });
  if (compactSyncStatus) {
    compactSyncStatus.innerText = selectedFiles.length > 0
      ? `⚡ ${selectedFiles.length} file(s) staged for hotspot link`
      : '⚡ Staged for offline hotspot link';
  }
}

function addFileObjects(newFiles) {
  newFiles.forEach(f => {
    if (!selectedFileObjects.some(existing => existing.name === f.name && existing.size === f.size)) {
      selectedFileObjects.push(f);
      selectedFiles.push({
        name: f.name,
        size: f.size,
        path: f.path || f.name
      });
    }
  });
  renderSelectedFiles();
}

function renderSelectedFiles() {
  const totalFiles = selectedFiles.length;
  const totalBytes = selectedFiles.reduce((acc, f) => acc + (f.size || 0), 0);

  if (totalFiles === 0) {
    if (selectionGridEmpty) selectionGridEmpty.style.display = 'grid';
    if (selectionCardActive) selectionCardActive.style.display = 'none';
    if (modalFileListItems) modalFileListItems.innerHTML = '<div class="popup-hint text-center">No files selected.</div>';
    syncSharedFilesToCpp();
    return;
  }

  if (selectionGridEmpty) selectionGridEmpty.style.display = 'none';
  if (selectionCardActive) selectionCardActive.style.display = 'block';

  if (compactFilesCount) compactFilesCount.innerText = `Files: ${totalFiles}`;
  if (compactFilesSize) compactFilesSize.innerText = `Size: ${formatBytes(totalBytes)}`;

  // Render mini pills preview (up to 4 items)
  if (compactFilesPreview) {
    compactFilesPreview.innerHTML = '';
    const sliceCount = Math.min(totalFiles, 4);
    for (let i = 0; i < sliceCount; i++) {
      const f = selectedFiles[i];
      const pill = document.createElement('div');
      pill.className = 'file-preview-pill';
      pill.innerHTML = `
        <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/></svg>
        <span title="${f.name}">${f.name}</span>
      `;
      compactFilesPreview.appendChild(pill);
    }
    if (totalFiles > 4) {
      const morePill = document.createElement('div');
      morePill.className = 'file-preview-pill';
      morePill.style.color = 'var(--accent-cyan)';
      morePill.innerText = `+${totalFiles - 4} more`;
      compactFilesPreview.appendChild(morePill);
    }
  }

  // Render detailed modal list items
  if (modalFileListItems) {
    modalFileListItems.innerHTML = '';
    selectedFiles.forEach((f, idx) => {
      const item = document.createElement('div');
      item.className = 'file-item';
      item.innerHTML = `
        <div class="file-name" title="${f.name}">${f.name}</div>
        <div style="display: flex; align-items: center; gap: 10px;">
          <span class="file-size">${formatBytes(f.size)}</span>
          <button class="file-remove-btn" title="Remove" onclick="removeSelectedFile(${idx})">✕</button>
        </div>
      `;
      modalFileListItems.appendChild(item);
    });
  }

  syncSharedFilesToCpp();
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
// 🎯 HEADER ACTION BUTTONS & MODAL TRIGGERS
// ========================================================

// 1. Rescan Peers (Button with Spin Animation)
const btnRescanPeers = document.getElementById('btn-rescan-peers');
if (btnRescanPeers) {
  btnRescanPeers.addEventListener('click', () => {
    btnRescanPeers.style.transform = 'rotate(180deg)';
    btnRescanPeers.style.transition = 'transform 0.4s ease';
    setTimeout(() => { btnRescanPeers.style.transform = 'none'; }, 400);
    sendToCpp('rescan_peers');
  });
}

// 2. Manual Sending (🎯 Modal)
const btnOpenManualSend = document.getElementById('btn-open-manual-send');
const btnConfirmManualSend = document.getElementById('btn-confirm-manual-send');
const manualIpInput = document.getElementById('manual-ip-input');
const manualPortInput = document.getElementById('manual-port-input');

if (btnOpenManualSend) {
  btnOpenManualSend.addEventListener('click', () => {
    window.openModal('modal-manual-send');
    setTimeout(() => { manualIpInput?.focus(); }, 100);
  });
}

if (btnConfirmManualSend) {
  btnConfirmManualSend.addEventListener('click', () => {
    const ip = (manualIpInput?.value || '').trim();
    const port = parseInt(manualPortInput?.value, 10) || 53317;

    if (!ip) {
      alert('Please enter a target IP address (e.g. 192.168.1.100)');
      return;
    }
    if (selectedFiles.length === 0) {
      alert('Please select files to send first!');
      return;
    }

    window.sendToPeer(ip, port);
    window.closeModal('modal-manual-send');
  });
}

// 3. Share via Link (🔗 Local Web Portal Modal)
const btnOpenShareLink = document.getElementById('btn-open-share-link');
const btnReceiveViewPortal = document.getElementById('btn-receive-view-portal');
const btnModalOpenBrowser = document.getElementById('btn-modal-open-browser');
const btnModalCopyLink = document.getElementById('btn-modal-copy-link');
const modalLocalUrlText = document.getElementById('modal-local-url-text');
const localModalQrcode = document.getElementById('local-modal-qrcode');

function updateShareLinkModal() {
  const ip = (localIps && localIps.length > 0) ? localIps[0] : '127.0.0.1';
  const port = currentSettings.port || 53317;
  const localUrl = `http://${ip}:${port}`;

  if (modalLocalUrlText) modalLocalUrlText.innerText = localUrl;

  if (localModalQrcode) {
    localModalQrcode.innerHTML = '';
    if (typeof QRCode !== 'undefined') {
      localQrInstance = new QRCode(localModalQrcode, {
        text: localUrl,
        width: 170,
        height: 170,
        colorDark: '#030712',
        colorLight: '#ffffff',
        correctLevel: QRCode.CorrectLevel.M
      });
    }
  }
}

if (btnOpenShareLink) {
  btnOpenShareLink.addEventListener('click', () => {
    updateShareLinkModal();
    window.openModal('modal-share-link');
  });
}

if (btnReceiveViewPortal) {
  btnReceiveViewPortal.addEventListener('click', () => {
    updateShareLinkModal();
    window.openModal('modal-share-link');
  });
}

if (btnModalOpenBrowser) {
  btnModalOpenBrowser.addEventListener('click', () => {
    const ip = (localIps && localIps.length > 0) ? localIps[0] : '127.0.0.1';
    const localUrl = `http://${ip}:${currentSettings.port || 53317}`;
    sendToCpp('open_url', { url: localUrl });
  });
}

if (btnModalCopyLink) {
  btnModalCopyLink.addEventListener('click', () => {
    const ip = (localIps && localIps.length > 0) ? localIps[0] : '127.0.0.1';
    const localUrl = `http://${ip}:${currentSettings.port || 53317}`;
    navigator.clipboard.writeText(localUrl);
    btnModalCopyLink.innerText = 'Copied! ✓';
    setTimeout(() => { btnModalCopyLink.innerText = 'Copy Link'; }, 1800);
  });
}

// 4. Remote WebRTC Share (🌐 Room Code Sender Modal)
const btnOpenRemoteShare = document.getElementById('btn-open-remote-share');
const btnModalStartRoom = document.getElementById('btn-modal-start-room');
const btnModalCopyRoomLink = document.getElementById('btn-modal-copy-room-link');
const remoteShareIdleView = document.getElementById('remote-share-idle-view');
const remoteShareActiveView = document.getElementById('remote-share-active-view');
const modalRoomCodeDisplay = document.getElementById('modal-room-code-display');
const modalRoomUrlText = document.getElementById('modal-room-url-text');
const remoteModalQrcode = document.getElementById('remote-modal-qrcode');
const modalRoomStatus = document.getElementById('modal-room-status');

if (btnOpenRemoteShare) {
  btnOpenRemoteShare.addEventListener('click', () => {
    window.openModal('modal-remote-share');
  });
}

if (btnModalStartRoom) {
  btnModalStartRoom.addEventListener('click', () => {
    if (selectedFileObjects.length === 0 && selectedFiles.length === 0) {
      if (filePicker) filePicker.click();
      return;
    }
    startWebRTCShareSession();
  });
}

if (btnModalCopyRoomLink) {
  btnModalCopyRoomLink.addEventListener('click', () => {
    if (currentSenderCode) {
      const url = `https://share.sunilshahid.com/?code=${currentSenderCode}`;
      navigator.clipboard.writeText(url);
      btnModalCopyRoomLink.innerText = 'Copied! ✓';
      setTimeout(() => { btnModalCopyRoomLink.innerText = 'Copy Direct Link'; }, 1800);
    }
  });
}

// 5. Receive via Room Code Modal (Receiver)
const btnOpenCodeModal = document.getElementById('btn-open-code-modal');
const modalInputReceiveCode = document.getElementById('modal-input-receive-code');
const btnModalConfirmReceive = document.getElementById('btn-modal-confirm-receive');
const modalReceiveCodeStatus = document.getElementById('modal-receive-code-status');

if (btnOpenCodeModal) {
  btnOpenCodeModal.addEventListener('click', () => {
    window.openModal('modal-receive-code');
    setTimeout(() => { modalInputReceiveCode?.focus(); }, 100);
  });
}

if (modalInputReceiveCode) {
  modalInputReceiveCode.addEventListener('input', (e) => {
    let val = e.target.value.toUpperCase().replace(/[^A-Z0-9-]/g, '');
    if (!val.includes('-') && val.length > 3) {
      val = `${val.slice(0, 3)}-${val.slice(3, 6)}`;
    }
    if (val.length > 7) val = val.slice(0, 7);
    modalInputReceiveCode.value = val;
  });
}

if (btnModalConfirmReceive) {
  btnModalConfirmReceive.addEventListener('click', () => {
    const code = (modalInputReceiveCode?.value || '').trim().toUpperCase();
    if (code.length < 6) {
      alert('Please enter a valid 6-digit room code (e.g. 123-456)');
      return;
    }
    if (modalReceiveCodeStatus) {
      modalReceiveCodeStatus.style.display = 'block';
      modalReceiveCodeStatus.innerText = `Connecting to room [${code}]...`;
    }
    startWebRTCReceiverSession(code);
    setTimeout(() => {
      window.closeModal('modal-receive-code');
    }, 600);
  });
}

// ========================================================
// 📊 LIVE IN-APP RECEIVING & SENDING PANELS (On-Screen)
// ========================================================
const receiveLivePanel = document.getElementById('receive-live-panel');
const receiveLiveTitle = document.getElementById('receive-live-title');
const receiveLiveFile = document.getElementById('receive-live-file');
const receiveLivePct = document.getElementById('receive-live-pct');
const receiveLiveBytes = document.getElementById('receive-live-bytes');
const receiveLiveFill = document.getElementById('receive-live-fill');
const receiveLiveSpeed = document.getElementById('receive-live-speed');
const receiveLiveEta = document.getElementById('receive-live-eta');
const btnReceiveCancelTransfer = document.getElementById('btn-receive-cancel-transfer');

const sendLivePanel = document.getElementById('send-live-panel');
const sendLiveTitle = document.getElementById('send-live-title');
const sendLiveFile = document.getElementById('send-live-file');
const sendLivePct = document.getElementById('send-live-pct');
const sendLiveBytes = document.getElementById('send-live-bytes');
const sendLiveFill = document.getElementById('send-live-fill');
const sendLiveSpeed = document.getElementById('send-live-speed');
const sendLiveEta = document.getElementById('send-live-eta');
const btnSendCancelTransfer = document.getElementById('btn-send-cancel-transfer');

function updateLiveTransferUI(p) {
  clearTimeout(livePanelHideTimeout);

  const pct = Math.min(100, Math.max(0, p.percentage || 0));
  const bytesStr = `${formatBytes(p.transferredBytes)} / ${formatBytes(p.totalBytes)}`;
  const speedStr = `${(p.speedMBs || 0).toFixed(1)} MB/s`;
  const etaStr = formatTime(p.etaSeconds);

  // Update Receive Panel
  if (receiveLivePanel) {
    receiveLivePanel.style.display = 'block';
    if (receiveLiveTitle && p.title) receiveLiveTitle.innerText = p.title;
    if (receiveLiveFile && p.fileName) receiveLiveFile.innerText = p.fileName;
    if (receiveLivePct) receiveLivePct.innerText = `${pct}%`;
    if (receiveLiveFill) receiveLiveFill.style.width = `${pct}%`;
    if (receiveLiveBytes) receiveLiveBytes.innerText = bytesStr;
    if (receiveLiveSpeed) receiveLiveSpeed.innerText = speedStr;
    if (receiveLiveEta) receiveLiveEta.innerText = etaStr;
  }

  // Update Send Panel
  if (sendLivePanel) {
    sendLivePanel.style.display = 'block';
    if (sendLiveTitle && p.title) sendLiveTitle.innerText = p.title;
    if (sendLiveFile && p.fileName) sendLiveFile.innerText = p.fileName;
    if (sendLivePct) sendLivePct.innerText = `${pct}%`;
    if (sendLiveFill) sendLiveFill.style.width = `${pct}%`;
    if (sendLiveBytes) sendLiveBytes.innerText = bytesStr;
    if (sendLiveSpeed) sendLiveSpeed.innerText = speedStr;
    if (sendLiveEta) sendLiveEta.innerText = etaStr;
  }
}

function completeLiveTransferUI(title, totalBytes) {
  if (receiveLiveTitle) receiveLiveTitle.innerText = 'Transfer Complete! 🎉';
  if (receiveLivePct) receiveLivePct.innerText = '100%';
  if (receiveLiveFill) receiveLiveFill.style.width = '100%';
  if (receiveLiveEta) receiveLiveEta.innerText = 'Done';

  if (sendLiveTitle) sendLiveTitle.innerText = 'Transfer Complete! 🎉';
  if (sendLivePct) sendLivePct.innerText = '100%';
  if (sendLiveFill) sendLiveFill.style.width = '100%';
  if (sendLiveEta) sendLiveEta.innerText = 'Done';

  addTransferHistoryItem(title || 'File Transfer', totalBytes || 0, 'Success');

  clearTimeout(livePanelHideTimeout);
  livePanelHideTimeout = setTimeout(() => {
    if (receiveLivePanel) receiveLivePanel.style.display = 'none';
    if (sendLivePanel) sendLivePanel.style.display = 'none';
  }, 4000);
}

function errorLiveTransferUI(message) {
  if (receiveLiveTitle) receiveLiveTitle.innerText = message || 'Transfer Cancelled';
  if (sendLiveTitle) sendLiveTitle.innerText = message || 'Transfer Cancelled';

  clearTimeout(livePanelHideTimeout);
  livePanelHideTimeout = setTimeout(() => {
    if (receiveLivePanel) receiveLivePanel.style.display = 'none';
    if (sendLivePanel) sendLivePanel.style.display = 'none';
  }, 2500);
}

if (btnReceiveCancelTransfer) {
  btnReceiveCancelTransfer.addEventListener('click', () => {
    sendToCpp('cancel_transfer');
    stopWebRTCShareSession();
    if (receiveLiveTitle) receiveLiveTitle.innerText = 'Cancelling transfer...';
    if (sendLiveTitle) sendLiveTitle.innerText = 'Cancelling transfer...';
    setTimeout(() => {
      if (receiveLivePanel) receiveLivePanel.style.display = 'none';
      if (sendLivePanel) sendLivePanel.style.display = 'none';
    }, 1000);
  });
}

if (btnSendCancelTransfer) {
  btnSendCancelTransfer.addEventListener('click', () => {
    sendToCpp('cancel_transfer');
    stopWebRTCShareSession();
    if (sendLiveTitle) sendLiveTitle.innerText = 'Cancelling transfer...';
    if (receiveLiveTitle) receiveLiveTitle.innerText = 'Cancelling transfer...';
    setTimeout(() => {
      if (sendLivePanel) sendLivePanel.style.display = 'none';
      if (receiveLivePanel) receiveLivePanel.style.display = 'none';
    }, 1000);
  });
}

// Request confirmation handlers
const btnRequestAccept = document.getElementById('btn-request-accept');
if (btnRequestAccept) {
  btnRequestAccept.addEventListener('click', () => {
    sendToCpp('accept_transfer');
    window.closeModal('modal-transfer-request');
  });
}

const btnRequestDecline = document.getElementById('btn-request-decline');
if (btnRequestDecline) {
  btnRequestDecline.addEventListener('click', () => {
    sendToCpp('reject_transfer');
    window.closeModal('modal-transfer-request');
  });
}

function addTransferHistoryItem(title, size, status) {
  const hist = document.getElementById('transfer-history');
  if (!hist) return;
  if (hist.innerText.includes('No recent')) hist.innerHTML = '';
  const item = document.createElement('div');
  item.className = 'history-row';
  const sizeText = size > 0 ? ` — ${formatBytes(size)}` : '';
  item.innerHTML = `
    <span class="history-name">${title}${sizeText}</span>
    <span class="history-status">${status}</span>
  `;
  hist.prepend(item);
}

// ========================================================
// 📡 UDP RADAR & PEER DISCOVERY
// ========================================================
function renderPeers() {
  const grid = document.getElementById('peer-grid');
  if (!grid) return;
  grid.innerHTML = '';

  if (!activePeers || activePeers.length === 0) {
    grid.innerHTML = `
      <div class="radar-empty-state">
        <div class="radar-scan-ring">
          <span class="radar-scan-dot"></span>
        </div>
        <div class="radar-empty-text">Scanning for nearby BlackHole devices on your Wi-Fi...</div>
        <div class="radar-troubleshoot-link" id="link-radar-troubleshoot">
          Please ensure the desired target is also on the same Wi-Fi network.
        </div>
      </div>
    `;
    return;
  }

  activePeers.forEach(peer => {
    const card = document.createElement('div');
    card.className = 'peer-card';

    card.innerHTML = `
      <div style="display: flex; align-items: center; gap: 12px;">
        <div style="width: 38px; height: 38px; border-radius: 10px; background: rgba(139,92,246,0.15); display: flex; align-items: center; justify-content: center; color: #a855f7;">
          <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect width="14" height="20" x="5" y="2" rx="2" ry="2"/><path d="M12 18h.01"/></svg>
        </div>
        <div class="peer-info">
          <h4>${peer.alias || 'Device'}</h4>
          <span>${peer.ip}:${peer.port}</span>
        </div>
      </div>
      <button class="btn-primary-gradient" style="padding: 7px 16px; font-size: 0.82rem;" onclick="sendToPeer('${peer.ip}', ${peer.port}, '${(peer.alias || '').replace(/'/g, "\\'")}')">
        Send
      </button>
    `;
    grid.appendChild(card);
  });
}

window.sendToPeer = function(peerIp, peerPort, peerAlias) {
  if (selectedFiles.length === 0) {
    alert('Please select files first using the selection grid above!');
    return;
  }
  const targetName = peerAlias || peerIp;
  const filePaths = selectedFiles.map(f => f.path);
  sendToCpp('send_files', { peerIp, peerPort, peerAlias: targetName, files: filePaths });

  // Display sending progress right away
  updateLiveTransferUI({
    title: `Sending to ${targetName}...`,
    fileName: 'Waiting for receiver to accept...',
    percentage: 0,
    transferredBytes: 0,
    totalBytes: selectedFiles.reduce((acc, f) => acc + (f.size || 0), 0),
    speedMBs: 0,
    etaSeconds: 0
  });
};

// ========================================================
// 🌐 WEBRTC ROOM CODE SHARING (SENDER)
// ========================================================
let p2pEstablished = false;
let relayFallbackTimeout = null;

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
  if (remoteShareActiveView) remoteShareActiveView.style.display = 'none';
  if (remoteShareIdleView) remoteShareIdleView.style.display = 'block';
}

function startWebRTCShareSession() {
  stopWebRTCShareSession();
  senderCancelled = false;

  if (remoteShareIdleView) remoteShareIdleView.style.display = 'none';
  if (remoteShareActiveView) remoteShareActiveView.style.display = 'block';
  if (modalRoomCodeDisplay) modalRoomCodeDisplay.innerText = '...';
  if (modalRoomStatus) modalRoomStatus.innerText = 'Connecting to signaling network...';

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
            completeLiveTransferUI('Web Client', selectedFiles.reduce((acc, f) => acc + (f.size || 0), 0));
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
        if (modalRoomCodeDisplay) modalRoomCodeDisplay.innerText = msg.code;
        const shareUrl = `https://share.sunilshahid.com/?code=${msg.code}`;
        if (modalRoomUrlText) modalRoomUrlText.innerText = shareUrl;
        if (modalRoomStatus) modalRoomStatus.innerText = 'Waiting for recipient to connect...';

        // Render QR Code
        if (remoteModalQrcode) {
          remoteModalQrcode.innerHTML = '';
          if (typeof QRCode !== 'undefined') {
            remoteQrInstance = new QRCode(remoteModalQrcode, {
              text: shareUrl,
              width: 170,
              height: 170,
              colorDark: '#030712',
              colorLight: '#ffffff',
              correctLevel: QRCode.CorrectLevel.M
            });
          }
        }

        initWebRTCSender(msg.code, ws, localIps);
      }
      else if (msg.type === 'peer-joined') {
        if (modalRoomStatus) modalRoomStatus.innerText = 'Receiver connected! Opening direct tunnel...';

        if (ws.readyState === WebSocket.OPEN && localIps.length > 0) {
          ws.send(JSON.stringify({ type: 'signal', data: { peerLocalIps: localIps } }));
        }

        if (webrtcSenderManager) {
          const offer = await webrtcSenderManager.createOffer();
          ws.send(JSON.stringify({ type: 'signal', data: { sdp: offer, peerLocalIps: localIps } }));
        }

        clearTimeout(relayFallbackTimeout);
        relayFallbackTimeout = setTimeout(() => {
          if (!p2pEstablished && !isSenderTransferring) {
            startRelaySender(currentSenderCode);
          }
        }, 12000);
      }
      else if (msg.type === 'signal') {
        if (msg.data?.peerLocalIps) {
          webrtcSenderManager?.setRemoteLocalIps(msg.data.peerLocalIps);
        }

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
        if (modalRoomStatus) modalRoomStatus.innerText = 'Receiver disconnected.';
      }
    };

    ws.onerror = () => {
      if (modalRoomStatus) modalRoomStatus.innerText = 'Failed to connect to signaling server.';
    };
  } catch (err) {
    if (modalRoomStatus) modalRoomStatus.innerText = 'Error: ' + (err.message || err);
  }
}

function initWebRTCSender(code, ws, myIps) {
  const webrtc = new WebRTCManager(true, {
    onConnectionModeChange: () => {},
    onSignal: (data) => {
      if (ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ type: 'signal', data }));
      }
    },
    onDataChannelOpen: async () => {
      p2pEstablished = true;
      clearTimeout(relayFallbackTimeout);
      if (modalRoomStatus) modalRoomStatus.innerText = 'Secure tunnel open! Streaming files...';

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
  }, myIps);

  webrtc.init();
  webrtcSenderManager = webrtc;
}

async function startWebRTCDataTransfer(code, transport) {
  isSenderTransferring = true;
  window.closeModal('modal-remote-share');

  try {
    const cryptoKey = await deriveKeyFromSecret(code);
    await sendFilesWebRTC(
      selectedFileObjects,
      transport,
      cryptoKey,
      (p) => {
        const speedMB = (p.speedBytesPerSec / (1024 * 1024));
        updateLiveTransferUI({
          title: 'Sending via WebRTC...',
          fileName: p.currentFileName,
          percentage: p.percentage,
          transferredBytes: p.transferredBytes,
          totalBytes: p.totalBytes,
          speedMBs: speedMB,
          etaSeconds: p.etaSeconds
        });
      },
      () => senderCancelled
    );

    completeLiveTransferUI('Web Client', selectedFiles.reduce((acc, f) => acc + (f.size || 0), 0));
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

  window.closeModal('modal-remote-share');
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
        const speedMB = (p.speedBytesPerSec / (1024 * 1024));
        updateLiveTransferUI({
          title: 'Streaming via Encrypted Relay...',
          fileName: p.currentFileName,
          percentage: p.percentage,
          transferredBytes: p.transferredBytes,
          totalBytes: p.totalBytes,
          speedMBs: speedMB,
          etaSeconds: p.etaSeconds
        });
      },
      () => senderCancelled
    );

    completeLiveTransferUI('Web Client (Relay)', selectedFiles.reduce((acc, f) => acc + (f.size || 0), 0));
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
function startWebRTCReceiverSession(code) {
  try {
    const ws = new WebSocket(SIGNALING_WS_URL);
    ws.binaryType = 'arraybuffer';
    webrtcReceiverWS = ws;

    const packetQueue = [];

    deriveKeyFromSecret(code).then(cryptoKey => {
      receiverEngine = new FileReceiverWebRTC(
        cryptoKey,
        (p) => {
          const speedMB = (p.speedBytesPerSec / (1024 * 1024));
          updateLiveTransferUI({
            title: `Receiving from [${code}]...`,
            fileName: p.currentFileName,
            percentage: p.percentage,
            transferredBytes: p.transferredBytes,
            totalBytes: p.totalBytes,
            speedMBs: speedMB,
            etaSeconds: p.etaSeconds
          });
        },
        (file) => {
          triggerFileSave(file);
        },
        () => {
          completeLiveTransferUI(`Sender [${code}]`, 0);
          try {
            const ack = encodeControlPacket({ type: 'TRANSFER_ACK' });
            if (webrtcReceiverManager) webrtcReceiverManager.sendPacket(ack).catch(() => {});
            if (ws.readyState === WebSocket.OPEN) ws.send(ack.buffer);
          } catch {}
        }
      );
    });

    const webrtc = new WebRTCManager(false, {
      onConnectionModeChange: () => {},
      onSignal: (data) => {
        if (ws.readyState === WebSocket.OPEN) {
          ws.send(JSON.stringify({ type: 'signal', data }));
        }
      },
      onDataChannelOpen: () => {
        updateLiveTransferUI({
          title: `Connected to [${code}]! Receiving...`,
          fileName: 'Connecting...',
          percentage: 0,
          transferredBytes: 0,
          totalBytes: 0,
          speedMBs: 0,
          etaSeconds: 0
        });
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
      ws.send(JSON.stringify({ type: 'join-room', code }));
    };

    ws.onmessage = async (event) => {
      if (event.data instanceof ArrayBuffer) {
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
        if (ws.readyState === WebSocket.OPEN && localIps.length > 0) {
          ws.send(JSON.stringify({ type: 'signal', data: { peerLocalIps: localIps } }));
        }
      }
      else if (msg.type === 'signal') {
        if (msg.data?.peerLocalIps) {
          webrtc.setRemoteLocalIps(msg.data.peerLocalIps);
        }
        if (msg.data?.sdp) {
          if (msg.data.sdp.type === 'offer') {
            const answer = await webrtc.createAnswer(msg.data.sdp);
            ws.send(JSON.stringify({ type: 'signal', data: { sdp: answer, peerLocalIps: localIps } }));
          }
        } else if (msg.data?.candidate) {
          await webrtc.handleCandidate(msg.data.candidate);
        }
      }
      else if (msg.type === 'error') {
        alert(msg.message || 'Room error');
      }
    };
  } catch (err) {
    alert('Receiver error: ' + (err.message || err));
  }
}

function triggerFileSave(receivedFile) {
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

  addTransferHistoryItem(receivedFile.metadata.name, receivedFile.metadata.size, 'Success');
}

// ========================================================
// ⚙️ SETTINGS & LOCAL SERVER SYNC
// ========================================================
function updateLocalServerDisplay() {
  const ip = (localIps && localIps.length > 0) ? localIps[0] : '127.0.0.1';
  const port = currentSettings.port || 53317;
  const localUrl = `http://${ip}:${port}`;

  const headerIp = document.getElementById('header-ip');
  if (headerIp) headerIp.innerText = ip;

  const sidebarAlias = document.getElementById('sidebar-alias');
  if (sidebarAlias) sidebarAlias.innerText = currentSettings.alias;

  const receiveAliasLabel = document.getElementById('receive-alias-label');
  if (receiveAliasLabel) receiveAliasLabel.innerText = currentSettings.alias;

  const receiveStatusText = document.getElementById('receive-status-text');
  if (receiveStatusText) receiveStatusText.innerText = `Ready to receive on port ${port}`;

  const receivePortalLinkLabel = document.getElementById('receive-portal-link-label');
  if (receivePortalLinkLabel) receivePortalLinkLabel.innerText = localUrl;

  const receiveFolderDisplay = document.getElementById('receive-folder-display');
  if (receiveFolderDisplay) receiveFolderDisplay.innerText = currentSettings.destinationFolder || 'Downloads\\BlackHole';
}

// Browse destination folder
const btnBrowseDest = document.getElementById('btn-browse-dest');
if (btnBrowseDest) {
  btnBrowseDest.addEventListener('click', () => {
    sendToCpp('browse_folder');
  });
}

// Open received folder
const btnOpenDownloadDir = document.getElementById('btn-open-download-dir');
if (btnOpenDownloadDir) {
  btnOpenDownloadDir.addEventListener('click', () => {
    sendToCpp('open_folder', { path: currentSettings.destinationFolder });
  });
}

// Save settings button
const btnSaveSettings = document.getElementById('btn-save-settings');
if (btnSaveSettings) {
  btnSaveSettings.addEventListener('click', () => {
    const updated = {
      alias: document.getElementById('setting-alias')?.value.trim() || 'BlackHole-PC',
      destinationFolder: document.getElementById('setting-dest-folder')?.value.trim(),
      quickSave: document.getElementById('setting-quick-save')?.checked ?? true,
      port: parseInt(document.getElementById('setting-port')?.value, 10) || 53317,
      encryptionEnabled: document.getElementById('setting-encryption')?.checked ?? true
    };

    currentSettings = updated;
    sendToCpp('save_settings', { data: updated });
    updateLocalServerDisplay();

    btnSaveSettings.innerText = 'Saved! ✓';
    btnSaveSettings.style.background = '#10b981';
    setTimeout(() => {
      btnSaveSettings.innerText = 'Save Settings';
      btnSaveSettings.style.background = '';
    }, 1800);
  });
}

// ========================================================
// 📬 MESSAGES FROM NATIVE C++ BACKEND
// ========================================================
if (window.chrome && window.chrome.webview) {
  window.chrome.webview.addEventListener('message', (event) => {
    const data = typeof event.data === 'string' ? JSON.parse(event.data) : event.data;

    if (data.type === 'init_data') {
      if (data.settings) {
        currentSettings = data.settings;
        const setAlias = document.getElementById('setting-alias');
        if (setAlias) setAlias.value = currentSettings.alias;
        const setDest = document.getElementById('setting-dest-folder');
        if (setDest) setDest.value = currentSettings.destinationFolder;
        const setQuick = document.getElementById('setting-quick-save');
        if (setQuick) setQuick.checked = currentSettings.quickSave;
        const setPort = document.getElementById('setting-port');
        if (setPort) setPort.value = currentSettings.port;
        const setEnc = document.getElementById('setting-encryption');
        if (setEnc) setEnc.checked = currentSettings.encryptionEnabled;
      }
      if (data.localIps && data.localIps.length > 0) {
        localIps = data.localIps;
      }
      updateLocalServerDisplay();
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
      const setDest = document.getElementById('setting-dest-folder');
      if (setDest) setDest.value = data.path;
      updateLocalServerDisplay();
      sendToCpp('save_settings', { data: currentSettings });
    }
    else if (data.type === 'transfer_event') {
      const ev = data;
      if (ev.eventType === 'incoming_request') {
        // Desktop notification if minimized or backgrounded
        if (window.Notification && Notification.permission === 'granted') {
          try {
            new Notification(`Incoming Transfer: ${ev.peerAlias || 'Nearby Device'}`, {
              body: `${ev.fileCount || 1} file(s) (${formatBytes(ev.totalBytes || 0)})\nClick to view and accept.`,
              icon: 'logo.svg'
            });
          } catch {}
        } else if (window.Notification && Notification.permission !== 'denied') {
          try { Notification.requestPermission(); } catch {}
        }

        const reqAlias = document.getElementById('request-sender-alias');
        if (reqAlias) reqAlias.innerText = ev.peerAlias || 'Nearby Device';

        const reqIp = document.getElementById('request-sender-ip');
        if (reqIp) reqIp.innerText = ev.peerIp || '';

        const reqCount = document.getElementById('request-files-count');
        if (reqCount) reqCount.innerText = `${ev.fileCount || 1} file(s)`;

        const reqTotal = document.getElementById('request-total-size');
        if (reqTotal) reqTotal.innerText = formatBytes(ev.totalBytes || 0);

        const reqPreview = document.getElementById('request-files-preview');
        if (reqPreview) {
          reqPreview.innerHTML = '';
          const fnames = ev.fileNames || [];
          if (fnames.length > 0) {
            fnames.forEach(fn => {
              const item = document.createElement('div');
              item.className = 'request-file-item';
              item.innerHTML = `<span>📄</span> <span>${fn}</span>`;
              reqPreview.appendChild(item);
            });
          } else {
            reqPreview.innerHTML = `<div class="request-file-item"><span>📄</span> <span>Incoming file(s)</span></div>`;
          }
        }

        window.openModal('modal-transfer-request');
      }
      else if (ev.eventType === 'progress') {
        const isReceiving = ev.peerAlias && ev.peerAlias.toLowerCase().includes('receiving');

        // Automatically switch to Receive tab if incoming file from mobile or peer
        if (isReceiving) {
          const receiveNavBtn = document.querySelector('.nav-btn[data-tab="receive"]');
          if (receiveNavBtn && !receiveNavBtn.classList.contains('active')) {
            receiveNavBtn.click();
          }
        }

        const remainingBytes = (ev.totalBytes || 0) - (ev.bytesTransferred || 0);
        const speedBytes = (ev.speedMBs || 0) * 1024 * 1024;
        const etaSec = (speedBytes > 0 && remainingBytes > 0) ? Math.ceil(remainingBytes / speedBytes) : 0;

        updateLiveTransferUI({
          title: ev.peerAlias || (isReceiving ? 'Receiving File...' : 'Sending File...'),
          fileName: ev.currentFileName || 'Transferring...',
          percentage: ev.progressPercent,
          transferredBytes: ev.bytesTransferred,
          totalBytes: ev.totalBytes,
          speedMBs: ev.speedMBs,
          etaSeconds: etaSec
        });
      }
      else if (ev.eventType === 'complete') {
        window.closeModal('modal-transfer-request');
        completeLiveTransferUI(ev.currentFileName || ev.peerAlias || 'File Transfer', ev.totalBytes);
      }
      else if (ev.eventType === 'error') {
        window.closeModal('modal-transfer-request');
        errorLiveTransferUI(ev.message || 'Transfer interrupted');
      }
    }
  });
}

// Request Initial Data on Load & Setup UI handlers
window.addEventListener('DOMContentLoaded', () => {
  sendToCpp('get_init_data');
  renderSelectedFiles();

  // Smooth mouse-wheel horizontal scrolling for top action cards row
  const actionsRow = document.querySelector('.receive-actions-row');
  if (actionsRow) {
    actionsRow.addEventListener('wheel', (e) => {
      if (actionsRow.scrollWidth > actionsRow.clientWidth) {
        if (Math.abs(e.deltaY) > Math.abs(e.deltaX)) {
          e.preventDefault();
          actionsRow.scrollLeft += e.deltaY;
        }
      }
    }, { passive: false });
  }
});
