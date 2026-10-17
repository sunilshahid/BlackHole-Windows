#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <commctrl.h>
#include <string>
#include <filesystem>
#include <fstream>
#include <vector>

#include "resource.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")

// Create a Windows Shortcut (.lnk)
static bool CreateShortcut(const std::wstring& targetPath, const std::wstring& shortcutPath, const std::wstring& iconPath, const std::wstring& workingDir) {
    IShellLinkW* psl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&psl));
    if (SUCCEEDED(hr)) {
        psl->SetPath(targetPath.c_str());
        psl->SetWorkingDirectory(workingDir.c_str());
        if (!iconPath.empty()) {
            psl->SetIconLocation(iconPath.c_str(), 0);
        }
        IPersistFile* ppf = nullptr;
        hr = psl->QueryInterface(IID_PPV_ARGS(&ppf));
        if (SUCCEEDED(hr)) {
            hr = ppf->Save(shortcutPath.c_str(), TRUE);
            ppf->Release();
        }
        psl->Release();
        return SUCCEEDED(hr);
    }
    return false;
}

// Get path to user's LocalAppData\Programs\BlackHole
static std::wstring GetInstallDir() {
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &localAppData))) {
        std::wstring dir = std::wstring(localAppData) + L"\\Programs\\BlackHole";
        CoTaskMemFree(localAppData);
        return dir;
    }
    return L"C:\\BlackHole";
}

// Get user's Desktop folder
static std::wstring GetDesktopDir() {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, NULL, &path))) {
        std::wstring s(path);
        CoTaskMemFree(path);
        return s;
    }
    return L"";
}

// Get user's Start Menu Programs folder
static std::wstring GetStartMenuDir() {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, NULL, &path))) {
        std::wstring s(path);
        CoTaskMemFree(path);
        return s;
    }
    return L"";
}

