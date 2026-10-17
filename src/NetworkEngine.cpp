#define WIN32_LEAN_AND_MEAN
#include "NetworkEngine.h"
#include "SimpleJson.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <iostream>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace BlackHole {

NetworkEngine::NetworkEngine() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    settings = ConfigManager::load();
}

NetworkEngine::~NetworkEngine() {
    stop();
    WSACleanup();
}

void NetworkEngine::updateSettings(const AppSettings& newSettings) {
    std::lock_guard<std::mutex> lock(settingsMutex);
    settings = newSettings;
}

int64_t NetworkEngine::getCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::vector<std::string> NetworkEngine::getLocalIPAddresses() {
    std::vector<std::string> ips;
    ULONG outBufLen = 15000;
    PIP_ADAPTER_ADDRESSES pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(outBufLen);

    if (pAddresses == NULL) return ips;

    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, pAddresses, &outBufLen) == ERROR_BUFFER_OVERFLOW) {
        free(pAddresses);
        pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(outBufLen);
        if (pAddresses == NULL) return ips;
    }

    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, pAddresses, &outBufLen) == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES pCurr = pAddresses; pCurr != NULL; pCurr = pCurr->Next) {
            if (pCurr->OperStatus != IfOperStatusUp) continue;
            for (PIP_ADAPTER_UNICAST_ADDRESS pUnicast = pCurr->FirstUnicastAddress; pUnicast != NULL; pUnicast = pUnicast->Next) {
                sockaddr_in* sa_in = (sockaddr_in*)pUnicast->Address.lpSockaddr;
                char ipStr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(sa_in->sin_addr), ipStr, INET_ADDRSTRLEN);
                std::string ip(ipStr);
                if (ip != "127.0.0.1" && ip.find("169.254.") != 0) {
                    ips.push_back(ip);
                }
            }
        }
    }

    free(pAddresses);
    return ips;
}

std::vector<PeerDevice> NetworkEngine::getPeers() {
    std::lock_guard<std::mutex> lock(peersMutex);
    std::vector<PeerDevice> list;
    int64_t now = getCurrentTimeMs();
    for (auto const& [ip, peer] : discoveredPeers) {
        if (now - peer.lastSeen < 12000) {
            list.push_back(peer);
        }
    }
    return list;
}

bool NetworkEngine::start() {
    if (isRunning) return true;
    isRunning = true;

    udpBroadcastThread = std::thread(&NetworkEngine::runUdpBroadcaster, this);
    udpListenThread = std::thread(&NetworkEngine::runUdpListener, this);
    tcpServerThread = std::thread(&NetworkEngine::runTcpServer, this);

    return true;
}

void NetworkEngine::stop() {
    if (!isRunning) return;
    isRunning = false;

    cancelTransfer();

    if (udpBroadcastThread.joinable()) udpBroadcastThread.join();
    if (udpListenThread.joinable()) udpListenThread.detach(); // Detach blocking recv
    if (tcpServerThread.joinable()) tcpServerThread.detach(); // Detach blocking accept
    if (clientTransferThread.joinable()) clientTransferThread.join();
}

void NetworkEngine::runUdpBroadcaster() {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return;

    BOOL broadcast = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char*)&broadcast, sizeof(broadcast));

    sockaddr_in destAddr;
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(53317);
    destAddr.sin_addr.s_addr = INADDR_BROADCAST;

    while (isRunning) {
        std::string alias;
        int port;
        {
            std::lock_guard<std::mutex> lock(settingsMutex);
            alias = settings.alias;
            port = settings.port;
        }

        JsonValue json = JsonValue::Object();
        json.set("app", "BlackHole");
        json.set("version", "1.0");
        json.set("alias", alias);
        json.set("port", port);
        json.set("deviceType", "desktop");
        json.set("protocol", "v1");

        std::string msg = json.serialize();
        sendto(sock, msg.c_str(), static_cast<int>(msg.size()), 0, (sockaddr*)&destAddr, sizeof(destAddr));

        // Sleep for 2.5 seconds
        for (int i = 0; i < 25 && isRunning; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    closesocket(sock);
}

void NetworkEngine::runUdpListener() {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return;

    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    // Set receive timeout so thread can exit cleanly
    DWORD timeout = 2000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    sockaddr_in bindAddr;
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(53317);
    bindAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        closesocket(sock);
        return;
    }

    char buffer[4096];
    sockaddr_in senderAddr;
    int senderLen = sizeof(senderAddr);

    while (isRunning) {
        int bytes = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&senderAddr, &senderLen);
        if (bytes > 0) {
            buffer[bytes] = '\0';
            char senderIp[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(senderAddr.sin_addr), senderIp, INET_ADDRSTRLEN);
            std::string ip(senderIp);

            // Filter out own IP addresses
            auto localIps = getLocalIPAddresses();
            bool isSelf = false;
            for (auto const& myIp : localIps) {
                if (myIp == ip) {
                    isSelf = true;
                    break;
                }
            }

            if (!isSelf) {
                JsonValue val = JsonParser::parse(std::string(buffer));
                std::string appName = val.get("app").asString();
                std::string alias = val.get("alias").asString();
                int port = val.get("port").asInt(53317);
                std::string devType = val.get("deviceType").asString("phone");

                if (!alias.empty()) {
                    PeerDevice peer;
                    peer.ip = ip;
                    peer.port = port;
                    peer.alias = alias;
                    peer.deviceType = devType;
                    peer.lastSeen = getCurrentTimeMs();

                    {
                        std::lock_guard<std::mutex> lock(peersMutex);
                        discoveredPeers[ip] = peer;
                    }

                    if (onPeersChanged) {
                        onPeersChanged(getPeers());
                    }
                }
            }
        }
    }

    closesocket(sock);
}

