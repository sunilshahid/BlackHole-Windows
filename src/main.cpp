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
#pragma comment(lib, "gdiplus.lib")
#include <wincrypt.h>
#include <gdiplus.h>
#include <fstream>

using namespace Microsoft::WRL;

// Global references
static HWND g_hWnd = NULL;
static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webview;
static std::unique_ptr<BlackHole::NetworkEngine> g_network;
static BlackHole::AppSettings g_settings;
static ULONG_PTR g_gdiplusToken = 0;

#define WM_APP_POST_JSON (WM_APP + 101)

static std::mutex g_postMsgMutex;
static std::vector<std::wstring> g_pendingJsonMessages;

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

static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    Gdiplus::ImageCodecInfo* pImageCodecInfo = (Gdiplus::ImageCodecInfo*)(malloc(size));
    if (!pImageCodecInfo) return -1;
    Gdiplus::GetImageEncoders(num, size, pImageCodecInfo);
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            free(pImageCodecInfo);
            return j;
        }
    }
    free(pImageCodecInfo);
    return -1;
}

static bool SaveClipboardBitmapToPng(const std::filesystem::path& outPath) {
    bool opened = false;
    for (int retry = 0; retry < 5; ++retry) {
        if (OpenClipboard(NULL)) {
            opened = true;
            break;
        }
        Sleep(10);
    }
    if (!opened) return false;

    // 1. Direct PNG from clipboard (browsers, Electron apps, etc.)
    UINT cfPng = RegisterClipboardFormatW(L"PNG");
    if (cfPng && IsClipboardFormatAvailable(cfPng)) {
        HANDLE hData = GetClipboardData(cfPng);
        if (hData) {
            SIZE_T sz = GlobalSize(hData);
            void* pData = GlobalLock(hData);
            if (pData && sz > 0) {
                std::ofstream out(outPath, std::ios::binary);
                if (out.is_open()) {
                    out.write(reinterpret_cast<const char*>(pData), sz);
                    out.close();
                    GlobalUnlock(hData);
                    CloseClipboard();
                    return true;
                }
            }
            if (pData) GlobalUnlock(hData);
        }
    }

    // 2. CF_BITMAP or CF_DIBV5 / CF_DIB (Snipping Tool, Paint, etc.)
    HBITMAP hBitmap = (HBITMAP)GetClipboardData(CF_BITMAP);
    bool createdBitmap = false;
    if (!hBitmap) {
        HANDLE hDib = GetClipboardData(CF_DIBV5);
        if (!hDib) hDib = GetClipboardData(CF_DIB);
        if (hDib) {
            LPBITMAPINFOHEADER pBmi = (LPBITMAPINFOHEADER)GlobalLock(hDib);
            if (pBmi) {
                DWORD headerSize = pBmi->biSize;
                DWORD numColors = 0;
                if (pBmi->biBitCount <= 8) {
                    numColors = pBmi->biClrUsed ? pBmi->biClrUsed : (1 << pBmi->biBitCount);
                }
                DWORD masksSize = 0;
                if (pBmi->biCompression == BI_BITFIELDS && headerSize == sizeof(BITMAPINFOHEADER)) {
                    masksSize = 3 * sizeof(DWORD);
                }
                void* pBits = (char*)pBmi + headerSize + (numColors * sizeof(RGBQUAD)) + masksSize;
                HDC hdc = GetDC(NULL);
                hBitmap = CreateDIBitmap(hdc, pBmi, CBM_INIT, pBits, (BITMAPINFO*)pBmi, DIB_RGB_COLORS);
                ReleaseDC(NULL, hdc);
                GlobalUnlock(hDib);
                createdBitmap = (hBitmap != NULL);
            }
        }
    }

    if (!hBitmap) {
        CloseClipboard();
        return false;
    }

    HPALETTE hPalette = (HPALETTE)GetClipboardData(CF_PALETTE);
    Gdiplus::Bitmap bitmap(hBitmap, hPalette);
    CloseClipboard();

    CLSID pngClsid;
    if (GetEncoderClsid(L"image/png", &pngClsid) == -1) {
        if (createdBitmap) DeleteObject(hBitmap);
        return false;
    }
    Gdiplus::Status status = bitmap.Save(outPath.wstring().c_str(), &pngClsid, NULL);
    if (createdBitmap) DeleteObject(hBitmap);
    return (status == Gdiplus::Ok);
}

