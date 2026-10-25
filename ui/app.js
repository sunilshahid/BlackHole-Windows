// BlackHole Windows Native Client UI Logic
// High-performance P2P UX + Cosmic Theme + Live In-App Receiving Display

// Candidate Signaling Endpoints (Public Cloudflare Tunnel + Local Dev Fallbacks)
function getSignalingCandidates() {
  const list = [];
  if (currentSettings.signalingUrl && currentSettings.signalingUrl.trim()) {
    list.push(currentSettings.signalingUrl.trim());
  }
  list.push('wss://share.sunilshahid.com/ws');
  list.push('ws://localhost:3001');
  if (Array.isArray(localIps)) {
    for (const ip of localIps) {
      if (ip && ip !== '127.0.0.1') {
        list.push(`ws://${ip}:3001`);
      }
    }
  }
  return Array.from(new Set(list));
}

const SIGNALING_HEALTH_URL = 'https://share.sunilshahid.com/health';

async function ensureSignalingAwake(onStatus, timeoutMs = 45000) {
  const startTime = Date.now();
  let attempt = 0;
  while (Date.now() - startTime < timeoutMs) {
    attempt++;
    const elapsedSec = Math.round((Date.now() - startTime) / 1000);
    if (onStatus) {
      if (attempt === 1) {
        onStatus('Connecting to secure signaling network...');
      } else {
        onStatus(`Waking up cloud server (${elapsedSec}s) - Render starting...`);
      }
    }

    try {
      const controller = new AbortController();
      const tId = setTimeout(() => controller.abort(), 5000);
      const res = await fetch(SIGNALING_HEALTH_URL, {
        method: 'GET',
        cache: 'no-store',
        mode: 'no-cors',
        signal: controller.signal
      });
      clearTimeout(tId);
      if (res.status === 200 || res.type === 'opaque') {
        if (onStatus) onStatus('Cloud server online! Connecting...');
        return true;
      }
    } catch (e) {
      // Server spinning up
    }
    await new Promise(r => setTimeout(r, 1500));
  }
  return false;
}

// Native File IPC Chunk Bridge
const pendingChunkRequests = new Map();
let chunkReqCounter = 0;

window.readNativeFileChunk = function(path, offset, length) {
  return new Promise((resolve, reject) => {
    const requestId = `read_${++chunkReqCounter}_${Date.now()}`;
    const timeout = setTimeout(() => {
      pendingChunkRequests.delete(requestId);
      reject(new Error(`Read timed out for ${path} at offset ${offset}`));
    }, 20000);

    pendingChunkRequests.set(requestId, {
      resolve: (data) => {
        clearTimeout(timeout);
        resolve(data);
      },
      reject: (err) => {
        clearTimeout(timeout);
        reject(err);
      }
    });

    sendToCpp('read_file_chunk', { path, offset, length, requestId });
  });
};

window.writeNativeFileChunk = function(filename, offset, chunkBuffer, isLast = false) {
  return new Promise((resolve, reject) => {
    const requestId = `write_${++chunkReqCounter}_${Date.now()}`;
    const timeout = setTimeout(() => {
      pendingChunkRequests.delete(requestId);
      reject(new Error(`Write timed out for ${filename} at offset ${offset}`));
    }, 20000);

    pendingChunkRequests.set(requestId, {
      resolve: (res) => {
        clearTimeout(timeout);
        if (res.success) resolve();
        else reject(new Error('Write chunk failed in native engine'));
      },
      reject: (err) => {
        clearTimeout(timeout);
        reject(err);
      }
    });

    // Convert chunkBuffer (ArrayBuffer) to base64
    const bytes = new Uint8Array(chunkBuffer);
    let binary = '';
    const len = bytes.byteLength;
    for (let i = 0; i < len; i++) {
      binary += String.fromCharCode(bytes[i]);
    }
    const dataBase64 = btoa(binary);

    sendToCpp('write_file_chunk', { filename, offset, dataBase64, isLast, requestId });
  });
};

function connectSignalingSocket(onStatus, onConnected, onFailed) {
  const candidates = getSignalingCandidates();
  let index = 0;
  let settled = false;
  let timer = null;

  function tryNext() {
    if (settled) return;
    if (index >= candidates.length) {
      settled = true;
      if (onFailed) onFailed(new Error('All signaling servers unreachable'));
      return;
    }

    const url = candidates[index++];
    if (onStatus) onStatus(`Connecting to signaling network (${url})...`);

    let ws;
    try {
      ws = new WebSocket(url);
      ws.binaryType = 'arraybuffer';
    } catch (e) {
      setTimeout(tryNext, 100);
      return;
    }

    timer = setTimeout(() => {
      if (!settled && ws.readyState !== WebSocket.OPEN) {
        try { ws.close(); } catch {}
        tryNext();
      }
    }, 15000);

    ws.onopen = () => {
      if (settled) {
        try { ws.close(); } catch {}
        return;
      }
      settled = true;
      clearTimeout(timer);
      if (onConnected) onConnected(ws, url);
    };

    ws.onerror = () => {
      if (!settled) {
        clearTimeout(timer);
        tryNext();
      }
    };
  }

  tryNext();
}

