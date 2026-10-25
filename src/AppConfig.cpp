#include "AppConfig.h"
#include "SimpleJson.h"
#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <filesystem>
#include <iostream>

namespace BlackHole {

std::string ConfigManager::getDefaultDeviceAlias() {
    char buffer[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(buffer);
    if (GetComputerNameA(buffer, &size)) {
        return std::string(buffer);
    }
    return "BlackHole-PC";
}

std::string ConfigManager::getDefaultDownloadFolder() {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, NULL, &path))) {
        char buffer[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, path, -1, buffer, MAX_PATH, NULL, NULL);
        CoTaskMemFree(path);
        std::filesystem::path p(buffer);
        p /= "BlackHole";
        try {
            std::filesystem::create_directories(p);
        } catch (...) {}
        return p.string();
    }
    return "C:\\Downloads\\BlackHole";
}

std::string ConfigManager::getConfigFilePath() {
    // Portable mode check: if settings.json exists right next to exe, use it!
    if (std::filesystem::exists("settings.json")) {
        return "settings.json";
    }

    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &path))) {
        char buffer[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, path, -1, buffer, MAX_PATH, NULL, NULL);
        CoTaskMemFree(path);
        std::filesystem::path dir(buffer);
        dir /= "BlackHole";
        try {
            std::filesystem::create_directories(dir);
        } catch (...) {}
        dir /= "settings.json";
        return dir.string();
    }
    return "settings.json";
}

AppSettings ConfigManager::load() {
    AppSettings s;
    s.alias = getDefaultDeviceAlias();
    s.destinationFolder = getDefaultDownloadFolder();

    std::string path = getConfigFilePath();
    if (!std::filesystem::exists(path)) {
        save(s);
        return s;
    }

    std::ifstream file(path);
    if (!file.is_open()) return s;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    JsonValue json = JsonParser::parse(content);
    if (json.has("alias")) s.alias = json.get("alias").asString(s.alias);
    if (json.has("destinationFolder")) s.destinationFolder = json.get("destinationFolder").asString(s.destinationFolder);
    if (json.has("quickSaveMode")) s.quickSaveMode = json.get("quickSaveMode").asInt(s.quickSave ? 2 : 0);
    if (json.has("quickSave")) s.quickSave = json.get("quickSave").asBool(s.quickSaveMode == 2);
    if (s.quickSaveMode == 2) s.quickSave = true;
    if (json.has("autoCopyText")) s.autoCopyText = json.get("autoCopyText").asBool(true);
    if (json.has("port")) s.port = json.get("port").asInt(s.port);
    if (json.has("encryptionEnabled")) s.encryptionEnabled = json.get("encryptionEnabled").asBool(s.encryptionEnabled);
    if (json.has("minimizeToTray")) s.minimizeToTray = json.get("minimizeToTray").asBool(s.minimizeToTray);

    return s;
}

bool ConfigManager::save(const AppSettings& s) {
    JsonValue json = JsonValue::Object();
    json.set("alias", s.alias);
    json.set("destinationFolder", s.destinationFolder);
    json.set("quickSaveMode", s.quickSaveMode);
    json.set("quickSave", s.quickSave || s.quickSaveMode == 2);
    json.set("autoCopyText", s.autoCopyText);
    json.set("port", s.port);
    json.set("encryptionEnabled", s.encryptionEnabled);
    json.set("minimizeToTray", s.minimizeToTray);

    std::string path = getConfigFilePath();
    std::ofstream file(path);
    if (!file.is_open()) return false;

    file << json.serialize();
    file.close();
    return true;
}

} // namespace BlackHole
