/**
 * BlackHole WebRTC & Zero-Knowledge Cryptography Engine
 * Pure vanilla ES6 implementation for Windows WebView2 & Web clients.
 * Full wire-compatibility with BlackHole web client protocol.
 */

// --- PROTOCOL CONSTANTS & PACKET HELPERS ---
const PACKET_TYPE_CHUNK = 0x01;
const PACKET_TYPE_CONTROL = 0x02;
const CHUNK_SIZE = 32 * 1024; // 32 KB chunk size (optimal for SCTP / WebRTC limits)

function encodeChunkPacket(fileIndex, chunkIndex, encryptedPayload) {
  const headerSize = 9;
  const packet = new Uint8Array(headerSize + encryptedPayload.byteLength);
  const view = new DataView(packet.buffer);

  view.setUint8(0, PACKET_TYPE_CHUNK);
  view.setUint32(1, fileIndex, false); // big-endian
  view.setUint32(5, chunkIndex, false); // big-endian

  packet.set(new Uint8Array(encryptedPayload), headerSize);
  return packet;
}

function decodePacket(buffer) {
  if (!(buffer instanceof ArrayBuffer)) {
    return { type: 'unknown' };
  }
  const view = new DataView(buffer);
  const packetType = view.getUint8(0);

  if (packetType === PACKET_TYPE_CHUNK) {
    if (buffer.byteLength < 9) return { type: 'unknown' };
    const fileIndex = view.getUint32(1, false);
    const chunkIndex = view.getUint32(5, false);
    const payload = buffer.slice(9);
    return { type: 'chunk', fileIndex, chunkIndex, payload };
  }

  if (packetType === PACKET_TYPE_CONTROL) {
    const jsonBytes = new Uint8Array(buffer, 1);
    const decoder = new TextDecoder();
    try {
      const message = JSON.parse(decoder.decode(jsonBytes));
      return { type: 'control', message };
    } catch {
      return { type: 'unknown' };
    }
  }

  return { type: 'unknown' };
}

function encodeControlPacket(message) {
  const encoder = new TextEncoder();
  const jsonBytes = encoder.encode(JSON.stringify(message));
  const packet = new Uint8Array(1 + jsonBytes.byteLength);
  packet[0] = PACKET_TYPE_CONTROL;
  packet.set(jsonBytes, 1);
  return packet;
}

// --- ZERO-KNOWLEDGE CRYPTOGRAPHY (WebCrypto API) ---
const SALT_STRING = 'BlackHole-P2P-ZeroKnowledge-Salt-v1';
const PBKDF2_ITERATIONS = 10000;

async function deriveKeyFromSecret(secret) {
  const enc = new TextEncoder();
  const rawKey = enc.encode(secret);
  const salt = enc.encode(SALT_STRING);

  const keyMaterial = await window.crypto.subtle.importKey(
    'raw',
    rawKey,
    { name: 'PBKDF2' },
    false,
    ['deriveKey']
  );

  const webKey = await window.crypto.subtle.deriveKey(
    {
      name: 'PBKDF2',
      salt,
      iterations: PBKDF2_ITERATIONS,
      hash: 'SHA-256',
    },
    keyMaterial,
    { name: 'AES-GCM', length: 256 },
    true,
    ['encrypt', 'decrypt']
  );

  return { webKey, preferredCipher: 'aes-gcm' };
}

function getRandomBytes(length) {
  const bytes = new Uint8Array(length);
  window.crypto.getRandomValues(bytes);
  return bytes;
}

async function encryptChunk(cryptoKey, data) {
  const iv = getRandomBytes(12);
  const ciphertext = await window.crypto.subtle.encrypt(
    {
      name: 'AES-GCM',
      iv: iv,
    },
    cryptoKey.webKey,
    data
  );

  const combined = new Uint8Array(iv.byteLength + ciphertext.byteLength);
  combined.set(iv, 0);
  combined.set(new Uint8Array(ciphertext), iv.byteLength);
  return combined.buffer;
}

async function decryptChunk(cryptoKey, encryptedData) {
  const iv = new Uint8Array(encryptedData, 0, 12);
  const ciphertext = new Uint8Array(encryptedData, 12);

  return await window.crypto.subtle.decrypt(
    {
      name: 'AES-GCM',
      iv: iv,
    },
    cryptoKey.webKey,
    ciphertext
  );
}