static std::vector<std::string> GetClipboardFiles() {
    std::vector<std::string> files;
    bool opened = false;
    for (int retry = 0; retry < 5; ++retry) {
        if (OpenClipboard(NULL)) {
            opened = true;
            break;
        }
        Sleep(10);
    }
    if (!opened) return files;
    HDROP hDrop = (HDROP)GetClipboardData(CF_HDROP);
    if (hDrop) {
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
        for (UINT i = 0; i < count; ++i) {
            wchar_t filePath[MAX_PATH];
            if (DragQueryFileW(hDrop, i, filePath, MAX_PATH)) {
                files.push_back(WideToUtf8(filePath));
            }
        }
    }
    CloseClipboard();
    return files;
}

// Native Win32 Clipboard helpers
static std::string GetNativeClipboardText() {
    bool opened = false;
    for (int retry = 0; retry < 5; ++retry) {
        if (OpenClipboard(NULL)) {
            opened = true;
            break;
        }
        Sleep(10);
    }
    if (!opened) return "";
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (!hData) {
        CloseClipboard();
        return "";
    }
    wchar_t* pszText = static_cast<wchar_t*>(GlobalLock(hData));
    if (!pszText) {
        CloseClipboard();
        return "";
    }
    std::wstring wText(pszText);
    GlobalUnlock(hData);
    CloseClipboard();

    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wText.c_str(), (int)wText.size(), NULL, 0, NULL, NULL);
    if (size_needed <= 0) return "";
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wText.c_str(), (int)wText.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

static void SetNativeClipboardText(const std::string& text) {
    if (!OpenClipboard(g_hWnd)) return;
    EmptyClipboard();
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, NULL, 0);
    if (wlen > 0) {
        HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, wlen * sizeof(wchar_t));
        if (hGlobal) {
            wchar_t* pBuf = static_cast<wchar_t*>(GlobalLock(hGlobal));
            if (pBuf) {
                MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, pBuf, wlen);
                GlobalUnlock(hGlobal);
                SetClipboardData(CF_UNICODETEXT, hGlobal);
            }
        }
    }
    CloseClipboard();
}

static void LogDebug(const std::string& msg) {
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &localAppData))) {
        std::wstring dir = std::wstring(localAppData) + L"\\BlackHole";
        CreateDirectoryW(dir.c_str(), NULL);
        std::wstring logPath = dir + L"\\debug.log";
        CoTaskMemFree(localAppData);
        std::ofstream f(logPath, std::ios::app);
        if (f.is_open()) {
            f << "[" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() << "] " << msg << "\n";
            f.flush();
        }
    }
}