void NetworkEngine::runTcpServer() {
    int port = 53317;
    {
        std::lock_guard<std::mutex> lock(settingsMutex);
        port = settings.port;
    }

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) return;

    BOOL reuse = TRUE;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenSock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        closesocket(listenSock);
        return;
    }

    if (listen(listenSock, 10) == SOCKET_ERROR) {
        closesocket(listenSock);
        return;
    }

    while (isRunning) {
        sockaddr_in clientAddr;
        int clientLen = sizeof(clientAddr);
        SOCKET clientSock = accept(listenSock, (sockaddr*)&clientAddr, &clientLen);
        if (clientSock == INVALID_SOCKET) continue;

        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(clientAddr.sin_addr), clientIp, INET_ADDRSTRLEN);

        std::thread([this, clientSock, ip = std::string(clientIp)]() {
            handleIncomingConnection(static_cast<uintptr_t>(clientSock), ip);
        }).detach();
    }

    closesocket(listenSock);
}

void NetworkEngine::handleIncomingConnection(uintptr_t clientSockRaw, std::string clientIp) {
    SOCKET sock = static_cast<SOCKET>(clientSockRaw);

    // 1. Read 4 bytes: header length (Network Byte Order)
    uint32_t headerLenNet = 0;
    int r = recv(sock, (char*)&headerLenNet, sizeof(headerLenNet), MSG_WAITALL);
    if (r != sizeof(headerLenNet)) {
        closesocket(sock);
        return;
    }
    uint32_t headerLen = ntohl(headerLenNet);
    if (headerLen == 0 || headerLen > 10 * 1024 * 1024) {
        closesocket(sock);
        return;
    }

    // 2. Read Header JSON
    std::string headerStr(headerLen, '\0');
    r = recv(sock, &headerStr[0], headerLen, MSG_WAITALL);
    if (r != headerLen) {
        closesocket(sock);
        return;
    }

    JsonValue header = JsonParser::parse(headerStr);
    std::string senderAlias = header.get("senderAlias").asString("Nearby Device");
    int64_t totalBytes = header.get("totalBytes").asInt64(0);
    JsonValue filesArr = header.get("files");

    std::string destDir;
    bool quickSave;
    {
        std::lock_guard<std::mutex> lock(settingsMutex);
        destDir = settings.destinationFolder;
        quickSave = settings.quickSave;
    }

    // If quickSave is false, wait for user prompt
    if (!quickSave) {
        pendingTransferAccepted = false;
        pendingTransferRejected = false;

        TransferEvent ev;
        ev.type = "incoming_request";
        ev.peerIp = clientIp;
        ev.peerAlias = senderAlias;
        ev.totalBytes = totalBytes;
        ev.message = "Incoming transfer request from " + senderAlias;
        if (onTransferEvent) onTransferEvent(ev);

        // Wait up to 30 seconds for user action
        int elapsed = 0;
        while (!pendingTransferAccepted && !pendingTransferRejected && elapsed < 300 && isRunning) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            elapsed++;
        }

        if (!pendingTransferAccepted) {
            JsonValue resp = JsonValue::Object();
            resp.set("status", "REJECTED");
            std::string respStr = resp.serialize();
            uint32_t respLenNet = htonl(static_cast<uint32_t>(respStr.size()));
            send(sock, (char*)&respLenNet, sizeof(respLenNet), 0);
            send(sock, respStr.c_str(), static_cast<int>(respStr.size()), 0);
            closesocket(sock);
            return;
        }
    }

    // Send ACCEPTED
    {
        JsonValue resp = JsonValue::Object();
        resp.set("status", "ACCEPTED");
        std::string respStr = resp.serialize();
        uint32_t respLenNet = htonl(static_cast<uint32_t>(respStr.size()));
        send(sock, (char*)&respLenNet, sizeof(respLenNet), 0);
        send(sock, respStr.c_str(), static_cast<int>(respStr.size()), 0);
    }

    std::filesystem::create_directories(destDir);

    int64_t totalBytesReceived = 0;
    auto startTime = std::chrono::steady_clock::now();
    const size_t CHUNK_SIZE = 1024 * 1024; // 1 MB chunk buffer
    std::vector<char> buffer(CHUNK_SIZE);

    for (auto const& fileObj : filesArr.arrVal) {
        std::string fileName = fileObj.get("name").asString("file");
        int64_t fileSize = fileObj.get("size").asInt64(0);

        std::filesystem::path outPath = std::filesystem::path(destDir) / fileName;
        std::ofstream outFile(outPath, std::ios::binary);
        if (!outFile.is_open()) {
            TransferEvent err;
            err.type = "error";
            err.message = "Failed to write file: " + fileName;
            if (onTransferEvent) onTransferEvent(err);
            closesocket(sock);
            return;
        }

        int64_t fileBytesReceived = 0;
        while (fileBytesReceived < fileSize && isRunning) {
            size_t toRead = static_cast<size_t>(std::min<int64_t>(CHUNK_SIZE, fileSize - fileBytesReceived));
            int recvd = recv(sock, buffer.data(), static_cast<int>(toRead), 0);
            if (recvd <= 0) {
                outFile.close();
                closesocket(sock);
                return;
            }

            outFile.write(buffer.data(), recvd);
            fileBytesReceived += recvd;
            totalBytesReceived += recvd;

            auto now = std::chrono::steady_clock::now();
            double seconds = std::chrono::duration<double>(now - startTime).count();
            double speed = (seconds > 0) ? (totalBytesReceived / (1024.0 * 1024.0)) / seconds : 0.0;
            int pct = (totalBytes > 0) ? static_cast<int>((totalBytesReceived * 100) / totalBytes) : 100;

            TransferEvent prog;
            prog.type = "progress";
            prog.peerIp = clientIp;
            prog.peerAlias = senderAlias;
            prog.currentFileName = fileName;
            prog.bytesTransferred = totalBytesReceived;
            prog.totalBytes = totalBytes;
            prog.speedMBs = speed;
            prog.progressPercent = pct;
            if (onTransferEvent) onTransferEvent(prog);
        }

        outFile.close();
    }

    // Send COMPLETE
    {
        JsonValue resp = JsonValue::Object();
        resp.set("status", "COMPLETE");
        std::string respStr = resp.serialize();
        uint32_t respLenNet = htonl(static_cast<uint32_t>(respStr.size()));
        send(sock, (char*)&respLenNet, sizeof(respLenNet), 0);
        send(sock, respStr.c_str(), static_cast<int>(respStr.size()), 0);
    }

    closesocket(sock);

    TransferEvent done;
    done.type = "complete";
    done.peerIp = clientIp;
    done.peerAlias = senderAlias;
    done.totalBytes = totalBytes;
    done.progressPercent = 100;
    done.message = "Transfer completed successfully!";
    if (onTransferEvent) onTransferEvent(done);
}

