#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <wrl.h>
#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <iostream>

#include "WebView2.h"
#include "resource.h"
#include "NetworkEngine.h"
#include "AppConfig.h"
#include "SimpleJson.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "crypt32.lib")
#include <wincrypt.h>
#include <fstream>

using namespace Microsoft::WRL;

// Global references
static HWND g_hWnd = NULL;
static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webview;
static std::unique_ptr<BlackHole::NetworkEngine> g_network;
static BlackHole::AppSettings g_settings;

#define WM_APP_POST_JSON (WM_APP + 101)

static std::mutex g_postMsgMutex;
static std::vector<std::wstring> g_pendingJsonMessages;

// Thread-safe helper to post JSON string to WebView2 UI
static void PostJsonToWebview(const std::string& jsonStr) {
    if (!g_hWnd) return;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, jsonStr.c_str(), -1, NULL, 0);
    std::wstring wstr(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, jsonStr.c_str(), -1, &wstr[0], wlen);

    {
        std::lock_guard<std::mutex> lock(g_postMsgMutex);
        g_pendingJsonMessages.push_back(std::move(wstr));
    }
    PostMessageW(g_hWnd, WM_APP_POST_JSON, 0, 0);
}

// Convert wide string to utf8
static std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
    std::string str(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], len, NULL, NULL);
    return str;
}

// Convert utf8 to wide string
static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, NULL, 0);
    std::wstring wstr(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], len);
    return wstr;
}

// Native Windows Folder Browser
static std::string BrowseForFolder(HWND parent) {
    std::string result = "";
    IFileDialog* pfd = NULL;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
        DWORD dwOptions;
        if (SUCCEEDED(pfd->GetOptions(&dwOptions))) {
            pfd->SetOptions(dwOptions | FOS_PICKFOLDERS);
        }
        if (SUCCEEDED(pfd->Show(parent))) {
            IShellItem* psi = NULL;
            if (SUCCEEDED(pfd->GetResult(&psi))) {
                PWSTR pszPath = NULL;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                    result = WideToUtf8(pszPath);
                    CoTaskMemFree(pszPath);
                }
                psi->Release();
            }
        }
        pfd->Release();
    }
    return result;
}

// Native Windows Multi-File Browser
static std::vector<std::string> BrowseForFiles(HWND parent) {
    std::vector<std::string> results;
    IFileOpenDialog* pfd = NULL;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
        DWORD dwOptions;
        if (SUCCEEDED(pfd->GetOptions(&dwOptions))) {
            pfd->SetOptions(dwOptions | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST);
        }
        if (SUCCEEDED(pfd->Show(parent))) {
            IShellItemArray* psia = NULL;
            if (SUCCEEDED(pfd->GetResults(&psia))) {
                DWORD count = 0;
                psia->GetCount(&count);
                for (DWORD i = 0; i < count; ++i) {
                    IShellItem* psi = NULL;
                    if (SUCCEEDED(psia->GetItemAt(i, &psi))) {
                        PWSTR pszPath = NULL;
                        if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                            results.push_back(WideToUtf8(pszPath));
                            CoTaskMemFree(pszPath);
                        }
                        psi->Release();
                    }
                }
                psia->Release();
            }
        }
        pfd->Release();
    }
    return results;
}

