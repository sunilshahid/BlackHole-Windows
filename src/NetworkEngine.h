#pragma once
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include "AppConfig.h"

namespace BlackHole {

struct PeerDevice {
    std::string ip;
    int port = 53317;
    std::string alias;
    std::string deviceType = "desktop";
    int64_t lastSeen = 0;
};

struct FileItem {
    std::string name;
    int64_t size = 0;
    std::string path;
};

struct TransferEvent {
    std::string type; // "incoming_request", "progress", "complete", "error"
    std::string peerIp;
    std::string peerAlias;
    std::string currentFileName;
    int64_t bytesTransferred = 0;
    int64_t totalBytes = 0;
    double speedMBs = 0.0;
    int progressPercent = 0;
    std::string message;
    int fileCount = 0;
    std::vector<std::string> fileNames;
};

class NetworkEngine {
public:
    using PeerCallback = std::function<void(const std::vector<PeerDevice>&)>;
    using TransferCallback = std::function<void(const TransferEvent&)>;

private:
    AppSettings settings;
    std::mutex settingsMutex;

    // UDP Discovery
    std::atomic<bool> isRunning{false};
    std::thread udpBroadcastThread;
    std::thread udpListenThread;
    std::map<std::string, PeerDevice> discoveredPeers;
    std::mutex peersMutex;
    PeerCallback onPeersChanged;

    // TCP Transfer Server
    std::thread tcpServerThread;
    TransferCallback onTransferEvent;
    std::atomic<bool> pendingTransferAccepted{false};
    std::atomic<bool> pendingTransferRejected{false};

    // Client Transfer Thread
    std::thread clientTransferThread;
    std::atomic<bool> cancelCurrentTransfer{false};

public:
    NetworkEngine();
    ~NetworkEngine();

    void updateSettings(const AppSettings& newSettings);
    void setPeerCallback(PeerCallback cb) { onPeersChanged = cb; }
    void setTransferCallback(TransferCallback cb) { onTransferEvent = cb; }

    bool start();
    void stop();

    std::vector<std::string> getLocalIPAddresses();
    std::vector<PeerDevice> getPeers();

    void acceptIncomingTransfer();
    void rejectIncomingTransfer();

    bool sendFilesToPeer(const std::string& peerIp, int peerPort, const std::vector<std::string>& filePaths, const std::string& targetAlias = "");
    void cancelTransfer();

    // Local Web Link Sharing (Bidirectional Local HTTP Server)
    void setSharedFiles(const std::vector<std::string>& files);
    std::vector<std::string> getSharedFiles();

private:
    void runUdpBroadcaster();
    void runUdpListener();
    void runTcpServer();
    void handleIncomingConnection(uintptr_t clientSock, std::string clientIp);
    void handleHttpRequest(uintptr_t clientSock, const std::string& clientIp);
    int64_t getCurrentTimeMs();

    std::vector<std::string> sharedFiles;
    std::mutex sharedFilesMutex;
};

} // namespace BlackHole
