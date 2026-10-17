// State
let currentSettings = {
  alias: 'BlackHole-PC',
  destinationFolder: '',
  quickSave: true,
  port: 53317,
  encryptionEnabled: true
};

let selectedFiles = []; // Array of { name, size, path }
let activePeers = [];
let localIps = [];

// Helper to send messages to C++ backend
function sendToCpp(action, data = {}) {
  if (window.chrome && window.chrome.webview) {
    window.chrome.webview.postMessage(JSON.stringify({ action, ...data }));
  } else {
    console.log('[Mock Webview]', action, data);
  }
}

// Format bytes
function formatBytes(bytes) {
  if (bytes === 0) return '0 B';
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

// Dropzone & File Selection
const dropzone = document.getElementById('dropzone');
dropzone.addEventListener('click', () => {
  sendToCpp('browse_files');
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
  // If dragged from desktop/explorer, C++ browse_files or native drop handles it
  sendToCpp('browse_files');
});

function renderSelectedFiles() {
  const container = document.getElementById('selected-files-list');
  container.innerHTML = '';

  if (selectedFiles.length === 0) return;

  selectedFiles.forEach((f, idx) => {
    const item = document.createElement('div');
    item.className = 'file-item';
    item.innerHTML = `
      <div class="file-name">${f.name}</div>
      <div style="display: flex; align-items: center; gap: 10px;">
        <span class="file-size">${formatBytes(f.size)}</span>
        <span style="cursor: pointer; color: #ef4444;" onclick="removeSelectedFile(${idx})">✕</span>
      </div>
    `;
    container.appendChild(item);
  });
}

window.removeSelectedFile = function(idx) {
  selectedFiles.splice(idx, 1);
  renderSelectedFiles();
};

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
  document.getElementById('transfer-overlay').classList.remove('active');
});

// Transfer Overlay Updates
function updateTransferOverlay(ev) {
  const overlay = document.getElementById('transfer-overlay');

  if (ev.eventType === 'progress') {
    overlay.classList.add('active');
    document.getElementById('transfer-modal-title').innerText = 'Transferring Files...';
    document.getElementById('transfer-modal-file').innerText = ev.currentFileName || '';
    document.getElementById('transfer-fill').style.width = (ev.progressPercent || 0) + '%';
    document.getElementById('transfer-percent').innerText = (ev.progressPercent || 0) + '%';
    document.getElementById('transfer-speed').innerText = (ev.speedMBs || 0).toFixed(1) + ' MB/s';
    document.getElementById('transfer-bytes').innerText = 
      formatBytes(ev.bytesTransferred) + ' / ' + formatBytes(ev.totalBytes);
  }
  else if (ev.eventType === 'complete') {
    document.getElementById('transfer-fill').style.width = '100%';
    document.getElementById('transfer-percent').innerText = '100%';
    document.getElementById('transfer-modal-title').innerText = 'Transfer Complete! 🎉';
    
    // Add to history
    const hist = document.getElementById('transfer-history');
    if (hist.innerText.includes('No recent')) hist.innerHTML = '';
    const hItem = document.createElement('div');
    hItem.style.padding = '8px 0';
    hItem.style.borderBottom = '1px solid rgba(255,255,255,0.06)';
    hItem.style.textAlign = 'left';
    hItem.innerHTML = `<strong>${ev.peerAlias || ev.peerIp}</strong> — ${formatBytes(ev.totalBytes)} <span style="color:#10b981; float:right;">Success</span>`;
    hist.prepend(hItem);

    setTimeout(() => {
      overlay.classList.remove('active');
    }, 1500);
  }
  else if (ev.eventType === 'error') {
    alert(ev.message || 'Transfer failed.');
    overlay.classList.remove('active');
  }
}

// Listen to Messages from C++ backend
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
      selectedFiles = data.files || [];
      renderSelectedFiles();
    }
    else if (data.type === 'folder_selected') {
      currentSettings.destinationFolder = data.path;
      document.getElementById('setting-dest-folder').value = data.path;
      sendToCpp('save_settings', { data: currentSettings });
    }
    else if (data.type === 'transfer_event') {
      updateTransferOverlay(data);
    }
  });
}

// Request Initial Data on Load
window.addEventListener('DOMContentLoaded', () => {
  sendToCpp('get_init_data');
});