// --- WEBRTC MANAGER ---
const ICE_SERVERS = {
  iceServers: [
    { urls: 'stun:stun.cloudflare.com:3478' },
    { urls: 'stun:stun.l.google.com:19302' },
    { urls: 'stun:stun1.l.google.com:19302' },
    { urls: 'stun:stun2.l.google.com:19302' },
  ],
  iceCandidatePoolSize: 10,
};

const BUFFER_THRESHOLD = 1024 * 1024; // 1 MB backpressure limit
const BUFFER_LOW_THRESHOLD = 128 * 1024; // 128 KB low threshold

const isPrivateIp = (ip) =>
  /^(192\.168\.|10\.|172\.(1[6-9]|2[0-9]|3[01])\.)/.test(ip);

class WebRTCManager {
  constructor(isSender, events = {}, myLocalIps = [], remoteLocalIps = []) {
    this.isSender = isSender;
    this.events = events;
    this.myLocalIps = myLocalIps || [];
    this.remoteLocalIps = remoteLocalIps || [];
    this.pc = null;
    this.dc = null;
    this.connectionMode = 'connecting';
    this.connectionRoute = '';
    this.candidateQueue = [];
    this.remoteDescriptionSet = false;
  }

  setRemoteLocalIps(ips) {
    if (ips && Array.isArray(ips)) {
      this.remoteLocalIps = ips;
      console.log('[WebRTC LAN Synthesis] Updated remote peer local IPs:', this.remoteLocalIps);
    }
  }

  init() {
    console.log(`[WebRTC] Initializing RTCPeerConnection (Role: ${this.isSender ? 'SENDER' : 'RECEIVER'})`);
    this.pc = new RTCPeerConnection(ICE_SERVERS);

    this.pc.onicecandidate = (event) => {
      if (event.candidate && this.events.onSignal) {
        // 1. Send genuine candidate
        this.events.onSignal({ candidate: event.candidate.toJSON() });

        const candStr = event.candidate.candidate;
        if (!candStr) return;

        // 2. De-anonymize THIS machine's .local mDNS host candidate with OUR real physical LAN IP
        if (candStr.includes('typ host') && candStr.includes('.local')) {
          for (const ip of this.myLocalIps) {
            const deAnonymized = candStr.replace(/[\w\-]+\.local/, ip);
            console.log('[WebRTC LAN Synthesis] Emitting de-anonymized direct LAN candidate for this host:', ip);
            this.events.onSignal({
              candidate: {
                candidate: deAnonymized,
                sdpMid: event.candidate.sdpMid,
                sdpMLineIndex: event.candidate.sdpMLineIndex,
              },
            });
          }
        }

        // 3. If STUN reveals private IP in raddr, emit direct host candidate
        if (candStr.includes('typ srflx')) {
          const raddrMatch = candStr.match(/raddr\s+(\d+\.\d+\.\d+\.\d+)/);
          const rportMatch = candStr.match(/rport\s+(\d+)/);
          if (raddrMatch && rportMatch && isPrivateIp(raddrMatch[1])) {
            const synthetic = candStr
              .replace(/typ srflx/, 'typ host')
              .replace(/(\d+\.\d+\.\d+\.\d+)\s+(\d+)\s+typ host/, `${raddrMatch[1]} ${rportMatch[1]} typ host`);
            console.log('[WebRTC LAN Synthesis] Emitting direct LAN candidate from STUN raddr:', raddrMatch[1], rportMatch[1]);
            this.events.onSignal({
              candidate: {
                candidate: synthetic,
                sdpMid: event.candidate.sdpMid,
                sdpMLineIndex: event.candidate.sdpMLineIndex,
              },
            });
          }
        }
      }
    };

    this.pc.oniceconnectionstatechange = () => {
      const iceState = this.pc?.iceConnectionState;
      console.log(`[WebRTC] ICE state -> ${iceState}`);
      if (iceState === 'connected' || iceState === 'completed') {
        this.detectConnectionMode();
      }
    };

    this.pc.onconnectionstatechange = () => {
      const state = this.pc?.connectionState;
      console.log(`[WebRTC] PeerConnection state -> ${state}`);
      if (state === 'connected') {
        this.detectConnectionMode();
      } else if (state === 'disconnected' || state === 'failed' || state === 'closed') {
        this.setMode('disconnected');
        this.events.onDataChannelClose?.();
      }
    };

    if (this.isSender) {
      this.dc = this.pc.createDataChannel('blackhole-transfer', { ordered: true });
      this.setupDataChannel(this.dc);
    } else {
      this.pc.ondatachannel = (event) => {
        this.dc = event.channel;
        this.setupDataChannel(this.dc);
      };
    }
  }