// Handle messages received from JavaScript via window.chrome.webview.postMessage
static void HandleWebMessage(const std::string& msgJson) {
    BlackHole::JsonValue req = BlackHole::JsonParser::parse(msgJson);
    std::string action = req.get("action").asString();

    if (action == "get_init_data") {
        BlackHole::JsonValue resp = BlackHole::JsonValue::Object();
        resp.set("type", "init_data");

        // Settings
        BlackHole::JsonValue sett = BlackHole::JsonValue::Object();
        sett.set("alias", g_settings.alias);
        sett.set("destinationFolder", g_settings.destinationFolder);
        sett.set("quickSave", g_settings.quickSave);
        sett.set("port", g_settings.port);
        sett.set("encryptionEnabled", g_settings.encryptionEnabled);
        resp.set("settings", sett);

        // Local IPs
        BlackHole::JsonValue ips = BlackHole::JsonValue::Array();
        for (auto const& ip : g_network->getLocalIPAddresses()) {
            ips.push(ip);
        }
        resp.set("localIps", ips);

        // Discovered Peers
        BlackHole::JsonValue peers = BlackHole::JsonValue::Array();
        for (auto const& p : g_network->getPeers()) {
            BlackHole::JsonValue pObj = BlackHole::JsonValue::Object();
            pObj.set("ip", p.ip);
            pObj.set("port", p.port);
            pObj.set("alias", p.alias);
            pObj.set("deviceType", p.deviceType);
            peers.push(pObj);
        }
        resp.set("peers", peers);

        PostJsonToWebview(resp.serialize());
    }
    else if (action == "save_settings") {
        BlackHole::JsonValue data = req.get("data");
        if (data.has("alias")) g_settings.alias = data.get("alias").asString();
        if (data.has("destinationFolder")) g_settings.destinationFolder = data.get("destinationFolder").asString();
        if (data.has("quickSave")) g_settings.quickSave = data.get("quickSave").asBool();
        if (data.has("port")) g_settings.port = data.get("port").asInt(53317);
        if (data.has("encryptionEnabled")) g_settings.encryptionEnabled = data.get("encryptionEnabled").asBool();

        BlackHole::ConfigManager::save(g_settings);
        g_network->updateSettings(g_settings);

        BlackHole::JsonValue resp = BlackHole::JsonValue::Object();
        resp.set("type", "settings_saved");
        PostJsonToWebview(resp.serialize());
    }
    else if (action == "browse_folder") {
        std::string chosen = BrowseForFolder(g_hWnd);
        if (!chosen.empty()) {
            BlackHole::JsonValue resp = BlackHole::JsonValue::Object();
            resp.set("type", "folder_selected");
            resp.set("path", chosen);
            PostJsonToWebview(resp.serialize());
        }
    }
    else if (action == "browse_files") {
        std::vector<std::string> chosen = BrowseForFiles(g_hWnd);
        if (!chosen.empty()) {
            BlackHole::JsonValue resp = BlackHole::JsonValue::Object();
            resp.set("type", "files_selected");
            BlackHole::JsonValue arr = BlackHole::JsonValue::Array();
            for (auto const& p : chosen) {
                std::filesystem::path fp(p);
                BlackHole::JsonValue fObj = BlackHole::JsonValue::Object();
                fObj.set("path", p);
                fObj.set("name", fp.filename().string());
                fObj.set("size", std::filesystem::exists(fp) ? static_cast<int64_t>(std::filesystem::file_size(fp)) : 0);
                arr.push(fObj);
            }
            resp.set("files", arr);
            PostJsonToWebview(resp.serialize());
        }
    }
    else if (action == "open_folder") {
        std::string folder = req.get("path").asString(g_settings.destinationFolder);
        ShellExecuteA(NULL, "open", folder.c_str(), NULL, NULL, SW_SHOW);
    }
    else if (action == "send_files") {
        std::string peerIp = req.get("peerIp").asString();
        int peerPort = req.get("peerPort").asInt(53317);
        BlackHole::JsonValue filesVal = req.get("files");
        std::vector<std::string> filePaths;
        for (auto const& f : filesVal.arrVal) {
            filePaths.push_back(f.asString());
        }
        g_network->sendFilesToPeer(peerIp, peerPort, filePaths);
    }
    else if (action == "accept_transfer") {
        g_network->acceptIncomingTransfer();
    }
    else if (action == "reject_transfer") {
        g_network->rejectIncomingTransfer();
    }
    else if (action == "cancel_transfer") {
        g_network->cancelTransfer();
    }
    else if (action == "save_file") {
        std::string filename = req.get("filename").asString();
        std::string base64Data = req.get("dataBase64").asString();
        if (!filename.empty() && !base64Data.empty()) {
            std::filesystem::path destDir(g_settings.destinationFolder);
            std::filesystem::create_directories(destDir);
            std::filesystem::path fullPath = destDir / filename;

            DWORD binLen = 0;
            if (CryptStringToBinaryA(base64Data.c_str(), (DWORD)base64Data.length(), CRYPT_STRING_BASE64, NULL, &binLen, NULL, NULL)) {
                std::vector<BYTE> buffer(binLen);
                if (CryptStringToBinaryA(base64Data.c_str(), (DWORD)base64Data.length(), CRYPT_STRING_BASE64, buffer.data(), &binLen, NULL, NULL)) {
                    std::ofstream out(fullPath, std::ios::binary);
                    if (out.is_open()) {
                        out.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
                        out.close();
                    }
                }
            }
        }
    }
    else if (action == "set_shared_files") {
        BlackHole::JsonValue filesVal = req.get("files");
        std::vector<std::string> filePaths;
        for (auto const& f : filesVal.arrVal) {
            filePaths.push_back(f.asString());
        }
        g_network->setSharedFiles(filePaths);
    }
    else if (action == "browse_folder_files") {
        std::string chosen = BrowseForFolder(g_hWnd);
        if (!chosen.empty()) {
            BlackHole::JsonValue resp = BlackHole::JsonValue::Object();
            resp.set("type", "files_selected");
            BlackHole::JsonValue arr = BlackHole::JsonValue::Array();
            std::error_code ec;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(chosen, ec)) {
                if (entry.is_regular_file(ec)) {
                    BlackHole::JsonValue fObj = BlackHole::JsonValue::Object();
                    fObj.set("path", entry.path().string());
                    fObj.set("name", entry.path().filename().string());
                    fObj.set("size", static_cast<int64_t>(entry.file_size(ec)));
                    arr.push(fObj);
                }
            }
            resp.set("files", arr);
            PostJsonToWebview(resp.serialize());
        }
    }
    else if (action == "open_url") {
        std::string url = req.get("url").asString();
        if (!url.empty()) {
            ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOW);
        }
    }
    else if (action == "rescan_peers") {
        BlackHole::JsonValue resp = BlackHole::JsonValue::Object();
        resp.set("type", "peers_updated");
        BlackHole::JsonValue peers = BlackHole::JsonValue::Array();
        for (auto const& p : g_network->getPeers()) {
            BlackHole::JsonValue pObj = BlackHole::JsonValue::Object();
            pObj.set("ip", p.ip);
            pObj.set("port", p.port);
            pObj.set("alias", p.alias);
            pObj.set("deviceType", p.deviceType);
            peers.push(pObj);
        }
        resp.set("peers", peers);
        PostJsonToWebview(resp.serialize());
    }
}

