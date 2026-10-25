#define WIN32_LEAN_AND_MEAN
#include "NetworkEngine.h"
#include "SimpleJson.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <ncrypt.h>
#include <bcrypt.h>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <shlobj.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ncrypt.lib")
#pragma comment(lib, "bcrypt.lib")

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

namespace BlackHole {

static void LogDebug(const std::string& msg) {
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &localAppData))) {
        std::wstring dir = std::wstring(localAppData) + L"\\BlackHole";
        CreateDirectoryW(dir.c_str(), NULL);
        std::wstring logPath = dir + L"\\debug.log";
        CoTaskMemFree(localAppData);
        std::ofstream out(logPath, std::ios::app);
        if (out.is_open()) {
            int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            out << "[" << nowMs << "] [Net] " << msg << std::endl;
        }
    }
}

static PCCERT_CONTEXT g_pClientCertContext = NULL;
static NCRYPT_KEY_HANDLE g_hClientKey = 0;
static NCRYPT_PROV_HANDLE g_hClientProv = 0;
static std::mutex g_certMutex;
static std::string g_deviceFingerprint = "";

static std::string ComputeCertFingerprint(PCCERT_CONTEXT pCert) {
    if (!pCert || !pCert->pbCertEncoded || pCert->cbCertEncoded == 0) return "";
    BCRYPT_ALG_HANDLE hAlg = NULL;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) return "";
    BCRYPT_HASH_HANDLE hHash = NULL;
    if (BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return "";
    }
    BCryptHashData(hHash, pCert->pbCertEncoded, pCert->cbCertEncoded, 0);
    BYTE hash[32];
    BCryptFinishHash(hHash, hash, 32, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    char hex[65];
    for (int i = 0; i < 32; i++) {
        sprintf_s(&hex[i * 2], 3, "%02X", hash[i]);
    }
    hex[64] = '\0';
    return std::string(hex);
}

static PCCERT_CONTEXT GetOrCreateClientCert() {
    std::lock_guard<std::mutex> lock(g_certMutex);
    if (g_pClientCertContext) return g_pClientCertContext;

    if (!g_hClientProv) {
        SECURITY_STATUS ss = NCryptOpenStorageProvider(&g_hClientProv, MS_KEY_STORAGE_PROVIDER, 0);
        if (ss != ERROR_SUCCESS) return NULL;
    }

    if (!g_hClientKey) {
        SECURITY_STATUS ss = NCryptOpenKey(g_hClientProv, &g_hClientKey, L"BlackHoleCNGAuthKey", 0, 0);
        if (ss != ERROR_SUCCESS) {
            ss = NCryptCreatePersistedKey(g_hClientProv, &g_hClientKey, BCRYPT_RSA_ALGORITHM, L"BlackHoleCNGAuthKey", 0, 0);
            if (ss == ERROR_SUCCESS) {
                DWORD keyLength = 2048;
                NCryptSetProperty(g_hClientKey, NCRYPT_LENGTH_PROPERTY, (PBYTE)&keyLength, sizeof(keyLength), 0);
                NCryptFinalizeKey(g_hClientKey, 0);
            } else {
                return NULL;
            }
        }
    }

    std::filesystem::path certPath = std::filesystem::path(ConfigManager::getConfigFilePath()).parent_path() / "device_cert.der";
    if (std::filesystem::exists(certPath)) {
        std::ifstream cf(certPath, std::ios::binary);
        if (cf.is_open()) {
            std::vector<BYTE> der((std::istreambuf_iterator<char>(cf)), std::istreambuf_iterator<char>());
            cf.close();
            if (!der.empty()) {
                PCCERT_CONTEXT pLoaded = CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, der.data(), static_cast<DWORD>(der.size()));
                if (pLoaded) {
                    WCHAR nameBuf[256] = { 0 };
                    CertGetNameStringW(pLoaded, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, nameBuf, 256);
                    if (wcscmp(nameBuf, L"LocalSend User") == 0) {
                        g_pClientCertContext = pLoaded;
                        CRYPT_KEY_PROV_INFO kpi = { 0 };
                        kpi.pwszContainerName = (LPWSTR)L"BlackHoleCNGAuthKey";
                        kpi.pwszProvName = (LPWSTR)MS_KEY_STORAGE_PROVIDER;
                        kpi.dwProvType = 0;
                        kpi.dwFlags = 0;
                        kpi.cProvParam = 0;
                        kpi.rgProvParam = NULL;
                        kpi.dwKeySpec = 0;
                        CertSetCertificateContextProperty(g_pClientCertContext, CERT_KEY_PROV_INFO_PROP_ID, 0, &kpi);
                    } else {
                        CertFreeCertificateContext(pLoaded);
                        std::error_code ec;
                        std::filesystem::remove(certPath, ec);
                    }
                }
            }
        }
    }

    if (!g_pClientCertContext) {
        CERT_NAME_BLOB subjectNameBlob = { 0 };
        if (!CertStrToNameW(X509_ASN_ENCODING, L"CN=LocalSend User", CERT_X500_NAME_STR, NULL, NULL, &subjectNameBlob.cbData, NULL)) {
            return NULL;
        }
        std::vector<BYTE> pbNameData(subjectNameBlob.cbData);
        subjectNameBlob.pbData = pbNameData.data();
        if (!CertStrToNameW(X509_ASN_ENCODING, L"CN=LocalSend User", CERT_X500_NAME_STR, NULL, subjectNameBlob.pbData, &subjectNameBlob.cbData, NULL)) {
            return NULL;
        }

        CRYPT_ALGORITHM_IDENTIFIER sigAlg = { 0 };
        sigAlg.pszObjId = (LPSTR)szOID_RSA_SHA256RSA;

        CERT_ENHKEY_USAGE eku = { 0 };
        char* usage[] = { (char*)szOID_PKIX_KP_CLIENT_AUTH };
        eku.cUsageIdentifier = 1;
        eku.rgpszUsageIdentifier = usage;

        CRYPT_DATA_BLOB ekuBlob = { 0 };
        CryptEncodeObject(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, &eku, NULL, &ekuBlob.cbData);
        std::vector<BYTE> ekuBytes(ekuBlob.cbData);
        ekuBlob.pbData = ekuBytes.data();
        CryptEncodeObject(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, &eku, ekuBlob.pbData, &ekuBlob.cbData);

        CERT_EXTENSION certExt[1] = { 0 };
        certExt[0].pszObjId = (char*)szOID_ENHANCED_KEY_USAGE;
        certExt[0].fCritical = FALSE;
        certExt[0].Value = ekuBlob;

        CERT_EXTENSIONS extensions = { 0 };
        extensions.cExtension = 1;
        extensions.rgExtension = certExt;

        SYSTEMTIME stStart = { 0 };
        stStart.wYear = 2020;
        stStart.wMonth = 1;
        stStart.wDay = 1;

        SYSTEMTIME stEnd = { 0 };
        stEnd.wYear = 2040;
        stEnd.wMonth = 1;
        stEnd.wDay = 1;

        g_pClientCertContext = CertCreateSelfSignCertificate(
            g_hClientKey,
            &subjectNameBlob,
            0,
            NULL,
            &sigAlg,
            &stStart,
            &stEnd,
            &extensions
        );

        if (g_pClientCertContext && g_pClientCertContext->pbCertEncoded && g_pClientCertContext->cbCertEncoded > 0) {
            std::ofstream out(certPath, std::ios::binary);
            if (out.is_open()) {
                out.write(reinterpret_cast<const char*>(g_pClientCertContext->pbCertEncoded), g_pClientCertContext->cbCertEncoded);
                out.close();
            }
        }
    }

    if (g_pClientCertContext) {
        CRYPT_KEY_PROV_INFO kpi = { 0 };
        kpi.pwszContainerName = (LPWSTR)L"BlackHoleCNGAuthKey";
        kpi.pwszProvName = (LPWSTR)MS_KEY_STORAGE_PROVIDER;
        kpi.dwProvType = 0;
        kpi.dwFlags = 0;
        kpi.cProvParam = 0;
        kpi.rgProvParam = NULL;
        kpi.dwKeySpec = 0;
        CertSetCertificateContextProperty(g_pClientCertContext, CERT_KEY_PROV_INFO_PROP_ID, 0, &kpi);
    }

    if (g_pClientCertContext && g_deviceFingerprint.empty()) {
        g_deviceFingerprint = ComputeCertFingerprint(g_pClientCertContext);
    }

    return g_pClientCertContext;
}

// Helper function to send an HTTP or HTTPS request using WinHTTP (with self-signed cert bypass and mTLS)
static bool sendWinHttpRequest(
    const std::string& host,
    int port,
    bool isHttps,
    const std::wstring& verb,
    const std::wstring& path,
    const std::string& postData,
    const std::wstring& contentType,
    int& outStatusCode,
    std::string& outResponseBody
) {
    std::wstring wHost(host.begin(), host.end());
    HINTERNET hSession = WinHttpOpen(L"BlackHole/2.2", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    WinHttpSetTimeouts(hSession, 4000, 4000, 60000, 60000);

    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), static_cast<INTERNET_PORT>(port), 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD reqFlags = isHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, verb.c_str(), path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, reqFlags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    if (isHttps) {
        DWORD secFlags = SECURITY_FLAG_IGNORE_ALL_CERT_ERRORS;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));

        DWORD ignoreRevOffline = TRUE;
        WinHttpSetOption(hRequest, 155 /* WINHTTP_OPTION_IGNORE_CERT_REVOCATION_OFFLINE */, &ignoreRevOffline, sizeof(ignoreRevOffline));
    }

    std::wstring headers = L"";
    if (!contentType.empty()) {
        headers = L"Content-Type: " + contentType + L"\r\n";
    }

    LogDebug("sendWinHttpRequest: sending to " + host + ":" + std::to_string(port) + " isHttps=" + std::to_string(isHttps) + " bodySize=" + std::to_string(postData.size()));

    BOOL sent = WinHttpSendRequest(
        hRequest,
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        static_cast<DWORD>(headers.size()),
        postData.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)postData.data(),
        static_cast<DWORD>(postData.size()),
        static_cast<DWORD>(postData.size()),
        0
    );

    if (!sent && isHttps) {
        DWORD err = GetLastError();
        LogDebug("sendWinHttpRequest: initial send failed, err=" + std::to_string(err));
        if (err == ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED) {
            PCCERT_CONTEXT pCert = GetOrCreateClientCert();
            if (pCert) {
                DWORD secFlags = SECURITY_FLAG_IGNORE_ALL_CERT_ERRORS;
                WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
                DWORD ignoreRevOffline = TRUE;
                WinHttpSetOption(hRequest, 155 /* WINHTTP_OPTION_IGNORE_CERT_REVOCATION_OFFLINE */, &ignoreRevOffline, sizeof(ignoreRevOffline));

                WinHttpSetOption(hRequest, WINHTTP_OPTION_CLIENT_CERT_CONTEXT, (LPVOID)pCert, sizeof(CERT_CONTEXT));
                sent = WinHttpSendRequest(
                    hRequest,
                    headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                    static_cast<DWORD>(headers.size()),
                    postData.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)postData.data(),
                    static_cast<DWORD>(postData.size()),
                    static_cast<DWORD>(postData.size()),
                    0
                );
                LogDebug("sendWinHttpRequest: send with cert result=" + std::to_string(sent) + " err=" + std::to_string(GetLastError()));
            }
        }
    }

    if (!sent) {
        LogDebug("sendWinHttpRequest: failed after send, err=" + std::to_string(GetLastError()));
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        DWORD rxErr = GetLastError();
        LogDebug("sendWinHttpRequest: WinHttpReceiveResponse failed, rxErr=" + std::to_string(rxErr));
        if (isHttps && rxErr == ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED) {
            PCCERT_CONTEXT pCert = GetOrCreateClientCert();
            if (pCert) {
                WinHttpSetOption(hRequest, WINHTTP_OPTION_CLIENT_CERT_CONTEXT, (LPVOID)pCert, sizeof(CERT_CONTEXT));
                if (WinHttpSendRequest(hRequest, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), static_cast<DWORD>(headers.size()), postData.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)postData.data(), static_cast<DWORD>(postData.size()), static_cast<DWORD>(postData.size()), 0)) {
                    if (WinHttpReceiveResponse(hRequest, NULL)) {
                        goto ProcessResponse;
                    }
                }
            }
        }
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