// Global State
let currentSettings = {
  alias: 'BlackHole-PC',
  destinationFolder: '',
  quickSaveMode: 0,
  quickSave: false,
  autoCopyText: true,
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
// 📁 FILE SELECTION & DRAG-AND-DROP
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

function getTextHistory() {
  try {
    const raw = localStorage.getItem('blackhole_text_history');
    return raw ? JSON.parse(raw) : [];
  } catch {
    return [];
  }
}

function saveTextToHistory(sender, text) {
  if (!text || !text.trim()) return;
  try {
    const history = getTextHistory();
    const filtered = history.filter(item => item.text !== text.trim());
    filtered.unshift({ sender: sender || 'Me', text: text.trim(), time: Date.now() });
    localStorage.setItem('blackhole_text_history', JSON.stringify(filtered.slice(0, 20)));
    renderTextHistory();
  } catch {}
}

function renderTextHistory() {
  const sec = document.getElementById('compose-text-history-section');
  const list = document.getElementById('compose-text-history-list');
  const input = document.getElementById('compose-text-input');
  if (!sec || !list) return;
  const history = getTextHistory();
  if (history.length === 0) {
    sec.style.display = 'none';
    return;
  }
  sec.style.display = 'block';
  list.innerHTML = '';
  history.forEach(item => {
    const chip = document.createElement('div');
    chip.style.cssText = 'background: rgba(255,255,255,0.06); border: 1px solid rgba(6,182,212,0.3); border-radius: 8px; padding: 4px 8px; font-size: 0.78rem; color: #e2e8f0; cursor: pointer; max-width: 200px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis;';
    chip.innerText = item.text;
    chip.title = item.text;
    chip.onclick = () => {
      if (input) input.value = item.text;
    };
    list.appendChild(chip);
  });
}

if (tileText) {
  tileText.addEventListener('click', () => {
    const input = document.getElementById('compose-text-input');
    if (input) input.value = '';
    renderTextHistory();
    window.openModal('modal-compose-text');
  });
}

const btnStageComposedText = document.getElementById('btn-stage-composed-text');
if (btnStageComposedText) {
  btnStageComposedText.addEventListener('click', () => {
    const input = document.getElementById('compose-text-input');
    const text = input ? input.value : '';
    if (text && text.trim().length > 0) {
      saveTextToHistory('Me', text);
      sendToCpp('save_note_file', { name: `note_${Date.now()}.txt`, content: text });
      window.closeModal('modal-compose-text');
    } else {
      alert('Please enter some text or link to stage.');
    }
  });
}

const btnPasteToCompose = document.getElementById('btn-paste-to-compose');
if (btnPasteToCompose) {
  btnPasteToCompose.addEventListener('click', () => {
    sendToCpp('get_clipboard', { target: 'compose' });
  });
}

const btnClearTextHistory = document.getElementById('btn-clear-text-history');
if (btnClearTextHistory) {
  btnClearTextHistory.addEventListener('click', () => {
    localStorage.removeItem('blackhole_text_history');
    renderTextHistory();
  });
}

if (tilePaste) {
  tilePaste.addEventListener('click', () => {
    sendToCpp('paste_clipboard', {});
  });
}

// Global Ctrl+V / Paste handling for files, images, and text
window.addEventListener('paste', (e) => {
  const activeTag = document.activeElement ? document.activeElement.tagName.toLowerCase() : '';
  if (activeTag !== 'input' && activeTag !== 'textarea') {
    e.preventDefault();
    sendToCpp('paste_clipboard', {});
  }
});

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

function getFileCategory(name, mime) {
  const n = (name || '').toLowerCase();
  const m = (mime || '').toLowerCase();
  if (m.startsWith('image/') || /\.(jpg|jpeg|png|webp|gif|bmp|svg)$/i.test(n)) return 'IMAGE';
  if (m.startsWith('video/') || /\.(mp4|mkv|mov|avi|webm|3gp)$/i.test(n)) return 'VIDEO';
  if (m.startsWith('text/') || /\.(txt|md|log|json|csv)$/i.test(n) || n.startsWith('shared_note_') || n.startsWith('note_')) return 'TEXT';
  if (/\.(apk|exe|msi)$/i.test(n)) return 'APP';
  return 'DOCUMENT';
}

function getCategoryIconSvg(cat) {
  switch (cat) {
    case 'IMAGE':
      return `<svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="var(--accent-green)" stroke-width="2"><rect width="18" height="18" x="3" y="3" rx="2" ry="2"/><circle cx="9" cy="9" r="2"/><path d="m21 15-3.086-3.086a2 2 0 0 0-2.828 0L6 21"/></svg>`;
    case 'VIDEO':
      return `<svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="var(--accent-violet)" stroke-width="2"><rect width="20" height="20" x="2" y="2" rx="2.18" ry="2.18"/><line x1="7" y1="2" x2="7" y2="22"/><line x1="17" y1="2" x2="17" y2="22"/><line x1="2" y1="12" x2="22" y2="12"/><line x1="2" y1="7" x2="7" y2="7"/><line x1="2" y1="17" x2="7" y2="17"/><line x1="17" y1="17" x2="22" y2="17"/><line x1="17" y1="7" x2="22" y2="7"/></svg>`;
    case 'TEXT':
      return `<svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="var(--accent-pink)" stroke-width="2"><line x1="21" y1="6" x2="3" y2="6"/><line x1="21" y1="12" x2="3" y2="12"/><line x1="17" y1="18" x2="3" y2="18"/></svg>`;
    case 'APP':
      return `<svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="var(--accent-cyan)" stroke-width="2"><rect width="7" height="7" x="3" y="3" rx="1"/><rect width="7" height="7" x="14" y="3" rx="1"/><rect width="7" height="7" x="14" y="14" rx="1"/><rect width="7" height="7" x="3" y="14" rx="1"/></svg>`;
    default:
      return `<svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="var(--accent-cyan)" stroke-width="2"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/></svg>`;
  }
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

  // Render preview tiles (Thumbnails, Video clapper, Doc, 3-line Text, App, and Add tile)
  if (compactFilesPreview) {
    compactFilesPreview.innerHTML = '';
    const sliceCount = Math.min(totalFiles, 6);
    for (let i = 0; i < sliceCount; i++) {
      const f = selectedFiles[i];
      const cat = getFileCategory(f.name, f.mimeType);
      const tile = document.createElement('div');
      tile.className = 'file-preview-tile';
      tile.title = `${f.name} (${formatBytes(f.size)})`;
      tile.onclick = () => window.openModal('modal-file-list');

      // Check if image object URL or local path
      let imgUrl = null;
      if (cat === 'IMAGE') {
        const fileObj = selectedFileObjects[i];
        if (fileObj instanceof Blob || fileObj instanceof File) {
          try { imgUrl = URL.createObjectURL(fileObj); } catch (_) {}
        } else if (f.path) {
          imgUrl = 'file:///' + f.path.replace(/\\/g, '/');
        }
      }

      if (imgUrl) {
        tile.innerHTML = `<img src="${imgUrl}" alt="${f.name}" onerror="this.onerror=null; this.parentNode.innerHTML = getCategoryIconSvg('${cat}') + '<div class=\\'tile-badge\\'>IMG</div>';" />`;
      } else {
        const badge = cat === 'VIDEO' ? 'VIDEO' : cat === 'TEXT' ? 'TXT' : cat === 'APP' ? 'APP' : (f.name.split('.').pop() || 'DOC').toUpperCase().slice(0, 4);
        tile.innerHTML = `
          <div class="tile-icon-box">${getCategoryIconSvg(cat)}</div>
          <div class="tile-badge">${badge}</div>
        `;
      }
      compactFilesPreview.appendChild(tile);
    }

    // Add More Tile
    const addTile = document.createElement('div');
    addTile.className = 'file-preview-tile tile-add-more';
    addTile.title = 'Add more files';
    addTile.onclick = () => sendToCpp('browse_files');
    addTile.innerHTML = `
      <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/></svg>
      <span>Add</span>
    `;
    compactFilesPreview.appendChild(addTile);
  }

  // Render detailed modal list items with category icons and individual delete buttons
  if (modalFileListItems) {
    modalFileListItems.innerHTML = '';
    selectedFiles.forEach((f, idx) => {
      const cat = getFileCategory(f.name, f.mimeType);
      const item = document.createElement('div');
      item.className = 'file-item';
      item.innerHTML = `
        <div style="display: flex; align-items: center; gap: 10px; overflow: hidden;">
          <div style="flex-shrink: 0; width: 34px; height: 34px; border-radius: 8px; background: rgba(255,255,255,0.06); display: flex; align-items: center; justify-content: center;">
            ${getCategoryIconSvg(cat)}
          </div>
          <div class="file-name" title="${f.name}" style="overflow: hidden; text-overflow: ellipsis; white-space: nowrap;">${f.name}</div>
        </div>
        <div style="display: flex; align-items: center; gap: 10px; flex-shrink: 0;">
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
      alert('Please enter a valid 6-character room code (e.g. 7K9-X2M)');
      return;
    }
    if (modalReceiveCodeStatus) {
      modalReceiveCodeStatus.style.display = 'block';
      modalReceiveCodeStatus.style.color = 'var(--accent-cyan)';
      modalReceiveCodeStatus.innerText = `Connecting to room [${code}]...`;
    }
    btnModalConfirmReceive.disabled = true;
    startWebRTCReceiverSession(code);
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

// DEDICATED FULL SCREEN TRANSFER VIEW CONTROLS
const fsTransferScreen = document.getElementById('screen-transfer-fullscreen');
const fsTransferTitle = document.getElementById('fs-transfer-title');
const fsTransferSubline = document.getElementById('fs-transfer-subline');
const fsTransferPeer = document.getElementById('fs-transfer-peer');
const fsFileName = document.getElementById('fs-file-name');
const fsFileSize = document.getElementById('fs-file-size');
const fsFileFill = document.getElementById('fs-file-fill');
const fsFileDone = document.getElementById('fs-file-done');
const fsTotalHeading = document.getElementById('fs-total-heading');
const fsLiveEta = document.getElementById('fs-live-eta');
const fsLivePct = document.getElementById('fs-live-pct');
const fsTotalFill = document.getElementById('fs-total-fill');
const btnFsToggleAdv = document.getElementById('btn-fs-toggle-advanced');
const fsAdvancedStats = document.getElementById('fs-advanced-stats');
const fsLiveSpeed = document.getElementById('fs-live-speed');
const fsLiveEtaAdv = document.getElementById('fs-live-eta-adv');
const btnFsCancelTransfer = document.getElementById('btn-fs-cancel-transfer');
const btnFsDoneTransfer = document.getElementById('btn-fs-done-transfer');

function updateLiveTransferUI(p) {
  clearTimeout(livePanelHideTimeout);
  window.closeModal('modal-sender-handshake');

  const pct = Math.min(100, Math.max(0, p.percentage || 0));
  const bytesStr = `${formatBytes(p.transferredBytes)} / ${formatBytes(p.totalBytes)}`;
  const speedStr = `${(p.speedMBs || 0).toFixed(1)} MB/s`;
  const etaStr = formatTime(p.etaSeconds);

  // Open Fullscreen Transfer View
  if (fsTransferScreen) {
    fsTransferScreen.style.display = 'flex';
    if (fsTransferTitle && p.title) fsTransferTitle.innerText = p.title;

    // Subline: show destination or save path cleanly
    const fsSubline = document.getElementById('fs-transfer-subline');
    if (fsSubline) {
      if (p.title && p.title.toLowerCase().includes('receiving')) {
        fsSubline.innerText = `Save to folder: ${currentSettings.destinationFolder || 'Downloads\\BlackHole'}`;
      } else {
        fsSubline.innerHTML = `To: <span class="dest-link">${p.peerAlias || 'Nearby Device'}</span>`;
      }
    }

    // Dynamic File Item Pinned to Top with Individual Progress Bar
    const container = document.getElementById('fs-files-container');
    const isPlaceholderFile = !p.fileName ||
      p.fileName === 'Connecting...' ||
      p.fileName === 'Transferring...' ||
      p.fileName === 'Preparing...' ||
      p.fileName === 'Waiting...' ||
      p.fileName.startsWith('Connecting') ||
      p.fileName.startsWith('Waiting for');

    // Remove default dummy element if still present
    const initialDummy = document.getElementById('fs-file-item');
    if (initialDummy) initialDummy.remove();

    if (container && p.fileName && !isPlaceholderFile) {
      const fileSize = p.fileBytes || p.totalBytes || 0;
      const safeId = 'fs-file-' + p.fileName.replace(/[^a-zA-Z0-9_-]/g, '_');
      let fileEl = document.getElementById(safeId);
      if (!fileEl) {
        fileEl = document.createElement('div');
        fileEl.className = 'live-file-item';
        fileEl.id = safeId;
        fileEl.innerHTML = `
          <div class="live-file-icon-box">
            <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/></svg>
          </div>
          <div class="live-file-details">
            <div class="live-file-name-row">
              <span class="live-file-name">${p.fileName}</span>
              <span class="live-file-size">${formatBytes(fileSize)}</span>
            </div>
            <div class="live-file-rail">
              <div class="live-file-bar" style="width: ${pct}%"></div>
            </div>
            <span class="live-file-done-tag" style="display: ${pct >= 100 ? 'block' : 'none'};">Done</span>
          </div>
        `;
        container.appendChild(fileEl);
      } else {
        const bar = fileEl.querySelector('.live-file-bar');
        const doneTag = fileEl.querySelector('.live-file-done-tag');
        const sizeEl = fileEl.querySelector('.live-file-size');
        if (bar) bar.style.width = `${pct}%`;
        if (sizeEl && fileSize > 0) sizeEl.innerText = formatBytes(fileSize);
        if (doneTag) doneTag.style.display = pct >= 100 ? 'block' : 'none';
      }
    }

    // Full Session Progress in Sticky Bottom Card
    if (fsTotalFill) fsTotalFill.style.width = `${pct}%`;
    if (fsLivePct) fsLivePct.innerText = `${pct}%`;
    if (fsLiveSpeed) fsLiveSpeed.innerText = speedStr;
    if (fsLiveEta) fsLiveEta.innerText = etaStr;
    if (fsLiveEtaAdv) fsLiveEtaAdv.innerText = etaStr;
    if (fsTotalHeading) fsTotalHeading.innerHTML = `Total Progress (${etaStr})`;
    if (btnFsCancelTransfer) btnFsCancelTransfer.style.display = 'inline-flex';
    if (btnFsDoneTransfer) btnFsDoneTransfer.style.display = 'none';
  }
}

function completeLiveTransferUI(title, totalBytes) {
  window.closeModal('modal-sender-handshake');

  if (fsTransferScreen) {
    const cont = document.getElementById('fs-files-container');
    if (cont) {
      const items = cont.querySelectorAll('.live-file-item');
      items.forEach(item => {
        const nameEl = item.querySelector('.live-file-name');
        const name = nameEl ? nameEl.innerText.trim() : '';
        if (!name || name === 'Connecting...' || name === 'Transferring...' || name === 'filename.ext' || name.startsWith('Connecting') || name.startsWith('Waiting')) {
          item.remove();
        }
      });
    }

    const doneTags = fsTransferScreen.querySelectorAll('.live-file-done-tag');
    doneTags.forEach(t => t.style.display = 'block');
    const bars = fsTransferScreen.querySelectorAll('.live-file-bar');
    bars.forEach(b => b.style.width = '100%');

    if (fsLivePct) fsLivePct.innerText = '100%';
    if (fsTotalFill) fsTotalFill.style.width = '100%';
    if (fsTotalHeading) fsTotalHeading.innerText = 'Total Progress (Done)';
    if (btnFsCancelTransfer) btnFsCancelTransfer.style.display = 'none';
    if (btnFsDoneTransfer) btnFsDoneTransfer.style.display = 'inline-flex';
  }
}

function errorLiveTransferUI(message) {
  window.closeModal('modal-sender-handshake');
  if (fsTransferScreen) fsTransferScreen.style.display = 'none';
}

// Done Button: dismisses full screen, resets selected files, and returns to previous tab!
if (btnFsDoneTransfer) {
  btnFsDoneTransfer.addEventListener('click', () => {
    if (fsTransferScreen) fsTransferScreen.style.display = 'none';
    const cont = document.getElementById('fs-files-container');
    if (cont) cont.innerHTML = '';
    selectedFiles = [];
    renderSelectedFiles();
  });
}

// Cancel Button: opens Cancel confirmation dialog
if (btnFsCancelTransfer) {
  btnFsCancelTransfer.addEventListener('click', () => {
    window.openModal('modal-cancel-confirm');
  });
}

const btnCancelTransferAbort = document.getElementById('btn-cancel-transfer-abort');
if (btnCancelTransferAbort) {
  btnCancelTransferAbort.addEventListener('click', () => {
    window.closeModal('modal-cancel-confirm');
    sendToCpp('cancel_transfer');
    stopWebRTCShareSession();
    stopWebRTCReceiverSession();
    if (fsTransferScreen) fsTransferScreen.style.display = 'none';
    const cont = document.getElementById('fs-files-container');
    if (cont) cont.innerHTML = '';
    selectedFiles = [];
    renderSelectedFiles();
  });
}

const btnCancelTransferContinue = document.getElementById('btn-cancel-transfer-continue');
if (btnCancelTransferContinue) {
  btnCancelTransferContinue.addEventListener('click', () => {
    window.closeModal('modal-cancel-confirm');
  });
}

// Toggle Advanced Stats
if (btnFsToggleAdv && fsAdvancedStats) {
  btnFsToggleAdv.addEventListener('click', () => {
    fsAdvancedStats.style.display = fsAdvancedStats.style.display === 'none' ? 'flex' : 'none';
  });
}

// Handshake Modal Cancel
const btnHandshakeCancel = document.getElementById('btn-handshake-cancel');
if (btnHandshakeCancel) {
  btnHandshakeCancel.addEventListener('click', () => {
    window.closeModal('modal-sender-handshake');
    sendToCpp('cancel_transfer');
    errorLiveTransferUI('Cancelled by user');
  });
}

// Options expander in Incoming Request Dialog
const btnToggleRequestOptions = document.getElementById('btn-toggle-request-options');
const requestOptionsPanel = document.getElementById('request-options-panel');
if (btnToggleRequestOptions && requestOptionsPanel) {
  btnToggleRequestOptions.addEventListener('click', () => {
    requestOptionsPanel.style.display = requestOptionsPanel.style.display === 'none' ? 'block' : 'none';
  });
}

// Message Copy Handler (Copies text and completes transfer for sender)
const btnCopyReceivedMsg = document.getElementById('btn-copy-received-msg');
const btnCopyReceivedMsgLbl = document.getElementById('btn-copy-received-msg-lbl');
const msgTextContent = document.getElementById('msg-text-content');
if (btnCopyReceivedMsg && msgTextContent) {
  btnCopyReceivedMsg.addEventListener('click', async () => {
    const textToCopy = msgTextContent.innerText || '';
    try {
      await navigator.clipboard.writeText(textToCopy);
      if (btnCopyReceivedMsgLbl) {
        btnCopyReceivedMsgLbl.innerText = 'Copied!';
      }
    } catch (e) {
      console.warn('Clipboard write fallback', e);
    }
    sendToCpp('set_clipboard', { text: textToCopy });
    // Notify sender with HTTP 204 acceptance!
    sendToCpp('accept_text_message');
    setTimeout(() => {
      window.closeModal('modal-message-received');
      if (btnCopyReceivedMsgLbl) btnCopyReceivedMsgLbl.innerText = 'Copy';
    }, 400);
  });
}

window.rejectTextMessageAndClose = function() {
  sendToCpp('reject_text_message');
  window.closeModal('modal-message-received');
};

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

// Request Options subview toggle
const btnToggleReqOptions = document.getElementById('btn-toggle-request-options');
if (btnToggleReqOptions) {
  btnToggleReqOptions.addEventListener('click', () => {
    const mainView = document.getElementById('request-main-view');
    const optView = document.getElementById('request-options-subview');
    if (mainView) mainView.style.display = 'none';
    if (optView) optView.style.display = 'block';
  });
}

const btnBackReqOptions = document.getElementById('btn-back-request-options');
if (btnBackReqOptions) {
  btnBackReqOptions.addEventListener('click', () => {
    const mainView = document.getElementById('request-main-view');
    const optView = document.getElementById('request-options-subview');
    if (optView) optView.style.display = 'none';
    if (mainView) mainView.style.display = 'block';
  });
}

const reqPin = document.getElementById('request-sender-pin');
if (reqPin) {
  reqPin.addEventListener('click', () => {
    const modalPin = document.getElementById('verify-modal-pin');
    if (modalPin) modalPin.innerText = reqPin.innerText;
    window.openModal('modal-sha-verify');
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
// 💖 FAVORITES SYSTEM
// ========================================================
function loadFavorites() {
  try {
    return JSON.parse(localStorage.getItem('blackhole_favorites') || '[]');
  } catch (e) {
    return [];
  }
}

function saveFavorites(favs) {
  try {
    localStorage.setItem('blackhole_favorites', JSON.stringify(favs));
    const fps = favs.map(f => f.fingerprint).filter(fp => fp && fp.length > 0);
    sendToCpp('update_favorites', { fingerprints: fps });
  } catch (e) {}
}

function toggleFavoritePeer(peer) {
  let favs = loadFavorites();
  const existingIndex = favs.findIndex(f => (f.fingerprint && peer.fingerprint && f.fingerprint === peer.fingerprint) || f.ip === peer.ip);
  if (existingIndex >= 0) {
    favs.splice(existingIndex, 1);
  } else {
    favs.push({
      id: peer.fingerprint || peer.ip,
      ip: peer.ip,
      port: peer.port || 53317,
      alias: peer.alias || 'Peer Device',
      customAlias: '',
      deviceType: peer.deviceType || 'phone',
      protocol: peer.protocol || 'blackhole',
      fingerprint: peer.fingerprint || ''
    });
  }
  saveFavorites(favs);
  renderPeers();
  renderFavoritesModal();
}

function renderFavoritesModal() {
  const container = document.getElementById('favorites-list-container');
  if (!container) return;
  const favs = loadFavorites();
  container.innerHTML = '';

  if (favs.length === 0) {
    container.innerHTML = `
      <div style="text-align: center; padding: 24px 12px; color: var(--text-muted); font-size: 0.9rem;">
        No favorite devices saved yet.<br>Click the heart icon on any nearby device to pin it!
      </div>
    `;
    return;
  }

  favs.forEach((fav, idx) => {
    const item = document.createElement('div');
    item.style.cssText = 'display: flex; align-items: center; justify-content: space-between; padding: 10px 14px; background: rgba(255, 255, 255, 0.04); border: 1px solid rgba(255, 255, 255, 0.08); border-radius: 12px;';
    const displayAlias = fav.customAlias || fav.alias;
    item.innerHTML = `
      <div style="display: flex; align-items: center; gap: 10px;">
        <span style="color: #f43f5e;">
          <svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor" stroke="currentColor" stroke-width="1.5"><path d="M19 14c1.49-1.46 3-3.21 3-5.5A5.5 5.5 0 0 0 16.5 3c-1.76 0-3 .5-4.5 2-1.5-1.5-2.74-2-4.5-2A5.5 5.5 0 0 0 2 8.5c0 2.3 1.5 4.05 3 5.5l7 7Z"/></svg>
        </span>
        <div>
          <div style="font-weight: 600; font-size: 0.95rem; color: #fff;">${displayAlias}</div>
          <div style="font-size: 0.78rem; color: var(--text-muted);">${fav.ip}:${fav.port} • <span style="text-transform: uppercase; font-size: 0.7rem; color: var(--accent-cyan); font-weight:600;">${fav.protocol || 'BlackHole'}</span></div>
        </div>
      </div>
      <div style="display: flex; align-items: center; gap: 8px;">
        <button class="btn-icon" style="color: var(--text-muted); padding: 5px;" title="Edit alias" onclick="editFavoriteAlias(${idx})">
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M17 3a2.828 2.828 0 1 1 4 4L7.5 20.5 2 22l1.5-5.5L17 3z"/></svg>
        </button>
        <button class="btn-icon" style="color: #ef4444; padding: 5px;" title="Remove" onclick="removeFavoriteByIndex(${idx})">
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
        </button>
        <button class="btn-pill-action" style="padding: 5px 12px; font-size: 0.8rem; background: var(--accent-purple); color: #fff;" onclick="sendToFavoriteByIndex(${idx})">
          Send
        </button>
      </div>
    `;
    container.appendChild(item);
  });
}

window.editFavoriteAlias = function(idx) {
  let favs = loadFavorites();
  if (idx >= 0 && idx < favs.length) {
    const current = favs[idx].customAlias || favs[idx].alias;
    const newName = prompt('Enter a custom alias for this favorite:', current);
    if (newName !== null) {
      favs[idx].customAlias = newName.trim();
      saveFavorites(favs);
      renderFavoritesModal();
      renderPeers();
    }
  }
};

window.removeFavoriteByIndex = function(idx) {
  let favs = loadFavorites();
  if (idx >= 0 && idx < favs.length) {
    favs.splice(idx, 1);
    saveFavorites(favs);
    renderFavoritesModal();
    renderPeers();
  }
};

window.sendToFavoriteByIndex = function(idx) {
  let favs = loadFavorites();
  if (idx >= 0 && idx < favs.length) {
    const fav = favs[idx];
    window.closeModal('modal-favorites');
    window.sendToPeer(fav.ip, fav.port, fav.customAlias || fav.alias, fav.protocol);
  }
};

// Wire Favorites Header Buttons
const btnOpenFavorites = document.getElementById('btn-open-favorites');
const btnReceiveFavorites = document.getElementById('btn-receive-favorites');
if (btnOpenFavorites) {
  btnOpenFavorites.addEventListener('click', () => {
    renderFavoritesModal();
    window.openModal('modal-favorites');
  });
}
if (btnReceiveFavorites) {
  btnReceiveFavorites.addEventListener('click', () => {
    renderFavoritesModal();
    window.openModal('modal-favorites');
  });
}

// ========================================================
// 📡 UDP RADAR & PEER DISCOVERY
// ========================================================
function renderPeers() {
  const grid = document.getElementById('peer-grid');
  if (!grid) return;
  grid.innerHTML = '';

  const countBadge = document.getElementById('peer-count-badge');
  if (countBadge) {
    if (activePeers && activePeers.length > 0) {
      countBadge.innerText = activePeers.length;
      countBadge.style.display = 'inline-block';
    } else {
      countBadge.style.display = 'none';
    }
  }

  if (!activePeers || activePeers.length === 0) {
    grid.className = 'peer-area peer-empty-radar';
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

  grid.className = 'peer-grid-active';

  const favs = loadFavorites();
  const isPeerFav = (p) => favs.some(f => (f.fingerprint && p.fingerprint && f.fingerprint === p.fingerprint) || f.ip === p.ip);

  const sortedPeers = [...activePeers].sort((a, b) => {
    const favA = isPeerFav(a) ? 1 : 0;
    const favB = isPeerFav(b) ? 1 : 0;
    if (favA !== favB) return favB - favA;
    return (a.alias || '').localeCompare(b.alias || '');
  });

  sortedPeers.forEach(peer => {
    const isFav = isPeerFav(peer);
    const isLs = peer.protocol === 'localsend';
    const protoBadge = isLs
      ? `<span class="peer-proto-badge badge-localsend" style="margin-left: 8px; font-size: 0.68rem; padding: 2px 7px; border-radius: 6px; font-weight: 700; background: rgba(6,182,212,0.18); color: #06b6d4; border: 1px solid rgba(6,182,212,0.35);">LocalSend</span>`
      : `<span class="peer-proto-badge badge-blackhole" style="margin-left: 8px; font-size: 0.68rem; padding: 2px 7px; border-radius: 6px; font-weight: 700; background: rgba(139,92,246,0.18); color: #a78bfa; border: 1px solid rgba(139,92,246,0.35);">BlackHole</span>`;
    const card = document.createElement('div');
    card.className = 'peer-card';
    card.style.cursor = 'pointer';

    card.innerHTML = `
      <div style="display: flex; align-items: center; justify-content: space-between; width: 100%;">
        <div style="display: flex; align-items: center; gap: 12px; flex: 1;" onclick="sendToPeer('${peer.ip}', ${peer.port}, '${peer.alias || ''}', '${isLs ? 'localsend' : 'blackhole'}')">
          <div style="width: 40px; height: 40px; border-radius: 12px; background: rgba(6,182,212,0.15); border: 1px solid rgba(6,182,212,0.3); display: flex; align-items: center; justify-content: center; color: var(--accent-cyan);">
            <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect width="14" height="20" x="5" y="2" rx="2" ry="2"/><path d="M12 18h.01"/></svg>
          </div>
          <div class="peer-info">
            <h4 style="display: flex; align-items: center; gap: 4px;">${peer.alias || 'Device'} ${protoBadge} ${isFav ? '<span style="color:#f43f5e;font-size:0.8rem;">❤️</span>' : ''}</h4>
            <span>${peer.ip}:${peer.port}</span>
          </div>
        </div>
        <div style="display: flex; align-items: center; gap: 8px;">
          <button class="btn-icon" style="color: ${isFav ? '#f43f5e' : 'var(--text-muted)'}; padding: 6px;" title="${isFav ? 'Unfavorite' : 'Favorite'}" onclick="event.stopPropagation(); toggleFavoritePeer({ ip: '${peer.ip}', port: ${peer.port}, alias: '${peer.alias || ''}', protocol: '${isLs ? 'localsend' : 'blackhole'}', fingerprint: '${peer.fingerprint || ''}' })">
            <svg width="18" height="18" viewBox="0 0 24 24" fill="${isFav ? 'currentColor' : 'none'}" stroke="currentColor" stroke-width="2"><path d="M19 14c1.49-1.46 3-3.21 3-5.5A5.5 5.5 0 0 0 16.5 3c-1.76 0-3 .5-4.5 2-1.5-1.5-2.74-2-4.5-2A5.5 5.5 0 0 0 2 8.5c0 2.3 1.5 4.05 3 5.5l7 7Z"/></svg>
          </button>
        </div>
      </div>
    `;
    grid.appendChild(card);
  });
}

window.sendToPeer = function(peerIp, peerPort, peerAlias, protocol = 'blackhole') {
  if (selectedFiles.length === 0) {
    alert('Please select files first using the selection grid above!');
    return;
  }
  const targetName = peerAlias || peerIp;
  const filePaths = selectedFiles.map(f => f.path);

  // Set Handshake Modal values & open it (Handshake Workflow with BlackHole Cosmic UI)
  const hsSender = document.getElementById('handshake-sender-alias');
  const hsTarget = document.getElementById('handshake-target-alias');
  const hsModel = document.getElementById('handshake-target-model');
  const hsProto = document.getElementById('handshake-target-proto');
  if (hsSender) hsSender.innerText = currentSettings.alias || 'This PC';
  if (hsTarget) hsTarget.innerText = targetName;
  if (hsProto) hsProto.innerText = (protocol === 'localsend') ? 'HTTPS' : 'P2P';
  if (hsModel) hsModel.innerText = (targetName.toLowerCase().includes('android') || targetName.toLowerCase().includes('phone') || targetName.toLowerCase().includes('sunil')) ? 'Android' : 'Device';
  window.openModal('modal-sender-handshake');

  sendToCpp('send_files', { peerIp, peerPort, peerAlias: targetName, files: filePaths, protocol: protocol || 'blackhole' });

  if (sendLivePeerAlias) sendLivePeerAlias.innerText = targetName;
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

async function startWebRTCShareSession() {
  stopWebRTCShareSession();
  senderCancelled = false;

  if (remoteShareIdleView) remoteShareIdleView.style.display = 'none';
  if (remoteShareActiveView) remoteShareActiveView.style.display = 'block';
  if (modalRoomCodeDisplay) modalRoomCodeDisplay.innerText = '...';
  if (modalRoomStatus) modalRoomStatus.innerText = 'Checking signaling network...';

  await ensureSignalingAwake((statusMsg) => {
    if (modalRoomStatus) modalRoomStatus.innerText = statusMsg;
  });

  connectSignalingSocket(
    (statusMsg) => {
      if (modalRoomStatus) modalRoomStatus.innerText = statusMsg;
    },
    (ws, url) => {
      webrtcSenderWS = ws;
      if (modalRoomStatus) modalRoomStatus.innerText = 'Connected! Creating room...';

      ws.send(JSON.stringify({ type: 'create-room' }));

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
          const shareUrl = url.includes('sunilshahid.com')
            ? `https://share.sunilshahid.com/?code=${msg.code}`
            : `http://${localIps[0] || 'localhost'}:5173/?code=${msg.code}`;
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
        if (modalRoomStatus) modalRoomStatus.innerText = 'Signaling socket connection dropped.';
      };
    },
    (err) => {
      if (modalRoomStatus) modalRoomStatus.innerText = 'Failed to connect to signaling server.';
    }
  );
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
      if (modalRoomStatus) modalRoomStatus.innerText = 'Secure tunnel open! Verifying link...';

      // Send RTC_PING handshake and wait for RTC_PONG
      let pongReceived = false;
      const onPongCheck = (event) => {
        if (event.data instanceof ArrayBuffer) {
          const packet = decodePacket(event.data);
          if (packet.type === 'control' && packet.message.type === 'RTC_PONG') {
            pongReceived = true;
          }
        }
      };

      const dc = webrtc.getDataChannel();
      if (dc) {
        dc.addEventListener('message', onPongCheck);
        try {
          await webrtc.sendPacket(encodeControlPacket({ type: 'RTC_PING' }));
        } catch {}
      }

      for (let i = 0; i < 25; i++) {
        if (pongReceived) break;
        await new Promise(r => setTimeout(r, 100));
      }
      if (dc) dc.removeEventListener('message', onPongCheck);

      console.log(`[WebRTC Sender] Handshake completed (PONG received: ${pongReceived})`);
      if (modalRoomStatus) modalRoomStatus.innerText = 'Direct secure tunnel established! Streaming files...';

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

  // Build unified files to send (prefer DOM File if available, else native selectedFiles with path)
  const filesToSend = selectedFiles.map(meta => {
    const domObj = selectedFileObjects.find(f => f.name === meta.name && f.size === meta.size);
    return domObj || meta;
  });

  try {
    const cryptoKey = await deriveKeyFromSecret(code);
    await sendFilesWebRTC(
      filesToSend,
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
    const filesToSend = selectedFiles.map(meta => {
      const domObj = selectedFileObjects.find(f => f.name === meta.name && f.size === meta.size);
      return domObj || meta;
    });

    await sendFilesWebRTC(
      filesToSend,
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
let receiverRelayTimeout = null;

function stopWebRTCReceiverSession() {
  if (receiverRelayTimeout) {
    clearTimeout(receiverRelayTimeout);
    receiverRelayTimeout = null;
  }
  isReceiverTransferring = false;
  if (webrtcReceiverWS) {
    try { webrtcReceiverWS.close(); } catch {}
    webrtcReceiverWS = null;
  }
  if (webrtcReceiverManager) {
    try { webrtcReceiverManager.close(); } catch {}
    webrtcReceiverManager = null;
  }
  receiverEngine = null;
}

async function startWebRTCReceiverSession(code) {
  stopWebRTCReceiverSession();

  const cont = document.getElementById('fs-files-container');
  if (cont) cont.innerHTML = '';

  const statusEl = document.getElementById('modal-receive-code-status');
  const confirmBtn = document.getElementById('btn-modal-confirm-receive');

  if (statusEl) {
    statusEl.style.display = 'block';
    statusEl.style.color = 'var(--accent-cyan)';
    statusEl.innerText = `Checking signaling network...`;
  }

  await ensureSignalingAwake((statusMsg) => {
    if (statusEl) statusEl.innerText = statusMsg;
  });

  connectSignalingSocket(
    (statusMsg) => {
      if (statusEl) statusEl.innerText = statusMsg;
    },
    (ws, url) => {
      webrtcReceiverWS = ws;
      if (statusEl) statusEl.innerText = `Connected! Joining room [${code}]...`;

      const packetQueue = [];

      deriveKeyFromSecret(code).then(cryptoKey => {
        receiverEngine = new FileReceiverWebRTC(
          cryptoKey,
          (p) => {
            const speedMB = (p.speedBytesPerSec / (1024 * 1024));
            updateLiveTransferUI({
              title: `Receiving from [${code}]...`,
              fileName: p.currentFileName,
              fileBytes: p.currentFileSize,
              percentage: p.percentage,
              transferredBytes: p.transferredBytes,
              totalBytes: p.totalBytes,
              speedMBs: speedMB,
              etaSeconds: p.etaSeconds
            });
          },
          (file) => {
            triggerFileSave(file);
            if (file?.metadata?.name) {
              const safeId = 'fs-file-' + file.metadata.name.replace(/[^a-zA-Z0-9_-]/g, '_');
              const fileEl = document.getElementById(safeId);
              if (fileEl) {
                const bar = fileEl.querySelector('.live-file-bar');
                const doneTag = fileEl.querySelector('.live-file-done-tag');
                if (bar) bar.style.width = '100%';
                if (doneTag) doneTag.style.display = 'block';
              }
            }
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

        // Process any packets received while key was being derived
        while (packetQueue.length > 0) {
          const queued = packetQueue.shift();
          if (queued) receiverEngine.handlePacket(queued);
        }
      });

      // Collect known peer IPs from active LAN discovery
      const peerIps = (activePeers || []).map(p => p.ip).filter(ip => ip && ip !== '127.0.0.1');

      const webrtc = new WebRTCManager(false, {
        onConnectionModeChange: () => {},
        onSignal: (data) => {
          if (ws.readyState === WebSocket.OPEN) {
            ws.send(JSON.stringify({ type: 'signal', data }));
          }
        },
        onDataChannelOpen: () => {
          if (receiverRelayTimeout) {
            clearTimeout(receiverRelayTimeout);
            receiverRelayTimeout = null;
          }
          window.closeModal('modal-receive-code');
          if (confirmBtn) confirmBtn.disabled = false;
          const cont = document.getElementById('fs-files-container');
          if (cont) cont.innerHTML = '';
          updateLiveTransferUI({
            title: `Connected to [${code}]! Receiving...`,
            percentage: 0,
            transferredBytes: 0,
            totalBytes: 0,
            speedMBs: 0,
            etaSeconds: 0
          });
        },
        onDataReceived: (buffer) => {
          window.closeModal('modal-receive-code');
          if (confirmBtn) confirmBtn.disabled = false;
          const packet = decodePacket(buffer);
          if (packet.type === 'control' && packet.message.type === 'RTC_PING') {
            console.log('[WebRTC Receiver] Received RTC_PING -> answering RTC_PONG');
            webrtc.sendPacket(encodeControlPacket({ type: 'RTC_PONG' })).catch(() => {});
            return;
          }
          if (receiverEngine) {
            receiverEngine.handlePacket(buffer);
          } else {
            packetQueue.push(buffer);
          }
        },
        onError: () => {
          if (ws.readyState === WebSocket.OPEN) {
            ws.send(JSON.stringify({ type: 'signal', data: { requestRelay: true } }));
          }
        }
      }, localIps, peerIps);

      webrtc.init();
      webrtcReceiverManager = webrtc;

      ws.onopen = () => {
        ws.send(JSON.stringify({ type: 'join-room', code }));
      };
      if (ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ type: 'join-room', code }));
      }

      ws.onmessage = async (event) => {
        let rawBuffer = event.data;
        if (rawBuffer instanceof Blob) {
          rawBuffer = await rawBuffer.arrayBuffer();
        }

        if (rawBuffer instanceof ArrayBuffer) {
          window.closeModal('modal-receive-code');
          if (confirmBtn) confirmBtn.disabled = false;
          const packet = decodePacket(rawBuffer);
          if (packet.type === 'control' && packet.message.type === 'RTC_PING') {
            console.log('[Relay Receiver] Received RTC_PING -> answering RTC_PONG');
            const pong = encodeControlPacket({ type: 'RTC_PONG' });
            if (ws.readyState === WebSocket.OPEN) ws.send(pong.buffer);
            return;
          }
          if (receiverEngine) {
            receiverEngine.handlePacket(rawBuffer);
          } else {
            packetQueue.push(rawBuffer);
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
          if (statusEl) statusEl.innerText = `Room joined! Establishing P2P link with sender...`;
          if (ws.readyState === WebSocket.OPEN && localIps.length > 0) {
            ws.send(JSON.stringify({ type: 'signal', data: { peerLocalIps: localIps } }));
          }

          // Fallback to relay after 6s if P2P takes long
          if (receiverRelayTimeout) clearTimeout(receiverRelayTimeout);
          receiverRelayTimeout = setTimeout(() => {
            if (ws.readyState === WebSocket.OPEN) {
              console.log('[Receiver] P2P timeout (6s), requesting encrypted relay...');
              ws.send(JSON.stringify({ type: 'signal', data: { requestRelay: true } }));
            }
          }, 6000);
        }
        else if (msg.type === 'signal') {
          if (msg.data?.switchToRelay) {
            console.log('[Receiver] Sender switched to Encrypted Relay');
            if (receiverRelayTimeout) {
              clearTimeout(receiverRelayTimeout);
              receiverRelayTimeout = null;
            }
            window.closeModal('modal-receive-code');
            if (confirmBtn) confirmBtn.disabled = false;
            const cont = document.getElementById('fs-files-container');
            if (cont) cont.innerHTML = '';
            updateLiveTransferUI({
              title: `Connected to [${code}] via Relay! Receiving...`,
              percentage: 0,
              transferredBytes: 0,
              totalBytes: 0,
              speedMBs: 0,
              etaSeconds: 0
            });
          }
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
          if (confirmBtn) confirmBtn.disabled = false;
          if (statusEl) {
            statusEl.style.display = 'block';
            statusEl.style.color = '#ef4444';
            statusEl.innerText = msg.message || 'Room not found or sender disconnected.';
          }
        }
      };

      ws.onerror = () => {
        if (confirmBtn) confirmBtn.disabled = false;
        if (statusEl) {
          statusEl.style.display = 'block';
          statusEl.style.color = '#ef4444';
          statusEl.innerText = 'Signaling connection lost.';
        }
      };
    },
    (err) => {
      if (confirmBtn) confirmBtn.disabled = false;
      if (statusEl) {
        statusEl.style.display = 'block';
        statusEl.style.color = '#ef4444';
        statusEl.innerText = 'Could not connect to signaling server.';
      }
    }
  );
}

async function triggerFileSave(receivedFile) {
  const meta = receivedFile.metadata;
  const fileName = meta.name;

  // 1. Direct native file writing if chunks and IPC bridge are available
  if (typeof window.writeNativeFileChunk === 'function' && receivedFile.chunks && receivedFile.chunks.length > 0) {
    try {
      let offset = 0;
      for (let i = 0; i < receivedFile.chunks.length; i++) {
        const chunkBuf = receivedFile.chunks[i];
        const isLast = (i === receivedFile.chunks.length - 1);
        await window.writeNativeFileChunk(fileName, offset, chunkBuf, isLast);
        offset += chunkBuf.byteLength;
      }
      console.log(`[Native Save] Saved ${fileName} (${offset} bytes) directly to destination folder.`);
      addTransferHistoryItem(fileName, meta.size, 'Success');
      return;
    } catch (err) {
      console.warn('[Native Chunk Write Failed, falling back]', err);
    }
  }

  // 2. Browser auto-download fallback
  try {
    const a = document.createElement('a');
    a.href = receivedFile.downloadUrl;
    a.download = fileName;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
  } catch (e) {
    console.warn('[Auto-Download Failed]', e);
  }

  // 3. Small file fallback to base64 save_file
  if (receivedFile.blob.size < 50 * 1024 * 1024) {
    const reader = new FileReader();
    reader.onload = () => {
      const base64 = reader.result.split(',')[1];
      sendToCpp('save_file', {
        filename: fileName,
        dataBase64: base64
      });
    };
    reader.readAsDataURL(receivedFile.blob);
  }

  addTransferHistoryItem(fileName, meta.size, 'Success');
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

  const receivePinBadge = document.getElementById('receive-pin-badge');
  if (receivePinBadge) receivePinBadge.innerText = `#${(port % 900) + 100}`;

  const receiveIpBadge = document.getElementById('receive-ip-badge');
  if (receiveIpBadge) receiveIpBadge.innerText = `${ip}:${port}`;

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

// Instant Auto-Save on Quick Save Mode
const setQuickMode = document.getElementById('setting-quick-save-mode');
if (setQuickMode) {
  setQuickMode.addEventListener('change', () => {
    currentSettings.quickSaveMode = parseInt(setQuickMode.value, 10) || 0;
    currentSettings.quickSave = currentSettings.quickSaveMode === 2;
    sendToCpp('save_settings', { data: currentSettings });
  });
}

// Instant Auto-Save on Auto-Copy Text Toggle
const setAutoCopy = document.getElementById('setting-auto-copy-text');
if (setAutoCopy) {
  setAutoCopy.addEventListener('change', () => {
    currentSettings.autoCopyText = setAutoCopy.checked;
    sendToCpp('save_settings', { data: currentSettings });
  });
}

// ========================================================
// 🌓 THEME MANAGEMENT (System, Dark, Light)
// ========================================================
function applyTheme(mode) {
  const effectiveMode = (mode === 'system')
    ? (window.matchMedia && window.matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark')
    : mode;
  document.documentElement.setAttribute('data-theme', effectiveMode);
  localStorage.setItem('blackhole_theme', mode);
  const sel = document.getElementById('setting-theme');
  if (sel && sel.value !== mode) sel.value = mode;
}

const savedTheme = localStorage.getItem('blackhole_theme') || 'system';
applyTheme(savedTheme);

if (window.matchMedia) {
  window.matchMedia('(prefers-color-scheme: light)').addEventListener('change', () => {
    const current = localStorage.getItem('blackhole_theme') || 'system';
    if (current === 'system') applyTheme('system');
  });
}

const settingThemeSel = document.getElementById('setting-theme');
if (settingThemeSel) {
  settingThemeSel.value = savedTheme;
  settingThemeSel.addEventListener('change', (e) => {
    applyTheme(e.target.value);
  });
}

// Save settings button
const btnSaveSettings = document.getElementById('btn-save-settings');
if (btnSaveSettings) {
  btnSaveSettings.addEventListener('click', () => {
    const qMode = parseInt(document.getElementById('setting-quick-save-mode')?.value, 10) || 0;
    const updated = {
      alias: document.getElementById('setting-alias')?.value.trim() || 'BlackHole-PC',
      destinationFolder: document.getElementById('setting-dest-folder')?.value.trim(),
      quickSaveMode: qMode,
      quickSave: qMode === 2,
      autoCopyText: document.getElementById('setting-auto-copy-text')?.checked ?? true,
      port: parseInt(document.getElementById('setting-port')?.value, 10) || 53317,
      encryptionEnabled: document.getElementById('setting-encryption')?.checked ?? true,
      themeMode: document.getElementById('setting-theme')?.value || 'system'
    };

    currentSettings = updated;
    applyTheme(updated.themeMode);
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
        currentSettings = { ...currentSettings, ...data.settings };
        if (currentSettings.quickSaveMode === undefined) {
          currentSettings.quickSaveMode = currentSettings.quickSave ? 2 : 0;
        }
        if (currentSettings.autoCopyText === undefined) {
          currentSettings.autoCopyText = true;
        }
        const setAlias = document.getElementById('setting-alias');
        if (setAlias) setAlias.value = currentSettings.alias;
        const setDest = document.getElementById('setting-dest-folder');
        if (setDest) setDest.value = currentSettings.destinationFolder;
        const setQuickMode = document.getElementById('setting-quick-save-mode');
        if (setQuickMode) setQuickMode.value = String(currentSettings.quickSaveMode);
        const setAutoCopy = document.getElementById('setting-auto-copy-text');
        if (setAutoCopy) setAutoCopy.checked = currentSettings.autoCopyText;
        const setPort = document.getElementById('setting-port');
        if (setPort) setPort.value = currentSettings.port;
        const setEnc = document.getElementById('setting-encryption');
        if (setEnc) setEnc.checked = currentSettings.encryptionEnabled;
        if (currentSettings.themeMode) applyTheme(currentSettings.themeMode);

        const favs = loadFavorites();
        const fps = favs.map(f => f.fingerprint).filter(fp => fp && fp.length > 0);
        sendToCpp('update_favorites', { fingerprints: fps });
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
    else if (data.type === 'file_chunk_result') {
      const handler = pendingChunkRequests.get(data.requestId);
      if (handler) {
        pendingChunkRequests.delete(data.requestId);
        if (data.success) {
          const binaryStr = atob(data.dataBase64 || '');
          const bytes = new Uint8Array(binaryStr.length);
          for (let i = 0; i < binaryStr.length; i++) {
            bytes[i] = binaryStr.charCodeAt(i);
          }
          handler.resolve({
            buffer: bytes.buffer,
            bytesRead: data.bytesRead,
            eof: data.eof
          });
        } else {
          handler.reject(new Error(data.error || 'Failed to read file chunk'));
        }
      }
    }
    else if (data.type === 'write_chunk_result') {
      const handler = pendingChunkRequests.get(data.requestId);
      if (handler) {
        pendingChunkRequests.delete(data.requestId);
        handler.resolve(data);
      }
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
    else if (data.type === 'note_saved') {
      if (!selectedFiles.some(existing => existing.name === data.name)) {
        selectedFiles.push({
          name: data.name,
          path: data.path,
          size: data.size
        });
      }
      if (data.isClipboard && data.text) {
        saveTextToHistory('Me', data.text);
      }
      renderSelectedFiles();
    }
    else if (data.type === 'clipboard_data') {
      if (data.target === 'compose') {
        const input = document.getElementById('compose-text-input');
        if (input && data.text) input.value = data.text;
      }
    }
    else if (data.type === 'clipboard_empty') {
      alert('Clipboard is empty.');
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

        const reqModel = document.getElementById('request-sender-model');
        if (reqModel) reqModel.innerText = (ev.peerAlias && (ev.peerAlias.toLowerCase().includes('android') || ev.peerAlias.toLowerCase().includes('lge') || ev.peerAlias.toLowerCase().includes('phone') || ev.peerAlias.toLowerCase().includes('sunil'))) ? 'Lge' : 'Android';

        const reqPin = document.getElementById('request-sender-pin');
        if (reqPin) reqPin.innerText = `#${((ev.totalBytes || 102) % 900 + 100)}`;

        const reqPrompt = document.getElementById('request-prompt-text');
        if (reqPrompt) {
          reqPrompt.innerText = `wants to send you ${ev.fileCount === 1 ? 'a file' : ev.fileCount + ' files'} (${formatBytes(ev.totalBytes || 0)})`;
        }

        const reqCount = document.getElementById('request-files-count');
        if (reqCount) reqCount.innerText = `${ev.fileCount || 1} file(s)`;

        const reqTotal = document.getElementById('request-total-size');
        if (reqTotal) reqTotal.innerText = formatBytes(ev.totalBytes || 0);

        // Reset views: show main request card, hide options subview
        const reqMain = document.getElementById('request-main-view');
        const reqOptions = document.getElementById('request-options-subview');
        if (reqMain) reqMain.style.display = 'block';
        if (reqOptions) reqOptions.style.display = 'none';

        // Options subview folder & files list (Matching media_1792560802796_d8c1af9d.png)
        const optPath = document.getElementById('options-folder-path');
        if (optPath) optPath.innerText = currentSettings.destinationFolder || 'Downloads\\BlackHole';

        const optList = document.getElementById('options-files-list');
        if (optList) {
          optList.innerHTML = '';
          const fnames = ev.fileNames || [];
          fnames.forEach(fn => {
            const row = document.createElement('div');
            row.className = 'options-file-row';
            row.innerHTML = `
              <div class="options-file-left">
                <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="color: var(--accent-cyan); flex-shrink: 0;"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/></svg>
                <div class="options-file-info">
                  <span class="options-file-name">${fn}</span>
                  <span class="options-file-sub">Unchanged • ${formatBytes(ev.totalBytes || 0)}</span>
                </div>
              </div>
              <div style="display: flex; align-items: center; gap: 8px;">
                <input type="checkbox" checked style="accent-color: var(--accent-cyan); width: 17px; height: 17px; cursor: pointer;">
              </div>
            `;
            optList.appendChild(row);
          });
        }

        window.closeModal('modal-sender-handshake');
        window.openModal('modal-transfer-request');
      }
      else if (ev.eventType === 'progress') {
        const isReceiving = ev.peerAlias && ev.peerAlias.toLowerCase().includes('receiving');
        const isWaiting = ev.currentFileName && ev.currentFileName.toLowerCase().includes('waiting for');

        if (isWaiting && !isReceiving) {
          // Keep modal-sender-handshake open, showing "Waiting for [Device] to accept..."
          const hsStatus = document.querySelector('#modal-sender-handshake .handshake-status-line');
          if (hsStatus) hsStatus.innerText = ev.currentFileName;
          return;
        }

        window.closeModal('modal-sender-handshake');

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
          fileName: ev.currentFileName || '',
          percentage: ev.progressPercent,
          transferredBytes: ev.bytesTransferred,
          totalBytes: ev.totalBytes,
          speedMBs: ev.speedMBs,
          etaSeconds: etaSec
        });
      }
      else if (ev.eventType === 'text_message') {
        window.closeModal('modal-sender-handshake');
        window.closeModal('modal-transfer-request');
        const msgAlias = document.getElementById('msg-sender-alias');
        const msgModel = document.getElementById('msg-sender-model');
        const msgText = document.getElementById('msg-text-content');
        if (msgAlias) msgAlias.innerText = ev.peerAlias || 'Nearby Device';
        if (msgModel) msgModel.innerText = (ev.peerAlias && ev.peerAlias.toLowerCase().includes('android')) ? 'Android' : 'Device';
        const txt = ev.textContent || ev.message || '';
        if (msgText) msgText.innerText = txt;
        saveTextToHistory(ev.peerAlias || 'Nearby Device', txt);

        if (currentSettings.autoCopyText) {
          sendToCpp('set_clipboard', { text: txt });
          navigator.clipboard.writeText(txt).then(() => {
            showToast('Text copied to clipboard!');
          }).catch(() => {});
        }

        window.openModal('modal-message-received');
      }
      else if (ev.eventType === 'complete') {
        window.closeModal('modal-sender-handshake');
        window.closeModal('modal-transfer-request');
        completeLiveTransferUI(ev.currentFileName || ev.peerAlias || 'File Transfer', ev.totalBytes);

        const txt = ev.textContent || '';
        if (txt && txt.trim().length > 0) {
          const msgAlias = document.getElementById('msg-sender-alias');
          const msgModel = document.getElementById('msg-sender-model');
          const msgText = document.getElementById('msg-text-content');
          if (msgAlias) msgAlias.innerText = ev.peerAlias || 'Nearby Device';
          if (msgModel) msgModel.innerText = (ev.peerAlias && ev.peerAlias.toLowerCase().includes('android')) ? 'Android' : 'Device';
          if (msgText) msgText.innerText = txt;
          saveTextToHistory(ev.peerAlias || 'Nearby Device', txt);
          window.openModal('modal-message-received');
        }
      }
      else if (ev.eventType === 'error') {
        window.closeModal('modal-sender-handshake');
        window.closeModal('modal-transfer-request');
        errorLiveTransferUI(ev.message || 'Transfer interrupted');
        if (ev.message && (ev.message.includes('declined') || ev.message.includes('did not respond') || ev.message.includes('Could not connect'))) {
          alert(ev.message);
        }
      }
    }
  });
}

// Request Initial Data on Load & Setup UI handlers
function initApp() {
  sendToCpp('get_init_data');
  renderSelectedFiles();
  initBlackHoleCanvas();

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
}

if (document.readyState === 'loading') {
  window.addEventListener('DOMContentLoaded', initApp);
} else {
  initApp();
}

// ========================================================
// 🌌 COSMIC BLACK HOLE SUCTION CANVAS (RECEIVE TAB)
// Matches Android Compose BlackHoleSuctionView 1:1
// ========================================================
function initBlackHoleCanvas() {
  const canvas = document.getElementById('bh-stage-canvas');
  if (!canvas) return;
  const ctx = canvas.getContext('2d');
  if (!ctx) return;

  const width = canvas.width || 220;
  const height = canvas.height || 220;
  const centerX = width / 2;
  const centerY = height / 2;
  const maxRadius = Math.min(width, height) / 2;

  const particleColors = [
    '#06B6D4', // Cyan
    '#8B5CF6', // Violet
    '#EC4899', // Pink
    '#10B981'  // Emerald
  ];

  const startTime = performance.now();

  function render(now) {
    const elapsed = now - startTime;
    const diskAngle = ((elapsed % 8000) / 8000) * (2 * Math.PI);
    const pulsePhase = (elapsed % 2400) / 2400;
    // Cosine pulse scale between 0.94 and 1.06
    const pulseScale = 1.0 + 0.06 * Math.sin(pulsePhase * 2 * Math.PI);
    const suctionPhase = (elapsed % 2400) / 2400;

    const horizonRadius = maxRadius * 0.38 * pulseScale;

    ctx.clearRect(0, 0, width, height);

    // 1. Accretion Swirl Background (Rotating Dashed Rings)
    ctx.save();
    ctx.translate(centerX, centerY);
    ctx.rotate(diskAngle);

    // Radial gradient glow
    const bgGrad = ctx.createRadialGradient(0, 0, 0, 0, 0, maxRadius * 0.95);
    bgGrad.addColorStop(0, 'rgba(139, 92, 246, 0.20)');
    bgGrad.addColorStop(0.5, 'rgba(6, 182, 212, 0.10)');
    bgGrad.addColorStop(1, 'rgba(0, 0, 0, 0)');
    ctx.fillStyle = bgGrad;
    ctx.beginPath();
    ctx.arc(0, 0, maxRadius * 0.95, 0, 2 * Math.PI);
    ctx.fill();

    // Outer dashed ring
    ctx.strokeStyle = 'rgba(139, 92, 246, 0.25)';
    ctx.lineWidth = 2;
    ctx.setLineDash([18, 18]);
    ctx.beginPath();
    ctx.arc(0, 0, maxRadius * 0.82, 0, 2 * Math.PI);
    ctx.stroke();

    // Inner dashed ring
    ctx.strokeStyle = 'rgba(6, 182, 212, 0.20)';
    ctx.lineWidth = 1.5;
    ctx.setLineDash([12, 14]);
    ctx.beginPath();
    ctx.arc(0, 0, maxRadius * 0.62, 0, 2 * Math.PI);
    ctx.stroke();
    ctx.restore();

    // 2. Gravitational Inflow Particles with Decreasing Glowing Tails
    const numParticles = 4;
    for (let i = 0; i < numParticles; i++) {
      const offsetPhase = (suctionPhase + (i / numParticles)) % 1;
      const r = maxRadius * 0.90 * (1 - (offsetPhase * offsetPhase)) + (horizonRadius * 0.15);
      const baseAngle = i * (2 * Math.PI / numParticles);
      const totalAngle = baseAngle + (offsetPhase * 4.2 * Math.PI);

      const px = centerX + r * Math.cos(totalAngle);
      const py = centerY + r * Math.sin(totalAngle);

      let alpha = 0.95;
      if (offsetPhase < 0.15) {
        alpha = offsetPhase / 0.15;
      } else if (offsetPhase > 0.82) {
        alpha = (1 - offsetPhase) / 0.18;
      }
      alpha = Math.max(0, Math.min(1, alpha));

      const particleRadius = 5 * (1 - (offsetPhase * 0.65));
      const hexColor = particleColors[i % particleColors.length];
      const rVal = parseInt(hexColor.slice(1, 3), 16);
      const gVal = parseInt(hexColor.slice(3, 5), 16);
      const bVal = parseInt(hexColor.slice(5, 7), 16);

      // 2a. Decreasing Trailing Tail
      let prevX = px;
      let prevY = py;
      const tailSteps = 10;
      for (let s = 1; s <= tailSteps; s++) {
        const trailPhase = offsetPhase - (s * 0.012);
        if (trailPhase <= 0) break;

        const tr = maxRadius * 0.90 * (1 - (trailPhase * trailPhase)) + (horizonRadius * 0.15);
        const trailAngle = baseAngle + (trailPhase * 4.2 * Math.PI);
        const tx = centerX + tr * Math.cos(trailAngle);
        const ty = centerY + tr * Math.sin(trailAngle);

        const stepFactor = 1 - (s / tailSteps);
        const tailWidth = Math.max(1.5, particleRadius * 1.8 * stepFactor);
        const tailAlpha = Math.max(0, Math.min(1, alpha * 0.65 * stepFactor));

        ctx.strokeStyle = `rgba(${rVal}, ${gVal}, ${bVal}, ${tailAlpha})`;
        ctx.lineWidth = tailWidth;
        ctx.lineCap = 'round';
        ctx.beginPath();
        ctx.moveTo(prevX, prevY);
        ctx.lineTo(tx, ty);
        ctx.stroke();

        prevX = tx;
        prevY = ty;
      }

      // 2b. Glowing Halos & Core
      // Outer soft halo
      ctx.fillStyle = `rgba(${rVal}, ${gVal}, ${bVal}, ${alpha * 0.35})`;
      ctx.beginPath();
      ctx.arc(px, py, particleRadius * 2.8, 0, 2 * Math.PI);
      ctx.fill();

      // Mid halo
      ctx.fillStyle = `rgba(${rVal}, ${gVal}, ${bVal}, ${alpha * 0.7})`;
      ctx.beginPath();
      ctx.arc(px, py, particleRadius * 1.6, 0, 2 * Math.PI);
      ctx.fill();

      // Particle core
      ctx.fillStyle = `rgba(${rVal}, ${gVal}, ${bVal}, ${alpha})`;
      ctx.beginPath();
      ctx.arc(px, py, particleRadius, 0, 2 * Math.PI);
      ctx.fill();

      // White glint
      ctx.fillStyle = `rgba(255, 255, 255, ${alpha * 0.9})`;
      ctx.beginPath();
      ctx.arc(px, py, particleRadius * 0.45, 0, 2 * Math.PI);
      ctx.fill();
    }

    // 3. Black Hole Event Horizon Singularity
    // Outer horizon glow
    const hGlow = ctx.createRadialGradient(centerX, centerY, 0, centerX, centerY, horizonRadius * 1.55);
    hGlow.addColorStop(0, 'rgba(139, 92, 246, 0.6)');
    hGlow.addColorStop(0.6, 'rgba(6, 182, 212, 0.2)');
    hGlow.addColorStop(1, 'rgba(0, 0, 0, 0)');
    ctx.fillStyle = hGlow;
    ctx.beginPath();
    ctx.arc(centerX, centerY, horizonRadius * 1.55, 0, 2 * Math.PI);
    ctx.fill();

    // Event Horizon Core (Deep Void)
    const voidGrad = ctx.createRadialGradient(centerX, centerY, 0, centerX, centerY, horizonRadius);
    voidGrad.addColorStop(0, '#010103');
    voidGrad.addColorStop(0.6, '#090A14');
    voidGrad.addColorStop(1, '#140D2E');
    ctx.fillStyle = voidGrad;
    ctx.beginPath();
    ctx.arc(centerX, centerY, horizonRadius, 0, 2 * Math.PI);
    ctx.fill();

    // Boundary Rim
    ctx.strokeStyle = 'rgba(139, 92, 246, 0.7)';
    ctx.lineWidth = 2.5;
    ctx.setLineDash([]);
    ctx.beginPath();
    ctx.arc(centerX, centerY, horizonRadius, 0, 2 * Math.PI);
    ctx.stroke();

    // Inner Rim
    ctx.strokeStyle = 'rgba(6, 182, 212, 0.4)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.arc(centerX, centerY, horizonRadius * 0.94, 0, 2 * Math.PI);
    ctx.stroke();

    // Singularity Center Core Glow
    ctx.fillStyle = 'rgba(196, 181, 253, 0.2)';
    ctx.beginPath();
    ctx.arc(centerX, centerY, horizonRadius * 0.28, 0, 2 * Math.PI);
    ctx.fill();

    requestAnimationFrame(render);
  }

  requestAnimationFrame(render);
}