// Register with Windows Add/Remove Programs
static void RegisterUninstall(const std::wstring& installDir, const std::wstring& exePath, const std::wstring& uninstallerPath) {
    HKEY hKey;
    const wchar_t* subKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\BlackHole";
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subKey, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        const wchar_t* displayName = L"BlackHole - Native P2P";
        const wchar_t* displayVer = L"1.0.0";
        const wchar_t* publisher = L"Sunil Shahid";
        std::wstring uninstallCmd = L"\"" + uninstallerPath + L"\" /uninstall";

        RegSetValueExW(hKey, L"DisplayName", 0, REG_SZ, (const BYTE*)displayName, (DWORD)((wcslen(displayName) + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"DisplayVersion", 0, REG_SZ, (const BYTE*)displayVer, (DWORD)((wcslen(displayVer) + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"Publisher", 0, REG_SZ, (const BYTE*)publisher, (DWORD)((wcslen(publisher) + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"DisplayIcon", 0, REG_SZ, (const BYTE*)exePath.c_str(), (DWORD)((exePath.length() + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"InstallLocation", 0, REG_SZ, (const BYTE*)installDir.c_str(), (DWORD)((installDir.length() + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"UninstallString", 0, REG_SZ, (const BYTE*)uninstallCmd.c_str(), (DWORD)((uninstallCmd.length() + 1) * sizeof(wchar_t)));

        DWORD noModify = 1;
        RegSetValueExW(hKey, L"NoModify", 0, REG_DWORD, (const BYTE*)&noModify, sizeof(DWORD));
        RegSetValueExW(hKey, L"NoRepair", 0, REG_DWORD, (const BYTE*)&noModify, sizeof(DWORD));

        RegCloseKey(hKey);
    }
}

// Unregister from Windows Add/Remove Programs
static void UnregisterUninstall() {
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\BlackHole");
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    std::string cmdLine(lpCmdLine);
    bool isUninstall = (cmdLine.find("/uninstall") != std::string::npos);

    std::wstring installDir = GetInstallDir();
    std::wstring desktopDir = GetDesktopDir();
    std::wstring startMenuDir = GetStartMenuDir();

    // -------------------------------------------------------------
    // UNINSTALL ROUTINE
    // -------------------------------------------------------------
    if (isUninstall) {
        int choice = MessageBoxW(
            NULL,
            L"Are you sure you want to uninstall BlackHole from this computer?",
            L"BlackHole Uninstall",
            MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2
        );

        if (choice != IDYES) return 0;

        // Remove Shortcuts
        if (!desktopDir.empty()) {
            std::filesystem::remove(desktopDir + L"\\BlackHole.lnk");
        }
        if (!startMenuDir.empty()) {
            std::filesystem::remove(startMenuDir + L"\\BlackHole.lnk");
        }

        // Remove Registry Keys
        UnregisterUninstall();

        // Self-deleting command to remove installDir after process exits
        std::wstring selfDeleteCmd = L"/c timeout /t 1 /nobreak >nul & rmdir /s /q \"" + installDir + L"\"";
        ShellExecuteW(NULL, L"open", L"cmd.exe", selfDeleteCmd.c_str(), NULL, SW_HIDE);

        MessageBoxW(
            NULL,
            L"BlackHole has been successfully uninstalled from your computer.",
            L"BlackHole Uninstall Complete",
            MB_ICONINFORMATION | MB_OK
        );
        return 0;
    }

    // -------------------------------------------------------------
    // INSTALL ROUTINE
    // -------------------------------------------------------------
    std::wstring promptMsg = 
        L"Welcome to BlackHole Setup!\n\n"
        L"BlackHole will be installed to:\n" + installDir + L"\n\n"
        L"A Desktop shortcut and Start Menu entry will be created.\n\n"
        L"Click OK to begin installation.";

    int choice = MessageBoxW(
        NULL,
        promptMsg.c_str(),
        L"BlackHole Setup - Fast Zero-Knowledge P2P",
        MB_ICONINFORMATION | MB_OKCANCEL
    );

    if (choice != IDOK) return 0;

    // 1. Extract embedded ZIP payload from executable resource
    HRSRC hRes = FindResource(hInstance, MAKEINTRESOURCE(IDR_PAYLOAD_ZIP), RT_RCDATA);
    if (!hRes) {
        MessageBoxW(NULL, L"Error: Installer payload resource not found.", L"Installation Error", MB_ICONERROR);
        return 1;
    }

    HGLOBAL hData = LoadResource(hInstance, hRes);
    DWORD dataSize = SizeofResource(hInstance, hRes);
    void* pData = LockResource(hData);

    WCHAR tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring tempZip = std::wstring(tempPath) + L"BlackHole_payload.zip";

    std::ofstream zipFile(tempZip, std::ios::binary);
    if (!zipFile.is_open()) {
        MessageBoxW(NULL, L"Error: Could not write temporary installation files.", L"Installation Error", MB_ICONERROR);
        return 1;
    }
    zipFile.write(static_cast<const char*>(pData), dataSize);
    zipFile.close();

    // 2. Ensure target installation directory exists
    try {
        std::filesystem::create_directories(installDir);
    } catch (...) {}

    // 3. Extract the ZIP using tar.exe or PowerShell
    std::wstring extractCmd = L"/c tar.exe -xf \"" + tempZip + L"\" -C \"" + installDir + L"\"";
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = extractCmd.c_str();
    sei.nShow = SW_HIDE;

    if (ShellExecuteExW(&sei)) {
        WaitForSingleObject(sei.hProcess, 30000);
        CloseHandle(sei.hProcess);
    }

    // Clean up temporary ZIP
    std::filesystem::remove(tempZip);

    // 4. Copy this setup binary as Uninstall.exe inside installDir
    WCHAR currentExe[MAX_PATH];
    GetModuleFileNameW(NULL, currentExe, MAX_PATH);
    std::wstring uninstallerPath = installDir + L"\\Uninstall.exe";
    CopyFileW(currentExe, uninstallerPath.c_str(), FALSE);

    // 5. Create Desktop and Start Menu Shortcuts
    std::wstring appExe = installDir + L"\\BlackHole.exe";
    if (!desktopDir.empty()) {
        CreateShortcut(appExe, desktopDir + L"\\BlackHole.lnk", appExe, installDir);
    }
    if (!startMenuDir.empty()) {
        CreateShortcut(appExe, startMenuDir + L"\\BlackHole.lnk", appExe, installDir);
    }

    // 6. Register in Windows Settings (Apps & Features)
    RegisterUninstall(installDir, appExe, uninstallerPath);

    // 7. Success Dialog
    int launchChoice = MessageBoxW(
        NULL,
        L"BlackHole has been successfully installed!\n\nWould you like to launch BlackHole now?",
        L"Installation Complete",
        MB_ICONINFORMATION | MB_YESNO
    );

    if (launchChoice == IDYES) {
        ShellExecuteW(NULL, L"open", appExe.c_str(), NULL, installDir.c_str(), SW_SHOW);
    }

    CoUninitialize();
    return 0;
}