  setupDataChannel(dc) {
    dc.binaryType = 'arraybuffer';
    dc.bufferedAmountLowThreshold = BUFFER_LOW_THRESHOLD;

    dc.onopen = () => {
      console.log('[WebRTC] RTCDataChannel opened successfully');
      this.events.onDataChannelOpen?.();
      this.detectConnectionMode();
    };

    dc.onclose = () => {
      console.log('[WebRTC] RTCDataChannel closed');
      this.events.onDataChannelClose?.();
      this.setMode('disconnected');
    };

    dc.onmessage = (event) => {
      if (event.data instanceof ArrayBuffer) {
        this.events.onDataReceived?.(event.data);
      }
    };

    dc.onerror = (err) => {
      console.error('[WebRTC] DataChannel error', err);
      this.events.onError?.(err);
    };
  }

  setMode(mode, route = '') {
    this.connectionMode = mode;
    this.connectionRoute = route;
    this.events.onConnectionModeChange?.(mode, route);
  }

  async detectConnectionMode(attempt = 0) {
    if (!this.pc) return;
    try {
      const stats = await this.pc.getStats();
      let selectedPair = null;

      stats.forEach((report) => {
        if (
          report.type === 'candidate-pair' &&
          (report.selected || report.state === 'succeeded' || report.nominated)
        ) {
          selectedPair = report;
        }
      });

      if (selectedPair) {
        const localCandidate = stats.get(selectedPair.localCandidateId);
        const remoteCandidate = stats.get(selectedPair.remoteCandidateId);

        if (localCandidate && remoteCandidate) {
          const localType = localCandidate.candidateType;
          const remoteType = remoteCandidate.candidateType;
          const localIp = localCandidate.address || localCandidate.ip || '';
          const remoteIp = remoteCandidate.address || remoteCandidate.ip || '';

          const routeStr = `${localIp} ⇄ ${remoteIp}`;
          console.log(`[WebRTC Path Inspection] ${localType}(${localIp}) <-> ${remoteType}(${remoteIp})`);

          if (
            (localType === 'host' && remoteType === 'host') ||
            (isPrivateIp(localIp) && isPrivateIp(remoteIp)) ||
            (isPrivateIp(remoteIp))
          ) {
            console.log(`[WebRTC] Active connection is DIRECT LAN (Local Wire Speed): ${routeStr}`);
            this.setMode('lan', routeStr);
            return;
          } else if (localType === 'relay' || remoteType === 'relay') {
            this.setMode('relay', routeStr);
            return;
          } else {
            this.setMode('wan', routeStr);
            return;
          }
        }
      }

      if (attempt < 4) {
        setTimeout(() => this.detectConnectionMode(attempt + 1), 500);
      } else {
        this.setMode('wan');
      }
    } catch {
      this.setMode('wan');
    }
  }

  async createOffer() {
    if (!this.pc) throw new Error('PeerConnection not initialized');
    const offer = await this.pc.createOffer();
    await this.pc.setLocalDescription(offer);
    return offer;
  }

  async createAnswer(offer) {
    if (!this.pc) throw new Error('PeerConnection not initialized');
    await this.pc.setRemoteDescription(new RTCSessionDescription(offer));
    this.remoteDescriptionSet = true;
    await this.flushCandidateQueue();
    const answer = await this.pc.createAnswer();
    await this.pc.setLocalDescription(answer);
    return answer;
  }

  async handleAnswer(answer) {
    if (!this.pc) throw new Error('PeerConnection not initialized');
    await this.pc.setRemoteDescription(new RTCSessionDescription(answer));
    this.remoteDescriptionSet = true;
    await this.flushCandidateQueue();
  }

  async handleCandidate(candidateInit) {
    if (!this.pc) return;
    if (!this.remoteDescriptionSet || !this.pc.remoteDescription) {
      this.candidateQueue.push(candidateInit);
      return;
    }
    await this.addCandidateInternal(candidateInit);
  }