// Thread-safe helper to post JSON string to WebView2 UI
static void PostJsonToWebview(const std::string& jsonStr) {
    if (!g_hWnd) return;
    std::wstring wstr = Utf8ToWide(jsonStr);
    {
        std::lock_guard<std::mutex> lock(g_postMsgMutex);
        g_pendingJsonMessages.push_back(std::move(wstr));
    }
    PostMessageW(g_hWnd, WM_APP_POST_JSON, 0, 0);
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

static void NotifyIncomingTransfer(HWND hWnd, const std::string& senderAlias, int numFiles, int64_t totalBytes) {
    if (!hWnd) return;

    if (IsIconic(hWnd) || GetForegroundWindow() != hWnd) {
        FLASHWINFO fwi = { sizeof(FLASHWINFO) };
        fwi.hwnd = hWnd;
        fwi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
        fwi.uCount = 0;
        fwi.dwTimeout = 0;
        FlashWindowEx(&fwi);

        MessageBeep(MB_ICONINFORMATION);
    }
}

// Handle messages received from JavaScript via window.chrome.webview.postMessage
static void HandleWebMessage(const std::string& msgJson) {
    try {
        LogDebug("HandleWebMessage incoming: " + msgJson);
        BlackHole::JsonValue req = BlackHole::JsonParser::parse(msgJson);
        std::string action = req.get("action").asString();
        LogDebug("HandleWebMessage action: " + action);

    if (action == "get_init_data") {
        BlackHole::JsonValue resp = BlackHole::JsonValue::Object();
        resp.set("type", "init_data");

        // Settings
        BlackHole::JsonValue sett = BlackHole::JsonValue::Object();
        sett.set("alias", g_settings.alias);
        sett.set("destinationFolder", g_settings.destinationFolder);
        sett.set("quickSaveMode", g_settings.quickSaveMode);
        sett.set("quickSave", g_settings.quickSave);
        sett.set("autoCopyText", g_settings.autoCopyText);
        sett.set("port", g_settings.port);
        sett.set("encryptionEnabled", g_settings.encryptionEnabled);
        resp.set("settings", sett);
        resp.set("deviceFingerprint", g_network ? g_network->getDeviceFingerprint() : "");

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
            pObj.set("deviceModel", p.deviceModel);
            pObj.set("deviceType", p.deviceType);
            pObj.set("fingerprint", p.fingerprint);
            pObj.set("protocol", p.protocol);
            peers.push(pObj);
        }
        resp.set("peers", peers);

        PostJsonToWebview(resp.serialize());
    }
    else if (action == "save_settings") {
        BlackHole::JsonValue data = req.get("data");
        if (data.has("alias")) g_settings.alias = data.get("alias").asString();
        if (data.has("destinationFolder")) g_settings.destinationFolder = data.get("destinationFolder").asString();
        if (data.has("quickSaveMode")) g_settings.quickSaveMode = data.get("quickSaveMode").asInt(0);
        if (data.has("quickSave")) g_settings.quickSave = data.get("quickSave").asBool();
        if (data.has("autoCopyText")) g_settings.autoCopyText = data.get("autoCopyText").asBool(true);
        if (data.has("port")) g_settings.port = data.get("port").asInt(53317);
        if (data.has("encryptionEnabled")) g_settings.encryptionEnabled = data.get("encryptionEnabled").asBool();

        BlackHole::ConfigManager::save(g_settings);
        g_network->updateSettings(g_settings);

        BlackHole::JsonValue resp = BlackHole::JsonValue::Object();
        resp.set("type", "settings_saved");
        PostJsonToWebview(resp.serialize());
    }
    else if (action == "update_favorites") {
        BlackHole::JsonValue data = req.get("data");
        if (data.has("fingerprints")) {
            std::vector<std::string> fps;
            for (auto const& item : data.get("fingerprints").arrVal) {
                fps.push_back(item.asString());
            }
            if (g_network) g_network->setFavoriteFingerprints(fps);
        }
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
    else if (action == "save_note_file") {
        std::string name = req.get("name").asString("note.txt");
        std::string content = req.get("content").asString("");
        char tempDir[MAX_PATH];
        GetTempPathA(MAX_PATH, tempDir);
        char longTemp[MAX_PATH];
        if (GetLongPathNameA(tempDir, longTemp, MAX_PATH) > 0) {
            strcpy_s(tempDir, longTemp);
        }
        std::filesystem::path dir = std::filesystem::path(tempDir) / "BlackHole";
        std::filesystem::create_directories(dir);
        std::filesystem::path fullPath = dir / name;
        std::ofstream out(fullPath, std::ios::binary);
        if (out.is_open()) {
            out.write(content.data(), content.size());
            out.close();
        }
        BlackHole::JsonValue resp = BlackHole::JsonValue::Object();
        resp.set("type", "note_saved");
        resp.set("path", fullPath.string());
        resp.set("name", name);
        resp.set("size", static_cast<int64_t>(content.size()));
        PostJsonToWebview(resp.serialize());
    }
    else if (action == "get_clipboard") {
        std::string txt = GetNativeClipboardText();
        std::string target = req.get("target").asString("compose");
        BlackHole::JsonValue resp = BlackHole::JsonValue::Object();
        resp.set("type", "clipboard_data");
        resp.set("text", txt);
        resp.set("target", target);
        PostJsonToWebview(resp.serialize());
    }
    else if (action == "set_clipboard") {
        std::string txt = req.get("text").asString("");
        SetNativeClipboardText(txt);
    }
    else if (action == "paste_clipboard" || action == "paste_clipboard_file") {
        // 1. Check if files were copied in File Explorer
        std::vector<std::string> dropFiles = GetClipboardFiles();
        if (!dropFiles.empty()) {
            BlackHole::JsonValue resp = BlackHole::JsonValue::Object();
            resp.set("type", "files_selected");
            BlackHole::JsonValue arr = BlackHole::JsonValue::Array();
            for (auto const& f : dropFiles) {
                if (std::filesystem::exists(f)) {
                    BlackHole::JsonValue item = BlackHole::JsonValue::Object();
                    item.set("name", std::filesystem::path(f).filename().string());
                    item.set("path", f);
                    item.set("size", static_cast<int64_t>(std::filesystem::file_size(f)));
                    arr.push(item);
                }
            }
            resp.set("files", arr);
            PostJsonToWebview(resp.serialize());
            return;
        }

        // 2. Check if an image is in clipboard (screenshot, Snipping tool, copy picture)
        char tempDir[MAX_PATH];
        GetTempPathA(MAX_PATH, tempDir);
        char longTemp[MAX_PATH];
        if (GetLongPathNameA(tempDir, longTemp, MAX_PATH) > 0) {
            strcpy_s(tempDir, longTemp);
        }
        std::filesystem::path dir = std::filesystem::path(tempDir) / "BlackHole";
        std::filesystem::create_directories(dir);
        int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        std::string imgName = "screenshot_" + std::to_string(nowMs) + ".png";
        std::filesystem::path imgPath = dir / imgName;

        if (SaveClipboardBitmapToPng(imgPath)) {
            int64_t imgSize = std::filesystem::file_size(imgPath);
            BlackHole::JsonValue resp = BlackHole::JsonValue::Object();
            resp.set("type", "note_saved");
            resp.set("path", imgPath.string());
            resp.set("name", imgName);
            resp.set("size", imgSize);
            resp.set("isImage", true);
            PostJsonToWebview(resp.serialize());
            return;
        }

        // 3. Check if text is in clipboard
        std::string txt = GetNativeClipboardText();
        if (!txt.empty()) {
            std::string txtName = "clipboard_" + std::to_string(nowMs) + ".txt";
            std::filesystem::path txtPath = dir / txtName;
            std::ofstream out(txtPath, std::ios::binary);
            if (out.is_open()) {
                out.write(txt.data(), txt.size());
                out.close();
            }
            BlackHole::JsonValue resp = BlackHole::JsonValue::Object();
            resp.set("type", "note_saved");
            resp.set("path", txtPath.string());
            resp.set("name", txtName);
            resp.set("size", static_cast<int64_t>(txt.size()));
            resp.set("isClipboard", true);
            resp.set("text", txt);
            PostJsonToWebview(resp.serialize());
            return;
        }

        // 4. Nothing on clipboard
        BlackHole::JsonValue resp = BlackHole::JsonValue::Object();
        resp.set("type", "clipboard_empty");
        PostJsonToWebview(resp.serialize());
    }
    else if (action == "send_files") {
        std::string peerIp = req.get("peerIp").asString();
        int peerPort = req.get("peerPort").asInt(53317);
        std::string peerAlias = req.get("peerAlias").asString();
        std::string protocol = req.get("protocol").asString("blackhole");
        BlackHole::JsonValue filesVal = req.get("files");
        std::vector<std::string> filePaths;
        for (auto const& f : filesVal.arrVal) {
            filePaths.push_back(f.asString());
        }
        g_network->sendFilesToPeer(peerIp, peerPort, filePaths, peerAlias, protocol);
    }
    else if (action == "accept_transfer") {
        g_network->acceptIncomingTransfer();
    }
    else if (action == "reject_transfer") {
        g_network->rejectIncomingTransfer();
    }
    else if (action == "accept_text_message") {
        g_network->acceptIncomingText();
    }
    else if (action == "reject_text_message") {
        g_network->rejectIncomingText();
    }
    else if (action == "cancel_transfer") {
        g_network->cancelTransfer();
    }
    else if (action == "save_file") {
        std::string filename = req.get("filename").asString();
        std::string base64Data = req.get("dataBase64").asString();
        if (!filename.empty() && !base64Data.empty()) {
            std::filesystem::path destDir(Utf8ToWide(g_settings.destinationFolder));
            std::filesystem::create_directories(destDir);
            std::filesystem::path fullPath = destDir / Utf8ToWide(filename);

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
    else if (action == "read_file_chunk") {
        std::string pathStr = req.get("path").asString();
        int64_t offset = req.get("offset").asInt(0);
        int32_t length = req.get("length").asInt(32768);
        std::string reqId = req.get("requestId").asString();

        BlackHole::JsonValue resp = BlackHole::JsonValue::Object();
        resp.set("type", "file_chunk_result");
        resp.set("requestId", reqId);

        std::wstring wpath = Utf8ToWide(pathStr);
        std::ifstream file(wpath, std::ios::binary);
        if (!file.is_open()) {
            resp.set("success", false);
            resp.set("error", "Failed to open file");
            resp.set("bytesRead", 0);
            resp.set("dataBase64", "");
            resp.set("eof", true);
            PostJsonToWebview(resp.serialize());
        } else {
            file.seekg(offset, std::ios::beg);
            std::vector<char> buffer(length);
            file.read(buffer.data(), length);
            std::streamsize bytesRead = file.gcount();
            bool isEof = file.eof() || (bytesRead < length);

            std::string b64Str = "";
            if (bytesRead > 0) {
                DWORD b64Len = 0;
                if (CryptBinaryToStringA(reinterpret_cast<const BYTE*>(buffer.data()), (DWORD)bytesRead, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &b64Len)) {
                    b64Str.resize(b64Len);
                    CryptBinaryToStringA(reinterpret_cast<const BYTE*>(buffer.data()), (DWORD)bytesRead, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &b64Str[0], &b64Len);
                    while (!b64Str.empty() && (b64Str.back() == '\0' || b64Str.back() == '\r' || b64Str.back() == '\n')) {
                        b64Str.pop_back();
                    }
                }
            }

            resp.set("success", true);
            resp.set("bytesRead", static_cast<int64_t>(bytesRead));
            resp.set("dataBase64", b64Str);
            resp.set("eof", isEof);
            PostJsonToWebview(resp.serialize());
        }
    }
    else if (action == "write_file_chunk") {
        std::string filename = req.get("filename").asString();
        int64_t offset = req.get("offset").asInt(0);
        std::string base64Data = req.get("dataBase64").asString();
        bool isLast = req.get("isLast").asBool(false);
        std::string reqId = req.get("requestId").asString();

        std::filesystem::path destDir(Utf8ToWide(g_settings.destinationFolder));
        std::filesystem::create_directories(destDir);
        std::filesystem::path fullPath = destDir / Utf8ToWide(filename);

        DWORD binLen = 0;
        bool ok = false;
        if (CryptStringToBinaryA(base64Data.c_str(), (DWORD)base64Data.length(), CRYPT_STRING_BASE64, NULL, &binLen, NULL, NULL)) {
            std::vector<BYTE> buffer(binLen);
            if (CryptStringToBinaryA(base64Data.c_str(), (DWORD)base64Data.length(), CRYPT_STRING_BASE64, buffer.data(), &binLen, NULL, NULL)) {
                std::ofstream out;
                if (offset == 0) {
                    out.open(fullPath, std::ios::binary | std::ios::trunc);
                } else {
                    out.open(fullPath, std::ios::binary | std::ios::in | std::ios::out);
                    if (!out.is_open()) {
                        out.open(fullPath, std::ios::binary | std::ios::app);
                    }
                    out.seekp(offset, std::ios::beg);
                }
                if (out.is_open()) {
                    out.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
                    out.close();
                    ok = true;
                }
            }
        }

        BlackHole::JsonValue resp = BlackHole::JsonValue::Object();
        resp.set("type", "write_chunk_result");
        resp.set("requestId", reqId);
        resp.set("success", ok);
        resp.set("isLast", isLast);
        PostJsonToWebview(resp.serialize());
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
            pObj.set("deviceModel", p.deviceModel);
            pObj.set("deviceType", p.deviceType);
            pObj.set("fingerprint", p.fingerprint);
            pObj.set("protocol", p.protocol);
            peers.push(pObj);
        }
        resp.set("peers", peers);
        PostJsonToWebview(resp.serialize());
    }
    } catch (const std::exception& e) {
        LogDebug("EXCEPTION in HandleWebMessage: " + std::string(e.what()));
    } catch (...) {
        LogDebug("UNKNOWN EXCEPTION in HandleWebMessage");
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
    case WM_ACTIVATE:
    case WM_SETFOCUS: {
        FLASHWINFO fwi = { sizeof(FLASHWINFO) };
        fwi.hwnd = hWnd;
        fwi.dwFlags = FLASHW_STOP;
        FlashWindowEx(&fwi);
        break;
    }
    case WM_APP_POST_JSON: {
        std::vector<std::wstring> msgs;
        {
            std::lock_guard<std::mutex> lock(g_postMsgMutex);
            msgs.swap(g_pendingJsonMessages);
        }
        if (g_webview) {
            for (const auto& wstr : msgs) {
                try {
                    HRESULT hr = g_webview->PostWebMessageAsJson(wstr.c_str());
                    if (FAILED(hr)) {
                        char buf[128];
                        sprintf_s(buf, "PostWebMessageAsJson hr=0x%08X", hr);
                        LogDebug(buf);
                    }
                } catch (const std::exception& e) {
                    LogDebug("EXCEPTION in PostWebMessageAsJson: " + std::string(e.what()));
                } catch (...) {
                    LogDebug("UNKNOWN EXCEPTION in PostWebMessageAsJson");
                }
            }
        }
        break;
    }
    case WM_DESTROY:
        LogDebug("WM_DESTROY received in WndProc!");
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
    LogDebug("InitializeWebView called. isFallback=" + std::to_string(isFallback) + " dir=" + WideToUtf8(actualDataDir));

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, actualDataDir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hWnd, baseDataDir, isFallback](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                char buf[128];
                sprintf_s(buf, "Environment callback: result=0x%08X env=%p", result, env);
                LogDebug(buf);
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
                            char cbuf[128];
                            sprintf_s(cbuf, "Controller callback: result=0x%08X controller=%p", result, controller);
                            LogDebug(cbuf);
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
                            LogDebug("Step 1: got CoreWebView2");

                            // Resize WebView to fill client area
                            RECT bounds;
                            GetClientRect(hWnd, &bounds);
                            g_controller->put_Bounds(bounds);
                            LogDebug("Step 2: put_Bounds done");

                            // Configure settings
                            ComPtr<ICoreWebView2Settings> settings;
                            g_webview->get_Settings(&settings);
                            if (settings) {
                                settings->put_IsScriptEnabled(TRUE);
                                settings->put_AreDefaultScriptDialogsEnabled(TRUE);
                                settings->put_IsWebMessageEnabled(TRUE);
                            }
                            LogDebug("Step 3: settings configured");

                            // Register WebMessageReceived handler
                            EventRegistrationToken token;
                            g_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        try {
                                            LPWSTR messageRaw = nullptr;
                                            HRESULT hr = args->TryGetWebMessageAsString(&messageRaw);
                                            if (SUCCEEDED(hr) && messageRaw) {
                                                std::string msgUtf8 = WideToUtf8(messageRaw);
                                                CoTaskMemFree(messageRaw);
                                                HandleWebMessage(msgUtf8);
                                            }
                                        } catch (const std::exception& e) {
                                            LogDebug("EXCEPTION in WebMessageReceived callback: " + std::string(e.what()));
                                        } catch (...) {
                                            LogDebug("UNKNOWN EXCEPTION in WebMessageReceived callback");
                                        }
                                        return S_OK;
                                    }).Get(),
                                &token
                            );
                            LogDebug("Step 4: add_WebMessageReceived done");

                            // Auto-grant clipboard read/write and notification permissions permanently without prompts
                            EventRegistrationToken permToken;
                            g_webview->add_PermissionRequested(
                                Callback<ICoreWebView2PermissionRequestedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                                        args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
                                        return S_OK;
                                    }).Get(),
                                &permToken
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
                            LogDebug("Step 5: downloadStarting checked");

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
                            LogDebug("Step 6: resolved uiPath = " + WideToUtf8(uiPath.wstring()));

                            std::wstring uiUri = L"file:///" + uiPath.wstring();
                            for (auto& c : uiUri) {
                                if (c == L'\\') c = L'/';
                            }
                            LogDebug("Step 7: navigating to uiUri = " + WideToUtf8(uiUri));

                            HRESULT navHr = g_webview->Navigate(uiUri.c_str());
                            LogDebug("Step 8: Navigate returned hr = " + std::to_string(navHr));
                            return S_OK;
                        }).Get()
                );
                return S_OK;
            }).Get()
    );
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* pEx) {
    char buf[256];
    sprintf_s(buf, "CRASH! ExceptionCode = 0x%08X at Address = %p",
        pEx->ExceptionRecord->ExceptionCode,
        pEx->ExceptionRecord->ExceptionAddress);
    LogDebug(buf);
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    SetUnhandledExceptionFilter(CrashFilter);
    LogDebug("WinMain started");
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"BlackHole_Native_SingleInstance_Mutex_v1");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hExisting = FindWindowW(L"BlackHoleWindowClass", NULL);
        if (hExisting) {
            LogDebug("Found existing window, restoring and bringing to front");
            if (IsIconic(hExisting)) {
                ShowWindow(hExisting, SW_RESTORE);
            } else {
                ShowWindow(hExisting, SW_SHOW);
            }
            SetForegroundWindow(hExisting);
            if (hMutex) CloseHandle(hMutex);
            return 0; // Clean exit, another instance is actively running
        }
        LogDebug("Mutex existed but no window found - proceeding with fresh startup");
    }

    LogDebug("WinMain: CoInitializeEx");
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    LogDebug("WinMain: GdiplusStartup");
    // Initialize GDI+ for image processing (saving clipboard screenshots, etc.)
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);

    LogDebug("WinMain: ConfigManager::load");
    // Load Settings
    g_settings = BlackHole::ConfigManager::load();

    LogDebug("WinMain: make_unique<NetworkEngine>");
    // Start Network Engine
    g_network = std::make_unique<BlackHole::NetworkEngine>();
    LogDebug("WinMain: NetworkEngine created");
    g_network->setPeerCallback([](const std::vector<BlackHole::PeerDevice>& peers) {
        BlackHole::JsonValue resp = BlackHole::JsonValue::Object();
        resp.set("type", "peers_updated");
        BlackHole::JsonValue arr = BlackHole::JsonValue::Array();
        for (auto const& p : peers) {
            BlackHole::JsonValue pObj = BlackHole::JsonValue::Object();
            pObj.set("ip", p.ip);
            pObj.set("port", p.port);
            pObj.set("alias", p.alias);
            pObj.set("deviceModel", p.deviceModel);
            pObj.set("deviceType", p.deviceType);
            pObj.set("fingerprint", p.fingerprint);
            pObj.set("protocol", p.protocol);
            arr.push(pObj);
        }
        resp.set("peers", arr);
        PostJsonToWebview(resp.serialize());
    });

    g_network->setTransferCallback([](const BlackHole::TransferEvent& ev) {
        if (ev.type == "incoming_request") {
            NotifyIncomingTransfer(g_hWnd, ev.peerAlias, ev.fileCount, ev.totalBytes);
        }

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
        resp.set("textContent", ev.textContent);
        resp.set("fileCount", ev.fileCount);

        BlackHole::JsonValue fNames = BlackHole::JsonValue::Array();
        for (const auto& fn : ev.fileNames) {
            fNames.push(fn);
        }
        resp.set("fileNames", fNames);

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
    LogDebug("Entering GetMessage loop");
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    LogDebug("Exited GetMessage loop, wParam=" + std::to_string(msg.wParam));

    if (g_network) {
        g_network->stop();
    }

    if (hMutex) {
        CloseHandle(hMutex);
    }

    if (g_gdiplusToken) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }

    CoUninitialize();
    return (int)msg.wParam;
}