ProcessResponse:
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    outStatusCode = static_cast<int>(statusCode);

    outResponseBody.clear();
    DWORD bytesAvailable = 0;
    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
        std::vector<char> buf(bytesAvailable);
        DWORD bytesRead = 0;
        if (WinHttpReadData(hRequest, buf.data(), bytesAvailable, &bytesRead) && bytesRead > 0) {
            outResponseBody.append(buf.data(), bytesRead);
        } else {
            break;
        }
    }

    LogDebug("sendWinHttpRequest: completed with statusCode=" + std::to_string(statusCode) + " resp=" + outResponseBody);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return true;
}

NetworkEngine::NetworkEngine() {
    LogDebug("NetworkEngine constructor start");
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    settings = ConfigManager::load();
    LogDebug("NetworkEngine: calling GetOrCreateClientCert");
    GetOrCreateClientCert();
    LogDebug("NetworkEngine: GetOrCreateClientCert done");
}

NetworkEngine::~NetworkEngine() {
    stop();
    WSACleanup();
}

void NetworkEngine::updateSettings(const AppSettings& newSettings) {
    std::lock_guard<std::mutex> lock(settingsMutex);
    settings = newSettings;
}

void NetworkEngine::setFavoriteFingerprints(const std::vector<std::string>& fps) {
    std::lock_guard<std::mutex> lock(favoritesMutex);
    favoriteFingerprints.clear();
    for (auto const& fp : fps) {
        if (!fp.empty()) favoriteFingerprints.insert(fp);
    }
}

std::string NetworkEngine::getDeviceFingerprint() {
    if (g_deviceFingerprint.empty()) {
        GetOrCreateClientCert();
    }
    return g_deviceFingerprint;
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

        std::string myFp = g_deviceFingerprint.empty() ? "BLACKHOLE_DEVICE" : g_deviceFingerprint;

        // 1. Native BlackHole Broadcast
        JsonValue json = JsonValue::Object();
        json.set("app", "BlackHole");
        json.set("version", "1.0");
        json.set("alias", alias);
        json.set("fingerprint", myFp);
        json.set("port", port);
        json.set("deviceType", "desktop");
        json.set("protocol", "blackhole");

        std::string msg = json.serialize();

        // 2. Multicast Beacon
        sockaddr_in lsDest;
        lsDest.sin_family = AF_INET;
        lsDest.sin_port = htons(53317);
        inet_pton(AF_INET, "224.0.0.167", &lsDest.sin_addr);

        JsonValue lsJson = JsonValue::Object();
        lsJson.set("app", "BlackHole");
        lsJson.set("alias", alias);
        lsJson.set("version", "2.2");
        lsJson.set("deviceModel", "Windows");
        lsJson.set("deviceType", "desktop");
        lsJson.set("fingerprint", myFp);
        lsJson.set("port", port);
        lsJson.set("protocol", "http");
        lsJson.set("download", true);
        lsJson.set("announcement", true);

        std::string lsMsg = lsJson.serialize();

        // Pin to each local interface to ensure outgoing multicast on Wi-Fi hotspot
        auto localIps = getLocalIPAddresses();
        for (const auto& lip : localIps) {
            in_addr ifAddr;
            if (inet_pton(AF_INET, lip.c_str(), &ifAddr) == 1) {
                setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, (char*)&ifAddr, sizeof(ifAddr));
                sendto(sock, lsMsg.c_str(), static_cast<int>(lsMsg.size()), 0, (sockaddr*)&lsDest, sizeof(lsDest));
                sendto(sock, msg.c_str(), static_cast<int>(msg.size()), 0, (sockaddr*)&lsDest, sizeof(lsDest));
            }

            // Subnet broadcast (e.g. 192.168.43.255 for 192.168.43.166)
            size_t lastDot = lip.rfind('.');
            if (lastDot != std::string::npos) {
                std::string subnetBcast = lip.substr(0, lastDot) + ".255";
                sockaddr_in subDest;
                subDest.sin_family = AF_INET;
                subDest.sin_port = htons(53317);
                inet_pton(AF_INET, subnetBcast.c_str(), &subDest.sin_addr);
                sendto(sock, msg.c_str(), static_cast<int>(msg.size()), 0, (sockaddr*)&subDest, sizeof(subDest));
                sendto(sock, lsMsg.c_str(), static_cast<int>(lsMsg.size()), 0, (sockaddr*)&subDest, sizeof(subDest));
            }
        }

        // Global broadcast 255.255.255.255
        sendto(sock, msg.c_str(), static_cast<int>(msg.size()), 0, (sockaddr*)&destAddr, sizeof(destAddr));
        sendto(sock, lsMsg.c_str(), static_cast<int>(lsMsg.size()), 0, (sockaddr*)&destAddr, sizeof(destAddr));

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

    // Join LocalSend Multicast Group 224.0.0.167 on INADDR_ANY and ALL local interfaces
    ip_mreq mreq;
    inet_pton(AF_INET, "224.0.0.167", &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&mreq, sizeof(mreq));

    auto localIps = getLocalIPAddresses();
    for (const auto& lip : localIps) {
        ip_mreq m;
        inet_pton(AF_INET, "224.0.0.167", &m.imr_multiaddr);
        inet_pton(AF_INET, lip.c_str(), &m.imr_interface);
        setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&m, sizeof(m));
    }

    char buffer[65536];
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
            auto localIpsList = getLocalIPAddresses();
            bool isSelf = false;
            for (auto const& myIp : localIpsList) {
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
                std::string fpStr = val.get("fingerprint").asString();
                std::string proto = "blackhole";

                bool isBlackHolePeer = (appName == "BlackHole") || (fpStr.find("blackhole") != std::string::npos);
                bool isHttps = false;

                if (isBlackHolePeer) {
                    proto = "blackhole";
                    if (alias.empty()) alias = "BlackHole Device";
                } else if (val.has("version") && (val.has("protocol") || val.has("fingerprint") || val.has("deviceModel"))) {
                    proto = "localsend";
                    if (alias.empty()) alias = "LocalSend Device";
                    if (devType == "mobile") devType = "phone";
                    isHttps = (val.get("protocol").asString("https") == "https");
                } else {
                    alias = "";
                }

                if (!alias.empty()) {
                    PeerDevice peer;
                    peer.ip = ip;
                    peer.port = port;
                    peer.alias = alias;
                    peer.deviceModel = val.get("deviceModel").asString("Device");
                    peer.deviceType = devType;
                    peer.fingerprint = fpStr;
                    peer.protocol = proto;
                    peer.isHttps = isHttps;
                    peer.lastSeen = getCurrentTimeMs();

                    {
                        std::lock_guard<std::mutex> lock(peersMutex);
                        discoveredPeers[ip] = peer;
                    }

                    if (onPeersChanged) {
                        onPeersChanged(getPeers());
                    }

                    // Respond to announcements with register
                    bool isAnnouncement = val.get("announcement").asBool(false);
                    if (isAnnouncement && proto == "localsend") {
                        std::string targetIp = ip;
                        int targetPort = port;
                        bool targetHttps = isHttps;
                        std::thread([this, targetIp, targetPort, targetHttps]() {
                            std::string myAlias;
                            int myPort;
                            {
                                std::lock_guard<std::mutex> lock(settingsMutex);
                                myAlias = settings.alias;
                                myPort = settings.port;
                            }
                            std::string myFp = g_deviceFingerprint.empty() ? "BLACKHOLE_DEVICE" : g_deviceFingerprint;
                            JsonValue regObj = JsonValue::Object();
                            regObj.set("alias", myAlias);
                            regObj.set("version", "2.2");
                            regObj.set("deviceModel", "Windows");
                            regObj.set("deviceType", "desktop");
                            regObj.set("fingerprint", myFp);
                            regObj.set("port", myPort);
                            regObj.set("protocol", "http");
                            regObj.set("download", true);
                            std::string regStr = regObj.serialize();

                            int outStatus = 0;
                            std::string outResp;
                            if (!sendWinHttpRequest(targetIp, targetPort, targetHttps, L"POST", L"/api/localsend/v2/register", regStr, L"application/json", outStatus, outResp)) {
                                sendWinHttpRequest(targetIp, targetPort, !targetHttps, L"POST", L"/api/localsend/v2/register", regStr, L"application/json", outStatus, outResp);
                            }
                        }).detach();
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
            try {
                handleIncomingConnection(static_cast<uintptr_t>(clientSock), ip);
            } catch (...) {
                closesocket(clientSock);
            }
        }).detach();
    }

    closesocket(listenSock);
}