  async addCandidateInternal(candidateInit) {
    if (!this.pc) return;
    try {
      await this.pc.addIceCandidate(new RTCIceCandidate(candidateInit));

      const candStr = candidateInit.candidate;
      if (!candStr) return;

      // 1. If remote peer sent a host candidate with .local, de-anonymize with remote known IPs
      if (candStr.includes('typ host') && candStr.includes('.local')) {
        for (const ip of this.remoteLocalIps) {
          const deAnonymized = candStr.replace(/[\w\-]+\.local/, ip);
          console.log('[WebRTC LAN Synthesis] Injecting direct LAN IP route for remote peer:', ip);
          await this.pc.addIceCandidate(new RTCIceCandidate({
            candidate: deAnonymized,
            sdpMid: candidateInit.sdpMid,
            sdpMLineIndex: candidateInit.sdpMLineIndex,
          })).catch(() => {});
        }
      }

      // 2. Synthesize direct LAN host candidate if STUN candidate reveals private IP in raddr
      if (candStr.includes('typ srflx')) {
        const raddrMatch = candStr.match(/raddr\s+(\d+\.\d+\.\d+\.\d+)/);
        const rportMatch = candStr.match(/rport\s+(\d+)/);
        if (raddrMatch && rportMatch && isPrivateIp(raddrMatch[1])) {
          const synthetic = candStr
            .replace(/typ srflx/, 'typ host')
            .replace(/(\d+\.\d+\.\d+\.\d+)\s+(\d+)\s+typ host/, `${raddrMatch[1]} ${rportMatch[1]} typ host`);
          
          console.log('[WebRTC LAN Synthesis] Injecting direct LAN route from remote STUN raddr:', raddrMatch[1], rportMatch[1]);
          await this.pc.addIceCandidate(new RTCIceCandidate({
            candidate: synthetic,
            sdpMid: candidateInit.sdpMid,
            sdpMLineIndex: candidateInit.sdpMLineIndex,
          })).catch(() => {});
        }
      }
    } catch (err) {
      console.warn('[WebRTC] Candidate error', err);
    }
  }

  async flushCandidateQueue() {
    while (this.candidateQueue.length > 0) {
      const candidate = this.candidateQueue.shift();
      if (candidate) {
        await this.addCandidateInternal(candidate);
      }
    }
  }

  async sendPacket(data) {
    if (!this.dc || this.dc.readyState !== 'open') {
      throw new Error('DataChannel is not open');
    }

    // Backpressure flow control
    if (this.dc.bufferedAmount > BUFFER_THRESHOLD) {
      await new Promise((resolve, reject) => {
        let done = false;
        let lastBuffered = this.dc?.bufferedAmount || 0;
        let lastProgressTime = Date.now();

        const finish = () => {
          if (!done) {
            done = true;
            cleanup();
            resolve();
          }
        };

        const fail = (err) => {
          if (!done) {
            done = true;
            cleanup();
            reject(err);
          }
        };

        const onLow = () => finish();
        this.dc?.addEventListener('bufferedamountlow', onLow, { once: true });

        const poll = setInterval(() => {
          if (!this.dc || this.dc.readyState !== 'open') {
            fail(new Error('DataChannel closed during wait'));
            return;
          }
          const currentBuffered = this.dc.bufferedAmount;
          if (currentBuffered <= BUFFER_LOW_THRESHOLD) {
            finish();
            return;
          }
          if (currentBuffered < lastBuffered) {
            lastBuffered = currentBuffered;
            lastProgressTime = Date.now();
          }
          if (Date.now() - lastProgressTime > 10000) {
            fail(new Error('DataChannel buffer stalled'));
          }
        }, 15);

        const cleanup = () => {
          clearInterval(poll);
          this.dc?.removeEventListener('bufferedamountlow', onLow);
        };
      });
    }

    if (!this.dc || this.dc.readyState !== 'open') {
      throw new Error('DataChannel closed during send');
    }

    this.dc.send(data);
  }

  getDataChannel() {
    return this.dc;
  }

  close() {
    try { this.dc?.close(); } catch {}
    try { this.pc?.close(); } catch {}
    this.dc = null;
    this.pc = null;
    this.setMode('disconnected');
  }
}