// Window Procedure
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        if (g_controller) {
            RECT bounds;
            GetClientRect(hWnd, &bounds);
            g_controller->put_Bounds(bounds);
        }
        break;
    case WM_APP_POST_JSON: {
        std::vector<std::wstring> msgs;
        {
            std::lock_guard<std::mutex> lock(g_postMsgMutex);
            msgs.swap(g_pendingJsonMessages);
        }
        if (g_webview) {
            for (const auto& wstr : msgs) {
                g_webview->PostWebMessageAsJson(wstr.c_str());
            }
        }
        break;
    }
    case WM_DESTROY:
        if (g_controller) {
            g_controller->Close();
            g_controller = nullptr;
        }
        g_webview = nullptr;
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// Forward declare WebView initialization with fallback capability
static void InitializeWebView(HWND hWnd, const std::wstring& baseDataDir, bool isFallback = false) {
    std::wstring actualDataDir = baseDataDir;
    if (isFallback) {
        // Fallback to process-isolated profile folder to bypass any locks or collisions
        actualDataDir = baseDataDir + L"\\Session_" + std::to_wstring(GetCurrentProcessId());
        CreateDirectoryW(actualDataDir.c_str(), NULL);
    }

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, actualDataDir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hWnd, baseDataDir, isFallback](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    if (!isFallback) {
                        InitializeWebView(hWnd, baseDataDir, true);
                        return S_OK;
                    }
                    WCHAR err[256];
                    wsprintfW(err, L"WebView2 Environment creation failed: 0x%08X", result);
                    MessageBoxW(hWnd, err, L"BlackHole Error", MB_ICONERROR);
                    return result;
                }

                env->CreateCoreWebView2Controller(
                    hWnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hWnd, baseDataDir, isFallback](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || !controller) {
                                if (!isFallback) {
                                    // Automatic fallback to clean session directory
                                    InitializeWebView(hWnd, baseDataDir, true);
                                    return S_OK;
                                }
                                WCHAR err[256];
                                wsprintfW(err, L"WebView2 Controller creation failed: 0x%08X", result);
                                MessageBoxW(hWnd, err, L"BlackHole Error", MB_ICONERROR);
                                return result;
                            }

                            g_controller = controller;
                            g_controller->get_CoreWebView2(&g_webview);

                            // Resize WebView to fill client area
                            RECT bounds;
                            GetClientRect(hWnd, &bounds);
                            g_controller->put_Bounds(bounds);

                            // Configure settings
                            ComPtr<ICoreWebView2Settings> settings;
                            g_webview->get_Settings(&settings);
                            if (settings) {
                                settings->put_IsScriptEnabled(TRUE);
                                settings->put_AreDefaultScriptDialogsEnabled(TRUE);
                                settings->put_IsWebMessageEnabled(TRUE);
                            }

                            // Register WebMessageReceived handler
                            EventRegistrationToken token;
                            g_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR messageRaw;
                                        args->TryGetWebMessageAsString(&messageRaw);
                                        if (messageRaw) {
                                            std::string msgUtf8 = WideToUtf8(messageRaw);
                                            CoTaskMemFree(messageRaw);
                                            HandleWebMessage(msgUtf8);
                                        }
                                        return S_OK;
                                    }).Get(),
                                &token
                            );

                            // Auto-save incoming downloaded files directly to destinationFolder without prompts
                            ComPtr<ICoreWebView2_4> webview4;
                            if (SUCCEEDED(g_webview.As(&webview4)) && webview4) {
                                EventRegistrationToken downloadToken;
                                webview4->add_DownloadStarting(
                                    Callback<ICoreWebView2DownloadStartingEventHandler>(
                                        [](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
                                            args->put_Handled(TRUE);
                                            LPWSTR resultFilePath = nullptr;
                                            if (SUCCEEDED(args->get_ResultFilePath(&resultFilePath)) && resultFilePath) {
                                                std::filesystem::path origPath(resultFilePath);
                                                std::wstring destDir = Utf8ToWide(g_settings.destinationFolder);
                                                if (!destDir.empty()) {
                                                    std::filesystem::create_directories(destDir);
                                                    std::wstring targetPath = destDir + L"\\" + origPath.filename().wstring();
                                                    args->put_ResultFilePath(targetPath.c_str());
                                                }
                                                CoTaskMemFree(resultFilePath);
                                            }
                                            return S_OK;
                                        }).Get(),
                                    &downloadToken
                                );
                            }

                            // Resolve UI path reliably relative to the executable
                            WCHAR exePath[MAX_PATH] = { 0 };
                            GetModuleFileNameW(NULL, exePath, MAX_PATH);
                            std::filesystem::path appDir = std::filesystem::path(exePath).parent_path();
                            std::filesystem::path uiPath = appDir / "ui" / "index.html";

                            if (!std::filesystem::exists(uiPath)) {
                                uiPath = appDir / ".." / "ui" / "index.html";
                            }
                            if (!std::filesystem::exists(uiPath)) {
                                uiPath = std::filesystem::current_path() / "ui" / "index.html";
                            }

                            std::wstring uiUri = L"file:///" + uiPath.wstring();
                            for (auto& c : uiUri) {
                                if (c == L'\\') c = L'/';
                            }

                            g_webview->Navigate(uiUri.c_str());
                            return S_OK;
                        }).Get()
                );
                return S_OK;
            }).Get()
    );
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // 1. Single Instance Check: Bring existing window to front if already open
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"BlackHole_Native_SingleInstance_Mutex_v1");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hExisting = FindWindowW(L"BlackHoleWindowClass", NULL);
        if (hExisting) {
            if (IsIconic(hExisting)) {
                ShowWindow(hExisting, SW_RESTORE);
            } else {
                ShowWindow(hExisting, SW_SHOW);
            }
            SetForegroundWindow(hExisting);
        }
        if (hMutex) CloseHandle(hMutex);
        return 0; // Clean exit without error
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    // Load Settings
    g_settings = BlackHole::ConfigManager::load();

    // Start Network Engine
    g_network = std::make_unique<BlackHole::NetworkEngine>();
    g_network->setPeerCallback([](const std::vector<BlackHole::PeerDevice>& peers) {
        BlackHole::JsonValue resp = BlackHole::JsonValue::Object();
        resp.set("type", "peers_updated");
        BlackHole::JsonValue arr = BlackHole::JsonValue::Array();
        for (auto const& p : peers) {
            BlackHole::JsonValue pObj = BlackHole::JsonValue::Object();
            pObj.set("ip", p.ip);
            pObj.set("port", p.port);
            pObj.set("alias", p.alias);
            pObj.set("deviceType", p.deviceType);
            arr.push(pObj);
        }
        resp.set("peers", arr);
        PostJsonToWebview(resp.serialize());
    });

    g_network->setTransferCallback([](const BlackHole::TransferEvent& ev) {
        BlackHole::JsonValue resp = BlackHole::JsonValue::Object();
        resp.set("type", "transfer_event");
        resp.set("eventType", ev.type);
        resp.set("peerIp", ev.peerIp);
        resp.set("peerAlias", ev.peerAlias);
        resp.set("currentFileName", ev.currentFileName);
        resp.set("bytesTransferred", ev.bytesTransferred);
        resp.set("totalBytes", ev.totalBytes);
        resp.set("speedMBs", ev.speedMBs);
        resp.set("progressPercent", ev.progressPercent);
        resp.set("message", ev.message);
        PostJsonToWebview(resp.serialize());
    });

    g_network->start();

    // Register Win32 Window
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    wcex.hIconSm = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wcex.lpszClassName = L"BlackHoleWindowClass";
    RegisterClassExW(&wcex);

    // Create Main Window
    g_hWnd = CreateWindowW(
        L"BlackHoleWindowClass",
        L"BlackHole - Zero-Knowledge Native P2P",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1020, 720,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_hWnd) {
        if (hMutex) CloseHandle(hMutex);
        return FALSE;
    }

    // Explicitly set taskbar and title bar icons and full Unicode title
    SendMessage(g_hWnd, WM_SETICON, ICON_BIG, (LPARAM)wcex.hIcon);
    SendMessage(g_hWnd, WM_SETICON, ICON_SMALL, (LPARAM)wcex.hIconSm);
    SetWindowTextW(g_hWnd, L"BlackHole - Zero-Knowledge Native P2P");

    // Enable Immersive Dark Mode in Windows Title Bar
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(g_hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    ShowWindow(g_hWnd, SW_SHOWNORMAL);
    UpdateWindow(g_hWnd);

    // Ensure LocalAppData\BlackHole\WebView2 exists for browser state
    PWSTR localAppData = nullptr;
    std::wstring webViewDataDir = L"";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &localAppData))) {
        std::wstring appDataDir = std::wstring(localAppData) + L"\\BlackHole";
        webViewDataDir = appDataDir + L"\\WebView2";
        CreateDirectoryW(appDataDir.c_str(), NULL);
        CreateDirectoryW(webViewDataDir.c_str(), NULL);
        CoTaskMemFree(localAppData);
    }

    // Initialize WebView2 with automatic session fallback
    InitializeWebView(g_hWnd, webViewDataDir);

    // Message Loop
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_network) {
        g_network->stop();
    }

    if (hMutex) {
        CloseHandle(hMutex);
    }

    CoUninitialize();
    return (int)msg.wParam;
}