void NetworkEngine::handleIncomingConnection(uintptr_t clientSockRaw, std::string clientIp) {
    SOCKET sock = static_cast<SOCKET>(clientSockRaw);

    // Disable Nagle algorithm on incoming socket to eliminate latency jitter
    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));

    // 0. Peek first 4 bytes to detect HTTP requests from browsers/phones
    char peekBuf[4] = {0};
    int p = recv(sock, peekBuf, 4, MSG_PEEK);
    if (p < 4) {
        closesocket(sock);
        return;
    }

    if (memcmp(peekBuf, "GET ", 4) == 0 || memcmp(peekBuf, "POST", 4) == 0 ||
        memcmp(peekBuf, "HEAD", 4) == 0 || memcmp(peekBuf, "OPTI", 4) == 0) {
        handleHttpRequest(clientSockRaw, clientIp);
        return;
    }

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

    std::vector<std::string> fnames;
    for (auto const& fObj : filesArr.arrVal) {
        fnames.push_back(fObj.get("name").asString("file"));
    }

    std::string destDir;
    bool autoAccept = false;
    {
        std::lock_guard<std::mutex> lock(settingsMutex);
        destDir = settings.destinationFolder;
        if (settings.quickSaveMode == 2 || settings.quickSave) {
            autoAccept = true;
        } else if (settings.quickSaveMode == 1) {
            std::string senderFp = header.get("fingerprint").asString();
            std::lock_guard<std::mutex> favLock(favoritesMutex);
            if (!senderFp.empty() && favoriteFingerprints.count(senderFp) > 0) {
                autoAccept = true;
            }
        }
    }

    // Wait for user prompt if not auto-accepting
    if (!autoAccept) {
        pendingTransferAccepted = false;
        pendingTransferRejected = false;

        TransferEvent ev;
        ev.type = "incoming_request";
        ev.peerIp = clientIp;
        ev.peerAlias = senderAlias;
        ev.totalBytes = totalBytes;
        ev.fileCount = static_cast<int>(filesArr.arrVal.size());
        ev.fileNames = fnames;
        ev.message = "Incoming transfer request from " + senderAlias;
        if (onTransferEvent) onTransferEvent(ev);

        // Wait up to 60 seconds for user action
        int elapsed = 0;
        while (!pendingTransferAccepted && !pendingTransferRejected && elapsed < 600 && isRunning) {
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
    int64_t lastProgTime = getCurrentTimeMs();
    int64_t lastProgBytes = 0;
    double smoothedSpeedMBs = 0.0;
    const size_t CHUNK_SIZE = 256 * 1024; // 256 KB chunk buffer for smooth wire-speed transfer
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

            int64_t now = getCurrentTimeMs();
            if (now - lastProgTime >= 80 || fileBytesReceived >= fileSize || totalBytesReceived >= totalBytes) {
                int64_t timeDeltaMs = now - lastProgTime;
                if (timeDeltaMs > 0) {
                    int64_t bytesDelta = totalBytesReceived - lastProgBytes;
                    double instantSpeed = (bytesDelta / (1024.0 * 1024.0)) / (timeDeltaMs / 1000.0);
                    smoothedSpeedMBs = (smoothedSpeedMBs <= 0.01) ? instantSpeed : (smoothedSpeedMBs * 0.75 + instantSpeed * 0.25);
                }
                lastProgTime = now;
                lastProgBytes = totalBytesReceived;

                int pct = (totalBytes > 0) ? static_cast<int>((totalBytesReceived * 100) / totalBytes) : 100;

                TransferEvent prog;
                prog.type = "progress";
                prog.peerIp = clientIp;
                prog.peerAlias = "Receiving from " + senderAlias;
                prog.currentFileName = fileName;
                prog.bytesTransferred = totalBytesReceived;
                prog.totalBytes = totalBytes;
                prog.speedMBs = smoothedSpeedMBs;
                prog.progressPercent = pct;
                if (onTransferEvent) onTransferEvent(prog);
            }
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

    std::string textContent = "";
    if (filesArr.arrVal.size() == 1) {
        std::string fname = filesArr.arrVal[0].get("name").asString();
        if ((fname.find("Shared_Note") != std::string::npos || fname.find("note_") != std::string::npos || fname.find("clipboard_") != std::string::npos || (fname.size() >= 4 && fname.rfind(".txt") == fname.size() - 4)) && totalBytes < 65536) {
            std::filesystem::path fp = std::filesystem::path(destDir) / fname;
            if (std::filesystem::exists(fp)) {
                std::ifstream tf(fp);
                if (tf.is_open()) {
                    textContent = std::string((std::istreambuf_iterator<char>(tf)), std::istreambuf_iterator<char>());
                }
            }
        }
    }

    TransferEvent done;
    done.type = "complete";
    done.peerIp = clientIp;
    done.peerAlias = "Received from " + senderAlias;
    done.currentFileName = (filesArr.arrVal.size() == 1) ? filesArr.arrVal[0].get("name").asString() : std::to_string(filesArr.arrVal.size()) + " files";
    done.totalBytes = totalBytes;
    done.progressPercent = 100;
    done.textContent = textContent;
    done.message = !textContent.empty() ? textContent : "Transfer completed successfully!";
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
    SOCKET s = (SOCKET)activeClientSock.exchange((uintptr_t)(~0ULL));
    if (s != INVALID_SOCKET) {
        shutdown(s, SD_BOTH);
        closesocket(s);
    }
}

bool NetworkEngine::sendFilesToPeer(const std::string& peerIp, int peerPort, const std::vector<std::string>& filePaths, const std::string& targetAlias, const std::string& protocol) {
    if (clientTransferThread.joinable()) {
        clientTransferThread.join();
    }

    cancelCurrentTransfer = false;

    clientTransferThread = std::thread([this, peerIp, peerPort, filePaths, targetAlias, protocol]() {
        try {
            std::string displayTarget = !targetAlias.empty() ? targetAlias : peerIp;
            std::string effectiveProtocol = protocol;
            {
                std::lock_guard<std::mutex> lock(peersMutex);
                if (discoveredPeers.count(peerIp) && !discoveredPeers[peerIp].protocol.empty()) {
                    effectiveProtocol = discoveredPeers[peerIp].protocol;
                }
            }

        // Gather file metadata first
        JsonValue filesArr = JsonValue::Array();
        int64_t totalBytes = 0;
        for (auto const& pathStr : filePaths) {
            std::filesystem::path p(pathStr);
            if (!std::filesystem::exists(p)) {
                char tempPath[MAX_PATH];
                GetTempPathA(MAX_PATH, tempPath);
                char longTemp[MAX_PATH];
                if (GetLongPathNameA(tempPath, longTemp, MAX_PATH) > 0) strcpy_s(tempPath, longTemp);
                std::filesystem::path alt = std::filesystem::path(tempPath) / "BlackHole" / p.filename();
                if (std::filesystem::exists(alt)) p = alt;
            }
            if (!std::filesystem::exists(p)) continue;
            int64_t sz = std::filesystem::file_size(p);
            totalBytes += sz;

            JsonValue f = JsonValue::Object();
            f.set("name", p.filename().string());
            f.set("size", sz);
            filesArr.push(f);
        }

        // Emit initial status: Connecting and waiting for receiver to accept
        TransferEvent startEv;
        startEv.type = "progress";
        startEv.peerIp = peerIp;
        startEv.peerAlias = "Sending to " + displayTarget;
        startEv.currentFileName = "Waiting for " + displayTarget + " to accept...";
        startEv.bytesTransferred = 0;
        startEv.totalBytes = totalBytes;
        startEv.speedMBs = 0.0;
        startEv.progressPercent = 0;
        if (onTransferEvent) onTransferEvent(startEv);

        if (effectiveProtocol == "localsend") {
            // LocalSend HTTP/HTTPS Transfer Flow via WinHTTP
            std::string myAlias;
            {
                std::lock_guard<std::mutex> lock(settingsMutex);
                myAlias = settings.alias;
            }

            bool hasPreviewText = false;
            JsonValue lsFiles = JsonValue::Object();
            for (size_t i = 0; i < filePaths.size(); ++i) {
                std::filesystem::path p(filePaths[i]);
                if (!std::filesystem::exists(p)) {
                    char tempPath[MAX_PATH];
                    GetTempPathA(MAX_PATH, tempPath);
                    char longTemp[MAX_PATH];
                    if (GetLongPathNameA(tempPath, longTemp, MAX_PATH) > 0) strcpy_s(tempPath, longTemp);
                    std::filesystem::path alt = std::filesystem::path(tempPath) / "BlackHole" / p.filename();
                    if (std::filesystem::exists(alt)) {
                        p = alt;
                    }
                }
                std::string fid = "file_" + std::to_string(i);
                JsonValue fDto = JsonValue::Object();
                fDto.set("id", fid);
                fDto.set("fileName", p.filename().string());
                int64_t fsz = std::filesystem::exists(p) ? static_cast<int64_t>(std::filesystem::file_size(p)) : 0;
                fDto.set("size", fsz);

                std::string ext = p.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                bool isText = ext == ".txt" || ext == ".md" || ext == ".json" || ext == ".csv" ||
                              ext == ".log" || ext == ".xml" || ext == ".ini" || ext == ".conf" ||
                              p.filename().string().find("note_") != std::string::npos ||
                              p.filename().string().find("clipboard_") != std::string::npos;

                std::string ft = "application/octet-stream";
                if (isText) {
                    ft = "text/plain";
                } else if (ext == ".jpg" || ext == ".jpeg") {
                    ft = "image/jpeg";
                } else if (ext == ".png") {
                    ft = "image/png";
                } else if (ext == ".gif") {
                    ft = "image/gif";
                } else if (ext == ".webp") {
                    ft = "image/webp";
                } else if (ext == ".bmp") {
                    ft = "image/bmp";
                } else if (ext == ".mp4") {
                    ft = "video/mp4";
                } else if (ext == ".mkv") {
                    ft = "video/x-matroska";
                } else if (ext == ".avi") {
                    ft = "video/x-msvideo";
                } else if (ext == ".mov") {
                    ft = "video/quicktime";
                } else if (ext == ".pdf") {
                    ft = "application/pdf";
                } else if (ext == ".apk") {
                    ft = "application/vnd.android.package-archive";
                }
                fDto.set("fileType", ft);

                if (isText && fsz > 0 && fsz < 512 * 1024 && std::filesystem::exists(p)) {
                    std::ifstream tf(p.wstring(), std::ios::binary);
                    std::string txt((std::istreambuf_iterator<char>(tf)), std::istreambuf_iterator<char>());
                    fDto.set("preview", txt);
                    hasPreviewText = true;
                }
                lsFiles.set(fid, fDto);
            }

            bool isHttps = true;
            {
                std::lock_guard<std::mutex> lock(peersMutex);
                if (discoveredPeers.count(peerIp)) {
                    isHttps = discoveredPeers[peerIp].isHttps;
                }
            }

            std::string myFp = g_deviceFingerprint.empty() ? "BLACKHOLE_DEVICE" : g_deviceFingerprint;
            JsonValue prepJson = JsonValue::Object();
            JsonValue infoObj = JsonValue::Object();
            infoObj.set("alias", myAlias);
            infoObj.set("version", "2.2");
            infoObj.set("deviceModel", "Windows");
            infoObj.set("deviceType", "desktop");
            infoObj.set("fingerprint", myFp);
            infoObj.set("port", 53317);
            infoObj.set("protocol", "http");
            infoObj.set("download", true);
            prepJson.set("info", infoObj);
            prepJson.set("files", lsFiles);

            std::string body = prepJson.serialize();

            int statusCode = 0;
            std::string respBody;
            bool ok = sendWinHttpRequest(peerIp, peerPort, isHttps, L"POST", L"/api/localsend/v2/prepare-upload", body, L"application/json", statusCode, respBody);
            if (!ok || statusCode == 0) {
                // Try fallback protocol
                isHttps = !isHttps;
                infoObj.set("protocol", "http");
                prepJson.set("info", infoObj);
                body = prepJson.serialize();
                ok = sendWinHttpRequest(peerIp, peerPort, isHttps, L"POST", L"/api/localsend/v2/prepare-upload", body, L"application/json", statusCode, respBody);
            }
            LogDebug("sendFilesToPeer: prepare-upload result ok=" + std::to_string(ok) + " statusCode=" + std::to_string(statusCode) + " resp=" + respBody);

            if (!ok || (statusCode != 200 && statusCode != 204)) {
                TransferEvent ev;
                ev.type = "error";
                ev.peerIp = peerIp;
                ev.peerAlias = "Sending to " + displayTarget;
                ev.message = (statusCode == 403) ? (displayTarget + " declined the transfer.") : ("Could not connect to " + displayTarget);
                if (onTransferEvent) onTransferEvent(ev);
                return;
            }

            // LocalSend text messages complete on 204 No Content!
            if (statusCode == 204 || (hasPreviewText && filePaths.size() == 1)) {
                JsonValue testJson = JsonParser::parse(respBody);
                if (statusCode == 204 || testJson.get("files").objVal.empty()) {
                    TransferEvent done;
                    done.type = "complete";
                    done.peerIp = peerIp;
                    done.peerAlias = "Sent to " + displayTarget;
                    done.currentFileName = (filePaths.size() == 1) ? std::filesystem::path(filePaths[0]).filename().string() : "Message delivered";
                    done.totalBytes = totalBytes;
                    done.progressPercent = 100;
                    done.message = "Message delivered to " + displayTarget;
                    if (onTransferEvent) onTransferEvent(done);
                    return;
                }
            }

            JsonValue respJson = JsonParser::parse(respBody);
            std::string sessionId = respJson.get("sessionId").asString();
            JsonValue remoteFiles = respJson.get("files");

            if (remoteFiles.objVal.empty()) {
                TransferEvent done;
                done.type = "complete";
                done.peerIp = peerIp;
                done.peerAlias = "Sent to " + displayTarget;
                done.currentFileName = (filePaths.size() == 1) ? std::filesystem::path(filePaths[0]).filename().string() : "Message delivered";
                done.totalBytes = totalBytes;
                done.progressPercent = 100;
                done.message = "Message delivered to " + displayTarget;
                if (onTransferEvent) onTransferEvent(done);
                return;
            }

            // Upload each authorized file to /api/localsend/v2/upload
            int64_t totalUploadedSoFar = 0;
            int64_t overallStartTime = getCurrentTimeMs();
            bool anyUploadFailed = false;

            for (auto const& pair : remoteFiles.objVal) {
                if (cancelCurrentTransfer) break;
                std::string fid = pair.first;
                std::string token = pair.second.asString();

                std::filesystem::path targetFile;
                for (size_t i = 0; i < filePaths.size(); ++i) {
                    if ("file_" + std::to_string(i) == fid) {
                        targetFile = filePaths[i];
                        if (!std::filesystem::exists(targetFile)) {
                            char tempPath[MAX_PATH];
                            GetTempPathA(MAX_PATH, tempPath);
                            char longTemp[MAX_PATH];
                            if (GetLongPathNameA(tempPath, longTemp, MAX_PATH) > 0) strcpy_s(tempPath, longTemp);
                            std::filesystem::path alt = std::filesystem::path(tempPath) / "BlackHole" / targetFile.filename();
                            if (std::filesystem::exists(alt)) targetFile = alt;
                        }
                        break;
                    }
                }
                if (targetFile.empty() || !std::filesystem::exists(targetFile)) {
                    LogDebug("Upload: targetFile not found for fid=" + fid);
                    continue;
                }

                int64_t fsz = static_cast<int64_t>(std::filesystem::file_size(targetFile));
                std::string uploadPath = "/api/localsend/v2/upload?sessionId=" + sessionId + "&fileId=" + fid + "&token=" + token;
                std::wstring wUploadPath(uploadPath.begin(), uploadPath.end());

                LogDebug("Upload starting for fid=" + fid + " size=" + std::to_string(fsz) + " file=" + targetFile.filename().string());

                // If file is within 16 MB (including all notes, text, clipboard, and common docs/images),
                // use sendWinHttpRequest which reliably completes mTLS handshake, attaches client cert,
                // bypasses revocation checks, and handles responses.
                if (fsz <= 16 * 1024 * 1024) {
                    std::ifstream inFile(targetFile.wstring(), std::ios::binary);
                    std::string fileData;
                    if (fsz > 0) {
                        fileData.resize(static_cast<size_t>(fsz));
                        inFile.read(&fileData[0], fsz);
                    }
                    inFile.close();

                    int upStatus = 0;
                    std::string upResp;
                    bool upOk = sendWinHttpRequest(peerIp, peerPort, isHttps, L"POST", wUploadPath, fileData, L"application/octet-stream", upStatus, upResp);
                    LogDebug("Upload file " + fid + " completed: upOk=" + std::to_string(upOk) + " statusCode=" + std::to_string(upStatus) + " resp=" + upResp);

                    if (!upOk || (upStatus != 200 && upStatus != 204)) {
                        anyUploadFailed = true;
                        break;
                    }

                    totalUploadedSoFar += fsz;
                    int pct = (totalBytes > 0) ? static_cast<int>((totalUploadedSoFar * 100) / totalBytes) : 100;

                    TransferEvent prog;
                    prog.type = "progress";
                    prog.peerIp = peerIp;
                    prog.peerAlias = "Sending to " + displayTarget;
                    prog.currentFileName = targetFile.filename().string();
                    prog.bytesTransferred = totalUploadedSoFar;
                    prog.totalBytes = totalBytes;
                    prog.speedMBs = 5.0;
                    prog.progressPercent = pct;
                    if (onTransferEvent) onTransferEvent(prog);
                } else {
                    // For files > 16 MB, stream chunks
                    std::wstring wHost(peerIp.begin(), peerIp.end());
                    HINTERNET hSession = WinHttpOpen(L"BlackHole/2.2", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
                    if (!hSession) { anyUploadFailed = true; break; }
                    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
                    WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
                    WinHttpSetTimeouts(hSession, 4000, 4000, 60000, 60000);
                    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), static_cast<INTERNET_PORT>(peerPort), 0);
                    if (!hConnect) { WinHttpCloseHandle(hSession); anyUploadFailed = true; break; }

                    DWORD reqFlags = isHttps ? WINHTTP_FLAG_SECURE : 0;
                    HINTERNET hUpReq = WinHttpOpenRequest(hConnect, L"POST", wUploadPath.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, reqFlags);
                    if (isHttps) {
                        DWORD secFlags = SECURITY_FLAG_IGNORE_ALL_CERT_ERRORS;
                        WinHttpSetOption(hUpReq, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
                        DWORD ignoreRevOffline = TRUE;
                        WinHttpSetOption(hUpReq, 155, &ignoreRevOffline, sizeof(ignoreRevOffline));
                    }

                    std::ifstream inFile(targetFile.wstring(), std::ios::binary);
                    if (!inFile.is_open()) {
                        WinHttpCloseHandle(hUpReq);
                        WinHttpCloseHandle(hConnect);
                        WinHttpCloseHandle(hSession);
                        anyUploadFailed = true;
                        break;
                    }

                    std::wstring upHeader = L"Content-Type: application/octet-stream\r\n";
                    BOOL sentReq = WinHttpSendRequest(hUpReq, upHeader.c_str(), static_cast<DWORD>(upHeader.size()), WINHTTP_NO_REQUEST_DATA, 0, static_cast<DWORD>(fsz), 0);
                    if (!sentReq && isHttps && GetLastError() == ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED) {
                        PCCERT_CONTEXT pCert = GetOrCreateClientCert();
                        if (pCert) {
                            DWORD secFlags = SECURITY_FLAG_IGNORE_ALL_CERT_ERRORS;
                            WinHttpSetOption(hUpReq, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
                            DWORD ignoreRevOffline = TRUE;
                            WinHttpSetOption(hUpReq, 155, &ignoreRevOffline, sizeof(ignoreRevOffline));
                            WinHttpSetOption(hUpReq, WINHTTP_OPTION_CLIENT_CERT_CONTEXT, (LPVOID)pCert, sizeof(CERT_CONTEXT));
                            sentReq = WinHttpSendRequest(hUpReq, upHeader.c_str(), static_cast<DWORD>(upHeader.size()), WINHTTP_NO_REQUEST_DATA, 0, static_cast<DWORD>(fsz), 0);
                        }
                    }

                    if (!sentReq) {
                        LogDebug("Upload streaming WinHttpSendRequest failed, err=" + std::to_string(GetLastError()));
                        inFile.close();
                        WinHttpCloseHandle(hUpReq);
                        WinHttpCloseHandle(hConnect);
                        WinHttpCloseHandle(hSession);
                        anyUploadFailed = true;
                        break;
                    }

                    std::vector<char> sendBuf(256 * 1024);
                    int64_t fileSent = 0;
                    int64_t lastProgTime = getCurrentTimeMs();
                    int64_t lastProgBytes = totalUploadedSoFar;
                    double smoothedSpeedMBs = 0.0;
                    while (fileSent < fsz && !cancelCurrentTransfer && isRunning) {
                        int toRead = (fsz - fileSent < static_cast<int64_t>(sendBuf.size())) ? static_cast<int>(fsz - fileSent) : static_cast<int>(sendBuf.size());
                        inFile.read(sendBuf.data(), toRead);
                        int readBytes = static_cast<int>(inFile.gcount());
                        if (readBytes <= 0) break;

                        DWORD written = 0;
                        if (!WinHttpWriteData(hUpReq, sendBuf.data(), readBytes, &written) || written == 0) break;
                        fileSent += written;
                        totalUploadedSoFar += written;

                        int64_t now = getCurrentTimeMs();
                        if (now - lastProgTime >= 80 || fileSent >= fsz || totalUploadedSoFar >= totalBytes) {
                            int64_t timeDeltaMs = now - lastProgTime;
                            if (timeDeltaMs > 0) {
                                int64_t bytesDelta = totalUploadedSoFar - lastProgBytes;
                                double instantSpeed = (bytesDelta / (1024.0 * 1024.0)) / (timeDeltaMs / 1000.0);
                                smoothedSpeedMBs = (smoothedSpeedMBs <= 0.01) ? instantSpeed : (smoothedSpeedMBs * 0.75 + instantSpeed * 0.25);
                            }
                            lastProgTime = now;
                            lastProgBytes = totalUploadedSoFar;

                            int pct = (totalBytes > 0) ? static_cast<int>((totalUploadedSoFar * 100) / totalBytes) : 100;

                            TransferEvent prog;
                            prog.type = "progress";
                            prog.peerIp = peerIp;
                            prog.peerAlias = "Sending to " + displayTarget;
                            prog.currentFileName = targetFile.filename().string();
                            prog.bytesTransferred = totalUploadedSoFar;
                            prog.totalBytes = totalBytes;
                            prog.speedMBs = smoothedSpeedMBs;
                            prog.progressPercent = pct;
                            if (onTransferEvent) onTransferEvent(prog);
                        }
                    }
                    inFile.close();
                    WinHttpReceiveResponse(hUpReq, NULL);
                    DWORD upStreamStatus = 0;
                    DWORD statusSize = sizeof(upStreamStatus);
                    WinHttpQueryHeaders(hUpReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &upStreamStatus, &statusSize, WINHTTP_NO_HEADER_INDEX);
                    LogDebug("Upload streaming completed for fid=" + fid + " statusCode=" + std::to_string(upStreamStatus));
                    WinHttpCloseHandle(hUpReq);
                    WinHttpCloseHandle(hConnect);
                    WinHttpCloseHandle(hSession);

                    if (upStreamStatus != 200 && upStreamStatus != 204) {
                        anyUploadFailed = true;
                        break;
                    }
                }
            }

            if (cancelCurrentTransfer) {
                TransferEvent ev;
                ev.type = "error";
                ev.peerIp = peerIp;
                ev.peerAlias = "Sending to " + displayTarget;
                ev.message = "Transfer cancelled by user";
                if (onTransferEvent) onTransferEvent(ev);
                return;
            }

            if (anyUploadFailed) {
                TransferEvent ev;
                ev.type = "error";
                ev.peerIp = peerIp;
                ev.peerAlias = "Sending to " + displayTarget;
                ev.message = "Failed to upload file to " + displayTarget;
                if (onTransferEvent) onTransferEvent(ev);
                return;
            }

            TransferEvent done;
            done.type = "complete";
            done.peerIp = peerIp;
            done.peerAlias = "Sent to " + displayTarget;
            done.currentFileName = (filePaths.size() == 1) ? std::filesystem::path(filePaths[0]).filename().string() : std::to_string(filePaths.size()) + " files";
            done.totalBytes = totalBytes;
            done.progressPercent = 100;
            done.message = "Successfully sent to " + displayTarget;
            if (onTransferEvent) onTransferEvent(done);
            return;
        }

        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) {
            TransferEvent ev;
            ev.type = "error";
            ev.peerIp = peerIp;
            ev.peerAlias = "Sending to " + displayTarget;
            ev.message = "Failed to create socket.";
            if (onTransferEvent) onTransferEvent(ev);
            return;
        }

        activeClientSock = (uintptr_t)sock;

        // Set 15-second receive & send timeouts so transfers never hang indefinitely
        DWORD sockTimeout = 15000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&sockTimeout, sizeof(sockTimeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sockTimeout, sizeof(sockTimeout));

        // Set high socket buffer sizes for wire-speed LAN streaming
        int bufSize = 4 * 1024 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)&bufSize, sizeof(bufSize));

        // Disable Nagle algorithm to prevent choppy delayed ACKs with mobile devices
        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));

        sockaddr_in targetAddr;
        targetAddr.sin_family = AF_INET;
        targetAddr.sin_port = htons(peerPort);
        inet_pton(AF_INET, peerIp.c_str(), &targetAddr.sin_addr);

        if (connect(sock, (sockaddr*)&targetAddr, sizeof(targetAddr)) == SOCKET_ERROR) {
            SOCKET s = (SOCKET)activeClientSock.exchange((uintptr_t)(~0ULL));
            if (s != INVALID_SOCKET) closesocket(s);
            TransferEvent ev;
            ev.type = "error";
            ev.peerIp = peerIp;
            ev.peerAlias = "Sending to " + displayTarget;
            ev.message = "Could not connect to " + displayTarget;
            if (onTransferEvent) onTransferEvent(ev);
            return;
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
        if (r != sizeof(respLenNet) || cancelCurrentTransfer) {
            SOCKET s = (SOCKET)activeClientSock.exchange((uintptr_t)(~0ULL));
            if (s != INVALID_SOCKET) closesocket(s);
            TransferEvent ev;
            ev.type = "error";
            ev.peerIp = peerIp;
            ev.peerAlias = "Sending to " + displayTarget;
            ev.message = cancelCurrentTransfer ? "Transfer canceled." : (displayTarget + " did not respond.");
            if (onTransferEvent) onTransferEvent(ev);
            return;
        }

        uint32_t respLen = ntohl(respLenNet);
        std::string respStr(respLen, '\0');
        recv(sock, &respStr[0], respLen, MSG_WAITALL);
        JsonValue resp = JsonParser::parse(respStr);
        if (resp.get("status").asString() != "ACCEPTED" || cancelCurrentTransfer) {
            SOCKET s = (SOCKET)activeClientSock.exchange((uintptr_t)(~0ULL));
            if (s != INVALID_SOCKET) closesocket(s);
            TransferEvent ev;
            ev.type = "error";
            ev.peerIp = peerIp;
            ev.peerAlias = "Sending to " + displayTarget;
            ev.message = cancelCurrentTransfer ? "Transfer canceled." : (displayTarget + " declined the transfer.");
            if (onTransferEvent) onTransferEvent(ev);
            return;
        }

        // Stream file buffers
        int64_t totalBytesSent = 0;
        int64_t lastProgTime = getCurrentTimeMs();
        int64_t lastProgBytes = 0;
        double smoothedSpeedMBs = 0.0;
        const size_t CHUNK_SIZE = 256 * 1024; // 256 KB chunks for ultra-smooth streaming
        std::vector<char> buffer(CHUNK_SIZE);

        for (auto const& pathStr : filePaths) {
            std::filesystem::path p(pathStr);
            if (!std::filesystem::exists(p)) {
                char tempPath[MAX_PATH];
                GetTempPathA(MAX_PATH, tempPath);
                char longTemp[MAX_PATH];
                if (GetLongPathNameA(tempPath, longTemp, MAX_PATH) > 0) strcpy_s(tempPath, longTemp);
                std::filesystem::path alt = std::filesystem::path(tempPath) / "BlackHole" / p.filename();
                if (std::filesystem::exists(alt)) p = alt;
            }
            if (!std::filesystem::exists(p)) continue;
            int64_t fileSize = std::filesystem::file_size(p);
            std::string fileName = p.filename().string();

            std::ifstream inFile(p.wstring(), std::ios::binary);
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
                    ev.peerIp = peerIp;
                    ev.peerAlias = "Sending to " + displayTarget;
                    ev.message = "Transfer connection lost.";
                    if (onTransferEvent) onTransferEvent(ev);
                    return;
                }

                fileBytesSent += sent;
                totalBytesSent += sent;

                int64_t now = getCurrentTimeMs();
                if (now - lastProgTime >= 80 || fileBytesSent >= fileSize || totalBytesSent >= totalBytes) {
                    int64_t timeDeltaMs = now - lastProgTime;
                    if (timeDeltaMs > 0) {
                        int64_t bytesDelta = totalBytesSent - lastProgBytes;
                        double instantSpeed = (bytesDelta / (1024.0 * 1024.0)) / (timeDeltaMs / 1000.0);
                        smoothedSpeedMBs = (smoothedSpeedMBs <= 0.01) ? instantSpeed : (smoothedSpeedMBs * 0.75 + instantSpeed * 0.25);
                    }
                    lastProgTime = now;
                    lastProgBytes = totalBytesSent;

                    int pct = (totalBytes > 0) ? static_cast<int>((totalBytesSent * 100) / totalBytes) : 100;

                    TransferEvent prog;
                    prog.type = "progress";
                    prog.peerIp = peerIp;
                    prog.peerAlias = "Sending to " + displayTarget;
                    prog.currentFileName = fileName;
                    prog.bytesTransferred = totalBytesSent;
                    prog.totalBytes = totalBytes;
                    prog.speedMBs = smoothedSpeedMBs;
                    prog.progressPercent = pct;
                    if (onTransferEvent) onTransferEvent(prog);
                }
            }

            inFile.close();
            if (cancelCurrentTransfer) break;
        }

        // Gracefully await receiver's completion ack (up to 2 seconds) so receiver can flush
        if (!cancelCurrentTransfer) {
            shutdown(sock, SD_SEND);
            DWORD ackTimeout = 2000;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ackTimeout, sizeof(ackTimeout));
            uint32_t compLenNet = 0;
            int cr = recv(sock, (char*)&compLenNet, sizeof(compLenNet), MSG_WAITALL);
            if (cr == sizeof(compLenNet)) {
                uint32_t compLen = ntohl(compLenNet);
                if (compLen > 0 && compLen < 65536) {
                    std::string compStr(compLen, '\0');
                    recv(sock, &compStr[0], compLen, MSG_WAITALL);
                    LogDebug("TCP transfer peer responded: " + compStr);
                }
            }
        }

        SOCKET s = (SOCKET)activeClientSock.exchange((uintptr_t)(~0ULL));
        if (s != INVALID_SOCKET) {
            closesocket(s);
        }

        if (cancelCurrentTransfer) {
            TransferEvent ev;
            ev.type = "error";
            ev.peerIp = peerIp;
            ev.peerAlias = "Sending to " + displayTarget;
            ev.message = "Transfer canceled.";
            if (onTransferEvent) onTransferEvent(ev);
        } else {
            TransferEvent done;
            done.type = "complete";
            done.peerIp = peerIp;
            done.peerAlias = "Sent to " + displayTarget;
            done.currentFileName = (filePaths.size() == 1) ? std::filesystem::path(filePaths[0]).filename().string() : std::to_string(filePaths.size()) + " files";
            done.totalBytes = totalBytes;
            done.progressPercent = 100;
            done.message = "Successfully sent to " + displayTarget;
            if (onTransferEvent) onTransferEvent(done);
        }
        } catch (const std::exception& e) {
            SOCKET s = (SOCKET)activeClientSock.exchange((uintptr_t)(~0ULL));
            if (s != INVALID_SOCKET) closesocket(s);
            TransferEvent ev;
            ev.type = "error";
            ev.peerIp = peerIp;
            ev.peerAlias = !targetAlias.empty() ? targetAlias : peerIp;
            ev.message = std::string("Transfer error: ") + e.what();
            if (onTransferEvent) onTransferEvent(ev);
        } catch (...) {
            SOCKET s = (SOCKET)activeClientSock.exchange((uintptr_t)(~0ULL));
            if (s != INVALID_SOCKET) closesocket(s);
            TransferEvent ev;
            ev.type = "error";
            ev.peerIp = peerIp;
            ev.peerAlias = !targetAlias.empty() ? targetAlias : peerIp;
            ev.message = "Unexpected transfer error.";
            if (onTransferEvent) onTransferEvent(ev);
        }
    });

    return true;
}