// --- STREAMING SENDER ENGINE ---
async function sendFilesWebRTC(files, transport, cryptoKey, onProgress, isCancelled) {
  const metadataList = files.map((f, idx) => ({
    id: `file_${idx}_${Date.now()}`,
    name: f.name,
    size: f.size,
    type: f.type || 'application/octet-stream',
    lastModified: f.lastModified || Date.now(),
  }));

  const totalBytes = files.reduce((acc, f) => acc + f.size, 0);
  let transferredBytes = 0;
  let lastProgressUpdate = Date.now();
  let lastTransferredBytes = 0;
  let rollingSpeed = 0;

  // 1. Send manifest
  const manifest = {
    type: 'MANIFEST',
    files: metadataList,
    totalBytes,
    transferId: `transfer_${Date.now()}`,
    cipher: 'aes-gcm',
  };

  await transport.sendPacket(encodeControlPacket(manifest));

  // 2. Iterate each file
  for (let fileIdx = 0; fileIdx < files.length; fileIdx++) {
    if (isCancelled && isCancelled()) break;

    const file = files[fileIdx];
    const totalChunks = Math.ceil(file.size / CHUNK_SIZE);

    await transport.sendPacket(
      encodeControlPacket({
        type: 'FILE_START',
        fileIndex: fileIdx,
        totalChunks,
      })
    );

    // Read and encrypt in 1 MB blocks
    const READ_BLOCK_SIZE = 1024 * 1024;
    const readAndEncryptBlock = async (blockStartOffset, startChunkIdx) => {
      const blockSlice = file.slice(blockStartOffset, blockStartOffset + READ_BLOCK_SIZE);
      const blockBuffer = await blockSlice.arrayBuffer();

      const promises = [];
      let curIdx = startChunkIdx;
      let batchBytes = 0;

      for (let chunkOffset = 0; chunkOffset < blockBuffer.byteLength; chunkOffset += CHUNK_SIZE) {
        const curChunkSize = Math.min(CHUNK_SIZE, blockBuffer.byteLength - chunkOffset);
        const rawChunk = blockBuffer.slice(chunkOffset, chunkOffset + curChunkSize);
        const thisChunkIdx = curIdx++;
        batchBytes += curChunkSize;

        promises.push(
          encryptChunk(cryptoKey, rawChunk).then((encrypted) =>
            encodeChunkPacket(fileIdx, thisChunkIdx, encrypted)
          )
        );
      }

      const packets = await Promise.all(promises);
      return {
        packets,
        totalBatchBytes: batchBytes,
        nextOffset: blockStartOffset + blockBuffer.byteLength,
        nextChunkIdx: curIdx,
      };
    };

    let offset = 0;
    let chunkIdx = 0;
    let nextBlockPromise = readAndEncryptBlock(offset, chunkIdx);

    while (offset < file.size) {
      if (isCancelled && isCancelled()) break;

      const currentBlock = await nextBlockPromise;
      offset = currentBlock.nextOffset;
      chunkIdx = currentBlock.nextChunkIdx;

      if (offset < file.size && !(isCancelled && isCancelled())) {
        nextBlockPromise = readAndEncryptBlock(offset, chunkIdx);
      }

      for (let i = 0; i < currentBlock.packets.length; i++) {
        if (isCancelled && isCancelled()) break;
        await transport.sendPacket(currentBlock.packets[i]);
      }
      transferredBytes += currentBlock.totalBatchBytes;

      const now = Date.now();
      if (now - lastProgressUpdate > 80 || transferredBytes === totalBytes) {
        const timeDiff = (now - lastProgressUpdate) / 1000;
        const bytesDiff = transferredBytes - lastTransferredBytes;
        const instantSpeed = timeDiff > 0 ? bytesDiff / timeDiff : 0;
        rollingSpeed = rollingSpeed === 0 ? instantSpeed : rollingSpeed * 0.7 + instantSpeed * 0.3;

        lastProgressUpdate = now;
        lastTransferredBytes = transferredBytes;

        const remainingBytes = totalBytes - transferredBytes;
        const eta = rollingSpeed > 0 ? remainingBytes / rollingSpeed : 0;
        const percentage = totalBytes > 0 ? Math.min(100, Math.round((transferredBytes / totalBytes) * 100)) : 100;

        if (onProgress) {
          onProgress({
            totalBytes,
            transferredBytes,
            currentFileIndex: fileIdx,
            totalFiles: files.length,
            currentFileName: file.name,
            percentage,
            speedBytesPerSec: rollingSpeed,
            etaSeconds: Math.ceil(eta),
          });
        }
      }
    }

    await transport.sendPacket(
      encodeControlPacket({
        type: 'FILE_END',
        fileIndex: fileIdx,
      })
    );
  }

  if (!(isCancelled && isCancelled())) {
    await transport.sendPacket(encodeControlPacket({ type: 'TRANSFER_COMPLETE' }));
  }
}