void NetworkEngine::acceptIncomingTransfer() {
    pendingTransferAccepted = true;
}

void NetworkEngine::rejectIncomingTransfer() {
    pendingTransferRejected = true;
}

void NetworkEngine::cancelTransfer() {
    cancelCurrentTransfer = true;
}

bool NetworkEngine::sendFilesToPeer(const std::string& peerIp, int peerPort, const std::vector<std::string>& filePaths) {
    if (clientTransferThread.joinable()) {
        clientTransferThread.join();
    }

    cancelCurrentTransfer = false;

    clientTransferThread = std::thread([this, peerIp, peerPort, filePaths]() {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) {
            TransferEvent ev;
            ev.type = "error";
            ev.message = "Failed to create socket.";
            if (onTransferEvent) onTransferEvent(ev);
            return;
        }

        // Set high socket buffer sizes for wire-speed LAN streaming
        int bufSize = 4 * 1024 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)&bufSize, sizeof(bufSize));

        sockaddr_in targetAddr;
        targetAddr.sin_family = AF_INET;
        targetAddr.sin_port = htons(peerPort);
        inet_pton(AF_INET, peerIp.c_str(), &targetAddr.sin_addr);

        if (connect(sock, (sockaddr*)&targetAddr, sizeof(targetAddr)) == SOCKET_ERROR) {
            closesocket(sock);
            TransferEvent ev;
            ev.type = "error";
            ev.message = "Could not connect to peer at " + peerIp;
            if (onTransferEvent) onTransferEvent(ev);
            return;
        }

        // Gather file metadata
        JsonValue filesArr = JsonValue::Array();
        int64_t totalBytes = 0;
        for (auto const& pathStr : filePaths) {
            std::filesystem::path p(pathStr);
            if (!std::filesystem::exists(p)) continue;
            int64_t sz = std::filesystem::file_size(p);
            totalBytes += sz;

            JsonValue f = JsonValue::Object();
            f.set("name", p.filename().string());
            f.set("size", sz);
            filesArr.push(f);
        }

        std::string myAlias;
        {
            std::lock_guard<std::mutex> lock(settingsMutex);
            myAlias = settings.alias;
        }

        JsonValue header = JsonValue::Object();
        header.set("type", "TRANSFER_HEADER");
        header.set("senderAlias", myAlias);
        header.set("numFiles", static_cast<int>(filePaths.size()));
        header.set("totalBytes", totalBytes);
        header.set("files", filesArr);

        std::string headerStr = header.serialize();
        uint32_t headerLenNet = htonl(static_cast<uint32_t>(headerStr.size()));

        send(sock, (char*)&headerLenNet, sizeof(headerLenNet), 0);
        send(sock, headerStr.c_str(), static_cast<int>(headerStr.size()), 0);

        // Wait for ACCEPTED response
        uint32_t respLenNet = 0;
        int r = recv(sock, (char*)&respLenNet, sizeof(respLenNet), MSG_WAITALL);
        if (r != sizeof(respLenNet)) {
            closesocket(sock);
            TransferEvent ev;
            ev.type = "error";
            ev.message = "Peer did not respond.";
            if (onTransferEvent) onTransferEvent(ev);
            return;
        }

        uint32_t respLen = ntohl(respLenNet);
        std::string respStr(respLen, '\0');
        recv(sock, &respStr[0], respLen, MSG_WAITALL);
        JsonValue resp = JsonParser::parse(respStr);
        if (resp.get("status").asString() != "ACCEPTED") {
            closesocket(sock);
            TransferEvent ev;
            ev.type = "error";
            ev.message = "Peer rejected the transfer.";
            if (onTransferEvent) onTransferEvent(ev);
            return;
        }

        // Stream file buffers
        int64_t totalBytesSent = 0;
        auto startTime = std::chrono::steady_clock::now();
        const size_t CHUNK_SIZE = 1024 * 1024; // 1 MB chunks
        std::vector<char> buffer(CHUNK_SIZE);

        for (auto const& pathStr : filePaths) {
            std::filesystem::path p(pathStr);
            if (!std::filesystem::exists(p)) continue;
            int64_t fileSize = std::filesystem::file_size(p);
            std::string fileName = p.filename().string();

            std::ifstream inFile(p, std::ios::binary);
            if (!inFile.is_open()) continue;

            int64_t fileBytesSent = 0;
            while (fileBytesSent < fileSize && !cancelCurrentTransfer && isRunning) {
                size_t toRead = static_cast<size_t>(std::min<int64_t>(CHUNK_SIZE, fileSize - fileBytesSent));
                inFile.read(buffer.data(), toRead);
                std::streamsize bytesRead = inFile.gcount();
                if (bytesRead <= 0) break;

                int sent = send(sock, buffer.data(), static_cast<int>(bytesRead), 0);
                if (sent <= 0) {
                    inFile.close();
                    closesocket(sock);
                    TransferEvent ev;
                    ev.type = "error";
                    ev.message = "Transfer connection lost.";
                    if (onTransferEvent) onTransferEvent(ev);
                    return;
                }

                fileBytesSent += sent;
                totalBytesSent += sent;

                auto now = std::chrono::steady_clock::now();
                double seconds = std::chrono::duration<double>(now - startTime).count();
                double speed = (seconds > 0) ? (totalBytesSent / (1024.0 * 1024.0)) / seconds : 0.0;
                int pct = (totalBytes > 0) ? static_cast<int>((totalBytesSent * 100) / totalBytes) : 100;

                TransferEvent prog;
                prog.type = "progress";
                prog.peerIp = peerIp;
                prog.currentFileName = fileName;
                prog.bytesTransferred = totalBytesSent;
                prog.totalBytes = totalBytes;
                prog.speedMBs = speed;
                prog.progressPercent = pct;
                if (onTransferEvent) onTransferEvent(prog);
            }

            inFile.close();
            if (cancelCurrentTransfer) break;
        }

        closesocket(sock);

        if (cancelCurrentTransfer) {
            TransferEvent ev;
            ev.type = "error";
            ev.message = "Transfer canceled.";
            if (onTransferEvent) onTransferEvent(ev);
        } else {
            TransferEvent done;
            done.type = "complete";
            done.peerIp = peerIp;
            done.totalBytes = totalBytes;
            done.progressPercent = 100;
            done.message = "Transfer completed successfully!";
            if (onTransferEvent) onTransferEvent(done);
        }
    });

    return true;
}

} // namespace BlackHole