void NetworkEngine::setSharedFiles(const std::vector<std::string>& files) {
    std::lock_guard<std::mutex> lock(sharedFilesMutex);
    sharedFiles = files;
}

std::vector<std::string> NetworkEngine::getSharedFiles() {
    std::lock_guard<std::mutex> lock(sharedFilesMutex);
    return sharedFiles;
}

static std::string HttpUrlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            int val = 0;
            std::istringstream is(in.substr(i + 1, 2));
            if (is >> std::hex >> val) {
                out += static_cast<char>(val);
                i += 2;
            } else {
                out += in[i];
            }
        } else if (in[i] == '+') {
            out += ' ';
        } else {
            out += in[i];
        }
    }
    return out;
}

void NetworkEngine::handleHttpRequest(uintptr_t clientSockRaw, const std::string& clientIp) {
    SOCKET sock = static_cast<SOCKET>(clientSockRaw);

    std::string headerData;
    char buf[1024];
    size_t headerEndPos = std::string::npos;

    while (headerData.size() < 65536) {
        int r = recv(sock, buf, sizeof(buf), 0);
        if (r <= 0) break;
        headerData.append(buf, r);
        headerEndPos = headerData.find("\r\n\r\n");
        if (headerEndPos != std::string::npos) break;
    }

    if (headerEndPos == std::string::npos) {
        closesocket(sock);
        return;
    }

    std::string headersPart = headerData.substr(0, headerEndPos);
    std::string bodyInitial = headerData.substr(headerEndPos + 4);

    std::istringstream hStream(headersPart);
    std::string requestLine;
    std::getline(hStream, requestLine);
    if (!requestLine.empty() && requestLine.back() == '\r') requestLine.pop_back();

    std::istringstream reqLineStream(requestLine);
    std::string method, fullPath, httpVer;
    reqLineStream >> method >> fullPath >> httpVer;

    std::map<std::string, std::string> headers;
    std::string line;
    while (std::getline(hStream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            while (!val.empty() && val.front() == ' ') val.erase(val.begin());
            for (auto& c : key) c = static_cast<char>(tolower(c));
            headers[key] = val;
        }
    }

    std::string path = fullPath;
    std::string query = "";
    auto qPos = fullPath.find('?');
    if (qPos != std::string::npos) {
        path = fullPath.substr(0, qPos);
        query = fullPath.substr(qPos + 1);
    }

    // CORS pre-flight
    if (method == "OPTIONS") {
        std::string resp = "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Headers: *\r\nConnection: close\r\n\r\n";
        send(sock, resp.c_str(), static_cast<int>(resp.size()), 0);
        closesocket(sock);
        return;
    }

    // 1. Serve Mobile Web Portal
    if (method == "GET" && (path == "/" || path == "/index.html" || path == "/web")) {
        std::filesystem::path htmlPath = std::filesystem::current_path() / "ui" / "web-offline.html";
        if (!std::filesystem::exists(htmlPath)) {
            htmlPath = std::filesystem::current_path() / ".." / "ui" / "web-offline.html";
        }
        std::string htmlContent = "";
        if (std::filesystem::exists(htmlPath)) {
            std::ifstream in(htmlPath, std::ios::binary);
            if (in.is_open()) {
                std::ostringstream ss;
                ss << in.rdbuf();
                htmlContent = ss.str();
            }
        }
        if (htmlContent.empty()) {
            htmlContent = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>BlackHole Local</title></head><body style='background:#09090b;color:#fff;font-family:sans-serif;text-align:center;padding:40px;'><h1>BlackHole Local Share</h1><p>Online on Local Hotspot</p></body></html>";
        }
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(htmlContent.size()) + "\r\nConnection: close\r\n\r\n" + htmlContent;
        send(sock, resp.c_str(), static_cast<int>(resp.size()), 0);
        closesocket(sock);
        return;
    }

    // GET /api/localsend/v2/info or /v1/info
    if (method == "GET" && (path == "/api/localsend/v2/info" || path == "/api/localsend/v1/info")) {
        std::string myAlias;
        {
            std::lock_guard<std::mutex> lock(settingsMutex);
            myAlias = settings.alias;
        }
        std::string myFp = g_deviceFingerprint.empty() ? "BLACKHOLE_DEVICE" : g_deviceFingerprint;
        JsonValue info = JsonValue::Object();
        info.set("alias", myAlias);
        info.set("version", "2.2");
        info.set("deviceModel", "Windows");
        info.set("deviceType", "desktop");
        info.set("fingerprint", myFp);
        info.set("download", true);
        std::string jsonStr = info.serialize();
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(jsonStr.size()) + "\r\nConnection: close\r\n\r\n" + jsonStr;
        send(sock, resp.c_str(), static_cast<int>(resp.size()), 0);
        closesocket(sock);
        return;
    }

    // LocalSend: POST /api/localsend/v2/register or /v1/register
    if (method == "POST" && (path == "/api/localsend/v2/register" || path == "/api/localsend/v1/register")) {
        std::string reqBody = bodyInitial;
        if (headers.count("content-length")) {
            size_t clen = static_cast<size_t>(std::atoll(headers["content-length"].c_str()));
            while (reqBody.size() < clen) {
                int r = recv(sock, buf, sizeof(buf), 0);
                if (r <= 0) break;
                reqBody.append(buf, r);
            }
        }
        JsonValue val = JsonParser::parse(reqBody);
        std::string peerAlias = val.get("alias").asString("Peer Device");
        int peerPort = val.get("port").asInt(53317);
        std::string devType = val.get("deviceType").asString("phone");
        std::string devModel = val.get("deviceModel").asString("Device");
        std::string peerFp = val.get("fingerprint").asString();
        std::string appName = val.get("app").asString();
        bool isBlackHolePeer = (appName == "BlackHole") || (peerFp.find("blackhole") != std::string::npos);

        PeerDevice peer;
        peer.ip = clientIp;
        peer.port = peerPort;
        peer.alias = peerAlias;
        peer.deviceModel = devModel;
        peer.deviceType = (devType == "mobile") ? "phone" : "desktop";
        peer.fingerprint = peerFp;
        peer.protocol = isBlackHolePeer ? "blackhole" : "localsend";
        peer.isHttps = (val.get("protocol").asString("https") == "https");
        peer.lastSeen = getCurrentTimeMs();

        {
            std::lock_guard<std::mutex> lock(peersMutex);
            discoveredPeers[clientIp] = peer;
        }
        if (onPeersChanged) onPeersChanged(getPeers());

        std::string myAlias;
        {
            std::lock_guard<std::mutex> lock(settingsMutex);
            myAlias = settings.alias;
        }
        std::string myFp = g_deviceFingerprint.empty() ? "BLACKHOLE_DEVICE" : g_deviceFingerprint;
        JsonValue respJson = JsonValue::Object();
        respJson.set("alias", myAlias);
        respJson.set("version", "2.2");
        respJson.set("deviceModel", "Windows");
        respJson.set("deviceType", "desktop");
        respJson.set("fingerprint", myFp);
        respJson.set("download", true);

        std::string respStr = respJson.serialize();
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(respStr.size()) + "\r\nConnection: close\r\n\r\n" + respStr;
        send(sock, resp.c_str(), static_cast<int>(resp.size()), 0);
        closesocket(sock);
        return;
    }

    // LocalSend: POST /api/localsend/v2/prepare-upload or /v1/send-request
    if (method == "POST" && (path == "/api/localsend/v2/prepare-upload" || path == "/api/localsend/v1/send-request")) {
        std::string reqBody = bodyInitial;
        if (headers.count("content-length")) {
            size_t clen = static_cast<size_t>(std::atoll(headers["content-length"].c_str()));
            while (reqBody.size() < clen) {
                int r = recv(sock, buf, sizeof(buf), 0);
                if (r <= 0) break;
                reqBody.append(buf, r);
            }
        }
        JsonValue req = JsonParser::parse(reqBody);
        JsonValue infoObj = req.get("info");
        std::string senderAlias = infoObj.get("alias").asString("Nearby User");
        JsonValue filesObj = req.get("files");

        // 1. Instant Text Message Check
        std::string instantText;
        for (auto const& pair : filesObj.objVal) {
            JsonValue f = pair.second;
            std::string ft = f.get("fileType").asString();
            if ((ft == "text" || ft == "text/plain" || ft.rfind("text/", 0) == 0) && f.has("preview")) {
                instantText = f.get("preview").asString();
                break;
            }
        }

        if (!instantText.empty()) {
            pendingTextAccepted = false;
            pendingTextRejected = false;

            TransferEvent ev;
            ev.type = "text_message";
            ev.peerIp = clientIp;
            ev.peerAlias = senderAlias;
            ev.message = instantText;
            ev.textContent = instantText;
            if (onTransferEvent) onTransferEvent(ev);

            bool autoAcceptText = false;
            {
                std::lock_guard<std::mutex> lock(settingsMutex);
                if (settings.autoCopyText || settings.quickSave || settings.quickSaveMode == 2) {
                    autoAcceptText = true;
                }
            }

            if (!autoAcceptText) {
                // Wait for user action (up to 60s)
                int waitMs = 0;
                while (!pendingTextAccepted && !pendingTextRejected && isRunning && waitMs < 60000) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    waitMs += 50;
                }
            }

            std::string resp = "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
            send(sock, resp.c_str(), static_cast<int>(resp.size()), 0);
            closesocket(sock);
            return;
        }

        // 2. Regular File Transfer Request
        int fileCount = static_cast<int>(filesObj.objVal.size());
        int64_t totalBytes = 0;
        std::vector<std::string> fnames;
        JsonValue respFiles = JsonValue::Object();
        std::string sessionId = "session_ls_" + std::to_string(getCurrentTimeMs());

        std::map<std::string, LocalSendFileInfo> sessionMap;
        for (auto const& pair : filesObj.objVal) {
            std::string fid = pair.first;
            std::string fn = pair.second.get("fileName").asString("file");
            int64_t sz = pair.second.get("size").asInt64(0);
            fnames.push_back(fn);
            totalBytes += sz;
            respFiles.set(fid, "token_" + fid);

            LocalSendFileInfo finfo;
            finfo.fileName = fn;
            finfo.size = sz;
            finfo.fileId = fid;
            sessionMap[fid] = finfo;
        }

        {
            std::lock_guard<std::mutex> lk(pendingUploadsMutex);
            pendingLocalSendUploads[sessionId] = sessionMap;
            sessionSenderAliases[sessionId] = senderAlias;
        }

        bool autoAccept = false;
        {
            std::lock_guard<std::mutex> lock(settingsMutex);
            if (settings.quickSaveMode == 2 || settings.quickSave) {
                autoAccept = true;
            } else if (settings.quickSaveMode == 1) {
                std::string senderFp = infoObj.get("fingerprint").asString();
                std::lock_guard<std::mutex> favLock(favoritesMutex);
                if (!senderFp.empty() && favoriteFingerprints.count(senderFp) > 0) {
                    autoAccept = true;
                }
            }
        }

        if (!autoAccept) {
            pendingTransferAccepted = false;
            pendingTransferRejected = false;

            TransferEvent ev;
            ev.type = "incoming_request";
            ev.peerIp = clientIp;
            ev.peerAlias = senderAlias;
            ev.totalBytes = totalBytes;
            ev.fileCount = fileCount;
            ev.fileNames = fnames;
            ev.message = "Incoming transfer request";
            if (onTransferEvent) onTransferEvent(ev);

            int elapsed = 0;
            while (!pendingTransferAccepted && !pendingTransferRejected && elapsed < 600 && isRunning) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                elapsed++;
            }

            if (!pendingTransferAccepted) {
                std::string resp = "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n{}";
                send(sock, resp.c_str(), static_cast<int>(resp.size()), 0);
                closesocket(sock);
                return;
            }
        }

        // Emit initial progress event so Windows UI automatically switches to Receive tab!
        TransferEvent startEv;
        startEv.type = "progress";
        startEv.peerIp = clientIp;
        startEv.peerAlias = "Receiving from " + senderAlias;
        startEv.currentFileName = fnames.empty() ? "File" : fnames[0];
        startEv.bytesTransferred = 0;
        startEv.totalBytes = totalBytes;
        startEv.fileCount = fileCount;
        startEv.fileNames = fnames;
        startEv.progressPercent = 0;
        if (onTransferEvent) onTransferEvent(startEv);

        JsonValue respJson = JsonValue::Object();
        respJson.set("sessionId", sessionId);
        respJson.set("files", respFiles);
        std::string respStr = respJson.serialize();
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(respStr.size()) + "\r\nConnection: close\r\n\r\n" + respStr;
        send(sock, resp.c_str(), static_cast<int>(resp.size()), 0);
        closesocket(sock);
        return;
    }

    // LocalSend: POST /api/localsend/v2/upload or /v1/send
    if (method == "POST" && (path == "/api/localsend/v2/upload" || path == "/api/localsend/v1/send")) {
        int64_t contentLength = 0;
        if (headers.count("content-length")) {
            contentLength = std::atoll(headers["content-length"].c_str());
        }

        auto parseQueryParam = [](const std::string& q, const std::string& key) -> std::string {
            size_t p = q.find(key + "=");
            if (p == std::string::npos) return "";
            size_t start = p + key.size() + 1;
            size_t end = q.find('&', start);
            return HttpUrlDecode(end == std::string::npos ? q.substr(start) : q.substr(start, end - start));
        };
        std::string sessionId = parseQueryParam(query, "sessionId");
        std::string fileId = parseQueryParam(query, "fileId");

        std::string fileName = "LocalSend_File_" + std::to_string(getCurrentTimeMs()) + ".bin";
        std::string senderAlias = "LocalSend User";
        int64_t expectedSize = contentLength;

        {
            std::lock_guard<std::mutex> lk(pendingUploadsMutex);
            if (!sessionId.empty() && pendingLocalSendUploads.count(sessionId)) {
                if (sessionSenderAliases.count(sessionId)) {
                    senderAlias = sessionSenderAliases[sessionId];
                }
                if (!fileId.empty() && pendingLocalSendUploads[sessionId].count(fileId)) {
                    fileName = pendingLocalSendUploads[sessionId][fileId].fileName;
                    if (expectedSize <= 0) {
                        expectedSize = pendingLocalSendUploads[sessionId][fileId].size;
                    }
                }
            }
        }

        if (headers.count("x-filename")) {
            fileName = HttpUrlDecode(headers["x-filename"]);
        }

        std::string destDir;
        {
            std::lock_guard<std::mutex> lock(settingsMutex);
            destDir = settings.destinationFolder;
        }
        std::filesystem::create_directories(destDir);
        std::filesystem::path outFilePath = std::filesystem::path(destDir) / fileName;
        std::ofstream outFile(outFilePath, std::ios::binary);

        bool isChunked = (headers.count("transfer-encoding") && headers["transfer-encoding"].find("chunked") != std::string::npos);

        int64_t totalReceived = 0;
        int64_t lastProgTime = getCurrentTimeMs();
        int64_t lastProgBytes = 0;
        double smoothedSpeedMBs = 0.0;

        if (isChunked) {
            std::string streamBuf = bodyInitial;
            size_t bufPos = 0;

            auto readByte = [&](char& outByte) -> bool {
                if (bufPos < streamBuf.size()) {
                    outByte = streamBuf[bufPos++];
                    return true;
                }
                char b;
                int r = recv(sock, &b, 1, 0);
                if (r <= 0) return false;
                outByte = b;
                return true;
            };

            auto readLine = [&]() -> std::string {
                std::string line;
                char c;
                while (readByte(c)) {
                    if (c == '\n') break;
                    if (c != '\r') line.push_back(c);
                }
                return line;
            };

            auto readExactBytes = [&](int64_t n) -> bool {
                int64_t rem = n;
                while (rem > 0 && isRunning) {
                    if (bufPos < streamBuf.size()) {
                        size_t avail = streamBuf.size() - bufPos;
                        size_t take = (rem < static_cast<int64_t>(avail)) ? static_cast<size_t>(rem) : avail;
                        outFile.write(&streamBuf[bufPos], take);
                        bufPos += take;
                        rem -= take;
                        totalReceived += take;
                    } else {
                        char chunkBuf[65536];
                        int toRead = (rem < static_cast<int64_t>(sizeof(chunkBuf))) ? static_cast<int>(rem) : static_cast<int>(sizeof(chunkBuf));
                        int r = recv(sock, chunkBuf, toRead, 0);
                        if (r <= 0) return false;
                        outFile.write(chunkBuf, r);
                        rem -= r;
                        totalReceived += r;
                    }

                    int64_t now = getCurrentTimeMs();
                    if (now - lastProgTime >= 80 || rem == 0) {
                        int64_t timeDeltaMs = now - lastProgTime;
                        if (timeDeltaMs > 0) {
                            int64_t bytesDelta = totalReceived - lastProgBytes;
                            double instantSpeed = (bytesDelta / (1024.0 * 1024.0)) / (timeDeltaMs / 1000.0);
                            smoothedSpeedMBs = (smoothedSpeedMBs <= 0.01) ? instantSpeed : (smoothedSpeedMBs * 0.75 + instantSpeed * 0.25);
                        }
                        lastProgTime = now;
                        lastProgBytes = totalReceived;

                        int pct = (expectedSize > 0) ? static_cast<int>(std::min<int64_t>(100, (totalReceived * 100) / expectedSize)) : 50;

                        TransferEvent prog;
                        prog.type = "progress";
                        prog.peerIp = clientIp;
                        prog.peerAlias = "Receiving from " + senderAlias;
                        prog.currentFileName = fileName;
                        prog.bytesTransferred = totalReceived;
                        prog.totalBytes = (expectedSize > 0) ? expectedSize : totalReceived;
                        prog.speedMBs = smoothedSpeedMBs;
                        prog.progressPercent = pct;
                        if (onTransferEvent) onTransferEvent(prog);
                    }
                }
                return rem == 0;
            };

            while (isRunning) {
                std::string line = readLine();
                size_t semi = line.find(';');
                std::string hexStr = (semi != std::string::npos) ? line.substr(0, semi) : line;
                while (!hexStr.empty() && (hexStr.front() == ' ' || hexStr.front() == '\t')) hexStr.erase(hexStr.begin());
                while (!hexStr.empty() && (hexStr.back() == ' ' || hexStr.back() == '\t')) hexStr.pop_back();
                if (hexStr.empty()) continue;

                int64_t chunkSize = 0;
                try {
                    chunkSize = std::stoll(hexStr, nullptr, 16);
                } catch (...) {
                    break;
                }

                if (chunkSize <= 0) {
                    readLine();
                    break;
                }

                if (!readExactBytes(chunkSize)) break;

                char cr = 0, lf = 0;
                readByte(cr);
                readByte(lf);
            }
        } else {
            if (!bodyInitial.empty()) {
                outFile.write(bodyInitial.data(), bodyInitial.size());
                totalReceived += bodyInitial.size();
            }

            int64_t targetLen = (contentLength > 0) ? contentLength : expectedSize;
            std::vector<char> upBuf(64 * 1024);
            while ((totalReceived < targetLen || targetLen <= 0) && isRunning) {
                int toRead = static_cast<int>(upBuf.size());
                if (targetLen > 0 && (targetLen - totalReceived) < static_cast<int64_t>(upBuf.size())) {
                    toRead = static_cast<int>(targetLen - totalReceived);
                }
                int r = recv(sock, upBuf.data(), toRead, 0);
                if (r <= 0) break;
                outFile.write(upBuf.data(), r);
                totalReceived += r;

                int64_t now = getCurrentTimeMs();
                if (now - lastProgTime >= 80 || (targetLen > 0 && totalReceived >= targetLen)) {
                    int64_t timeDeltaMs = now - lastProgTime;
                    if (timeDeltaMs > 0) {
                        int64_t bytesDelta = totalReceived - lastProgBytes;
                        double instantSpeed = (bytesDelta / (1024.0 * 1024.0)) / (timeDeltaMs / 1000.0);
                        smoothedSpeedMBs = (smoothedSpeedMBs <= 0.01) ? instantSpeed : (smoothedSpeedMBs * 0.75 + instantSpeed * 0.25);
                    }
                    lastProgTime = now;
                    lastProgBytes = totalReceived;

                    int pct = (targetLen > 0) ? static_cast<int>((totalReceived * 100) / targetLen) : 50;

                    TransferEvent prog;
                    prog.type = "progress";
                    prog.peerIp = clientIp;
                    prog.peerAlias = "Receiving from " + senderAlias;
                    prog.currentFileName = fileName;
                    prog.bytesTransferred = totalReceived;
                    prog.totalBytes = targetLen;
                    prog.speedMBs = smoothedSpeedMBs;
                    prog.progressPercent = pct;
                    if (onTransferEvent) onTransferEvent(prog);
                }
                if (targetLen > 0 && totalReceived >= targetLen) break;
            }
        }
        outFile.close();

        // Emit Complete Event so UI updates history & displays success
        TransferEvent done;
        done.type = "complete";
        done.peerIp = clientIp;
        done.peerAlias = "Receiving from " + senderAlias;
        done.currentFileName = fileName;
        done.bytesTransferred = totalReceived;
        done.totalBytes = totalReceived;
        done.progressPercent = 100;
        done.message = "File received successfully: " + fileName;
        if (onTransferEvent) onTransferEvent(done);

        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n{}";
        send(sock, resp.c_str(), static_cast<int>(resp.size()), 0);
        closesocket(sock);
        return;
    }

    // LocalSend: POST /api/localsend/v2/cancel or /v1/cancel
    if (method == "POST" && (path == "/api/localsend/v2/cancel" || path == "/api/localsend/v1/cancel")) {
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n{}";
        send(sock, resp.c_str(), static_cast<int>(resp.size()), 0);
        closesocket(sock);
        return;
    }

    // 2. Return list of shared files
    if (method == "GET" && path == "/api/files") {
        std::vector<std::string> currentFiles = getSharedFiles();
        JsonValue arr = JsonValue::Array();
        int id = 0;
        for (auto const& fpath : currentFiles) {
            std::filesystem::path fp(fpath);
            if (std::filesystem::exists(fp)) {
                JsonValue item = JsonValue::Object();
                item.set("id", id);
                item.set("name", fp.filename().string());
                item.set("size", static_cast<int64_t>(std::filesystem::file_size(fp)));
                item.set("url", "/api/download?id=" + std::to_string(id));
                arr.push(item);
                id++;
            }
        }
        std::string jsonStr = arr.serialize();
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(jsonStr.size()) + "\r\nConnection: close\r\n\r\n" + jsonStr;
        send(sock, resp.c_str(), static_cast<int>(resp.size()), 0);
        closesocket(sock);
        return;
    }

    // 2b. Device status and alias
    if (method == "GET" && path == "/api/info") {
        std::string myAlias;
        {
            std::lock_guard<std::mutex> lock(settingsMutex);
            myAlias = settings.alias;
        }
        JsonValue info = JsonValue::Object();
        info.set("alias", myAlias);
        info.set("version", "2.0.0");
        info.set("status", "online");
        std::string jsonStr = info.serialize();
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(jsonStr.size()) + "\r\nConnection: close\r\n\r\n" + jsonStr;
        send(sock, resp.c_str(), static_cast<int>(resp.size()), 0);
        closesocket(sock);
        return;
    }

    // 3. Download a shared file from PC to phone
    if (method == "GET" && path == "/api/download") {
        cancelCurrentTransfer = false;
        int targetId = -1;
        auto idPos = query.find("id=");
        if (idPos != std::string::npos) {
            targetId = std::atoi(query.substr(idPos + 3).c_str());
        }
        std::vector<std::string> currentFiles = getSharedFiles();
        if (targetId >= 0 && targetId < static_cast<int>(currentFiles.size())) {
            std::string targetPath = currentFiles[targetId];
            std::filesystem::path fp(targetPath);
            if (std::filesystem::exists(fp)) {
                int64_t fileSize = static_cast<int64_t>(std::filesystem::file_size(fp));
                std::string fileName = fp.filename().string();
                std::string header = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Disposition: attachment; filename=\"" + fileName + "\"\r\nContent-Length: " + std::to_string(fileSize) + "\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
                send(sock, header.c_str(), static_cast<int>(header.size()), 0);

                std::ifstream inFile(targetPath, std::ios::binary);
                std::vector<char> fbuf(64 * 1024);
                int64_t sent = 0;
                auto startT = std::chrono::steady_clock::now();
                while (inFile.good() && sent < fileSize && !cancelCurrentTransfer && isRunning) {
                    inFile.read(fbuf.data(), fbuf.size());
                    std::streamsize bytesRead = inFile.gcount();
                    if (bytesRead <= 0) break;
                    int s = send(sock, fbuf.data(), static_cast<int>(bytesRead), 0);
                    if (s <= 0) break;
                    sent += s;

                    auto now = std::chrono::steady_clock::now();
                    double elapsed = std::chrono::duration<double>(now - startT).count();
                    double speed = elapsed > 0 ? (sent / (1024.0 * 1024.0)) / elapsed : 0;
                    int pct = fileSize > 0 ? static_cast<int>((sent * 100) / fileSize) : 100;

                    TransferEvent prog;
                    prog.type = "progress";
                    prog.peerIp = clientIp;
                    prog.peerAlias = "Sending to Mobile";
                    prog.currentFileName = fileName;
                    prog.bytesTransferred = sent;
                    prog.totalBytes = fileSize;
                    prog.speedMBs = speed;
                    prog.progressPercent = pct;
                    if (onTransferEvent) onTransferEvent(prog);
                }
                inFile.close();

                if (cancelCurrentTransfer) {
                    TransferEvent ev;
                    ev.type = "error";
                    ev.peerIp = clientIp;
                    ev.peerAlias = "Sending to Mobile";
                    ev.message = "Download cancelled by user.";
                    if (onTransferEvent) onTransferEvent(ev);
                    closesocket(sock);
                    return;
                }

                if (sent >= fileSize) {
                    TransferEvent done;
                    done.type = "complete";
                    done.peerIp = clientIp;
                    done.peerAlias = "Sending to Mobile";
                    done.currentFileName = fileName;
                    done.totalBytes = fileSize;
                    done.progressPercent = 100;
                    done.message = "Downloaded by phone: " + fileName;
                    if (onTransferEvent) onTransferEvent(done);
                }

                closesocket(sock);
                return;
            }
        }
        std::string notFound = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(sock, notFound.c_str(), static_cast<int>(notFound.size()), 0);
        closesocket(sock);
        return;
    }

    // 3b. Mobile upload preparation and permission check
    if (method == "POST" && path == "/api/prepare-upload") {
        std::string reqBody = bodyInitial;
        if (headers.count("content-length")) {
            size_t clen = static_cast<size_t>(std::atoll(headers["content-length"].c_str()));
            while (reqBody.size() < clen) {
                int r = recv(sock, buf, sizeof(buf), 0);
                if (r <= 0) break;
                reqBody.append(buf, r);
            }
        }

        JsonValue req = JsonParser::parse(reqBody);
        std::string senderAlias = req.get("alias").asString("Mobile Web Device");
        int fileCount = req.get("count").asInt(1);
        int64_t totalBytes = req.get("size").asInt64(0);
        std::vector<std::string> fnames;
        if (req.has("files")) {
            for (auto const& f : req.get("files").arrVal) {
                fnames.push_back(f.get("name").asString("file"));
            }
        }

        bool autoAccept = false;
        {
            std::lock_guard<std::mutex> lock(settingsMutex);
            if (settings.quickSaveMode == 2 || settings.quickSave) {
                autoAccept = true;
            } else if (settings.quickSaveMode == 1) {
                std::string senderFp = req.get("fingerprint").asString();
                std::lock_guard<std::mutex> favLock(favoritesMutex);
                if (!senderFp.empty() && favoriteFingerprints.count(senderFp) > 0) {
                    autoAccept = true;
                }
            }
        }

        if (!autoAccept) {
            pendingTransferAccepted = false;
            pendingTransferRejected = false;

            TransferEvent ev;
            ev.type = "incoming_request";
            ev.peerIp = clientIp;
            ev.peerAlias = senderAlias;
            ev.totalBytes = totalBytes;
            ev.fileCount = fileCount;
            ev.fileNames = fnames;
            ev.message = "Incoming mobile transfer request";
            if (onTransferEvent) onTransferEvent(ev);

            // Wait up to 60 seconds for user response
            int elapsed = 0;
            while (!pendingTransferAccepted && !pendingTransferRejected && elapsed < 600 && isRunning) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                elapsed++;
            }

            if (!pendingTransferAccepted) {
                std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n{\"status\":\"REJECTED\"}";
                send(sock, resp.c_str(), static_cast<int>(resp.size()), 0);
                closesocket(sock);
                return;
            }
        }

        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n{\"status\":\"ACCEPTED\"}";
        send(sock, resp.c_str(), static_cast<int>(resp.size()), 0);
        closesocket(sock);
        return;
    }

    // 4. Upload file from phone to PC
    if (method == "POST" && path == "/api/upload") {
        cancelCurrentTransfer = false;
        int64_t contentLength = 0;
        if (headers.count("content-length")) {
            contentLength = std::atoll(headers["content-length"].c_str());
        }
        std::string fileName = "Mobile_Upload_" + std::to_string(getCurrentTimeMs()) + ".bin";
        if (headers.count("x-filename")) {
            fileName = HttpUrlDecode(headers["x-filename"]);
        } else if (query.find("filename=") != std::string::npos) {
            fileName = HttpUrlDecode(query.substr(query.find("filename=") + 9));
        }

        std::string destDir;
        {
            std::lock_guard<std::mutex> lock(settingsMutex);
            destDir = settings.destinationFolder;
        }
        std::filesystem::create_directories(destDir);
        std::filesystem::path outFilePath = std::filesystem::path(destDir) / fileName;
        std::ofstream outFile(outFilePath, std::ios::binary);

        int64_t totalReceived = 0;
        auto startT = std::chrono::steady_clock::now();

        if (!bodyInitial.empty()) {
            outFile.write(bodyInitial.data(), bodyInitial.size());
            totalReceived += bodyInitial.size();
        }

        std::vector<char> upBuf(64 * 1024);
        while ((totalReceived < contentLength || contentLength == 0) && !cancelCurrentTransfer && isRunning) {
            int r = recv(sock, upBuf.data(), static_cast<int>(upBuf.size()), 0);
            if (r <= 0) break;
            outFile.write(upBuf.data(), r);
            totalReceived += r;

            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - startT).count();
            double speed = elapsed > 0 ? (totalReceived / (1024.0 * 1024.0)) / elapsed : 0;
            int pct = contentLength > 0 ? static_cast<int>((totalReceived * 100) / contentLength) : 50;

            TransferEvent prog;
            prog.type = "progress";
            prog.peerIp = clientIp;
            prog.peerAlias = "Receiving from Mobile";
            prog.currentFileName = fileName;
            prog.bytesTransferred = totalReceived;
            prog.totalBytes = contentLength;
            prog.speedMBs = speed;
            prog.progressPercent = pct;
            if (onTransferEvent) onTransferEvent(prog);
        }
        outFile.close();

        if (cancelCurrentTransfer) {
            std::error_code ec;
            std::filesystem::remove(outFilePath, ec);
            TransferEvent ev;
            ev.type = "error";
            ev.peerIp = clientIp;
            ev.peerAlias = "Receiving from Mobile";
            ev.message = "Upload cancelled by user.";
            if (onTransferEvent) onTransferEvent(ev);
            closesocket(sock);
            return;
        }

        if (contentLength > 0 && totalReceived < contentLength) {
            std::error_code ec;
            std::filesystem::remove(outFilePath, ec);
            TransferEvent ev;
            ev.type = "error";
            ev.peerIp = clientIp;
            ev.peerAlias = "Receiving from Mobile";
            ev.message = "Upload interrupted or cancelled by client.";
            if (onTransferEvent) onTransferEvent(ev);
            closesocket(sock);
            return;
        }

        TransferEvent done;
        done.type = "complete";
        done.peerIp = clientIp;
        done.peerAlias = "Receiving from Mobile";
        done.currentFileName = fileName;
        done.totalBytes = totalReceived;
        done.progressPercent = 100;
        done.message = "Uploaded from phone: " + fileName;
        if (onTransferEvent) onTransferEvent(done);

        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n{\"status\":\"ok\"}";
        send(sock, resp.c_str(), static_cast<int>(resp.size()), 0);
        closesocket(sock);
        return;
    }

    std::string notFound = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    send(sock, notFound.c_str(), static_cast<int>(notFound.size()), 0);
    closesocket(sock);
}

} // namespace BlackHole