// --- STREAMING RECEIVER ENGINE ---
class FileReceiverWebRTC {
  constructor(cryptoKey, onProgress, onFileReady, onComplete) {
    this.cryptoKey = cryptoKey;
    this.onProgress = onProgress;
    this.onFileReady = onFileReady;
    this.onComplete = onComplete;
    this.manifest = null;
    this.incomingChunks = new Map();
    this.receivedFiles = [];
    this.totalBytes = 0;
    this.transferredBytes = 0;
    this.lastUpdate = Date.now();
    this.lastTransferredBytes = 0;
    this.rollingSpeed = 0;
  }

  async handlePacket(buffer) {
    const packet = decodePacket(buffer);

    if (packet.type === 'control') {
      const msg = packet.message;
      if (msg.type === 'MANIFEST') {
        this.manifest = msg;
        this.totalBytes = msg.totalBytes;
        this.transferredBytes = 0;
        this.lastUpdate = Date.now();
      } else if (msg.type === 'FILE_START') {
        this.incomingChunks.set(msg.fileIndex, { chunks: [], receivedBytes: 0 });
      } else if (msg.type === 'FILE_END') {
        await this.finalizeFile(msg.fileIndex);
      } else if (msg.type === 'TRANSFER_COMPLETE') {
        this.onComplete?.();
      }
    } else if (packet.type === 'chunk') {
      const { fileIndex, chunkIndex, payload } = packet;
      const decrypted = await decryptChunk(this.cryptoKey, payload);

      const fileState = this.incomingChunks.get(fileIndex);
      if (fileState) {
        fileState.chunks[chunkIndex] = decrypted;
        fileState.receivedBytes += decrypted.byteLength;
        this.transferredBytes += decrypted.byteLength;

        const now = Date.now();
        // Throttle progress updates to at most once per 80ms to eliminate UI flickering
        if (now - this.lastUpdate > 80 || this.transferredBytes === this.totalBytes) {
          const timeDiff = (now - this.lastUpdate) / 1000;
          const bytesDiff = this.transferredBytes - this.lastTransferredBytes;
          const instantSpeed = timeDiff > 0 ? bytesDiff / timeDiff : 0;
          this.rollingSpeed = this.rollingSpeed === 0 ? instantSpeed : this.rollingSpeed * 0.7 + instantSpeed * 0.3;

          this.lastUpdate = now;
          this.lastTransferredBytes = this.transferredBytes;

          const remainingBytes = this.totalBytes - this.transferredBytes;
          const eta = this.rollingSpeed > 0 ? remainingBytes / this.rollingSpeed : 0;
          const currentFileName = this.manifest?.files[fileIndex]?.name || 'Receiving...';
          const percentage = this.totalBytes > 0
            ? Math.min(100, Math.round((this.transferredBytes / this.totalBytes) * 100))
            : 100;

          if (this.onProgress) {
            this.onProgress({
              totalBytes: this.totalBytes,
              transferredBytes: this.transferredBytes,
              currentFileIndex: fileIndex,
              totalFiles: this.manifest?.files?.length || 1,
              currentFileName,
              percentage,
              speedBytesPerSec: this.rollingSpeed,
              etaSeconds: Math.ceil(eta),
            });
          }
        }
      }
    }
  }

  async finalizeFile(fileIndex) {
    const fileState = this.incomingChunks.get(fileIndex);
    if (!fileState || !this.manifest) return;

    const meta = this.manifest.files[fileIndex];
    if (!meta) return;

    const blob = new Blob(fileState.chunks, { type: meta.type || 'application/octet-stream' });
    const downloadUrl = URL.createObjectURL(blob);

    const received = {
      metadata: meta,
      blob,
      downloadUrl,
    };

    this.receivedFiles.push(received);
    this.incomingChunks.delete(fileIndex);
    this.onFileReady?.(received);
  }
}
