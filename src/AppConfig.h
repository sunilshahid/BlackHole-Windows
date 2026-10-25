#pragma once
#include <string>

namespace BlackHole {

struct AppSettings {
    std::string alias = "BlackHole-PC";
    std::string destinationFolder = "";
    int quickSaveMode = 0; // 0: Off, 1: Favorites, 2: Always
    bool quickSave = false; // true if quickSaveMode == 2
    int port = 53317;
    bool encryptionEnabled = true;
    bool minimizeToTray = true;
    bool autoCopyText = true;
};

class ConfigManager {
public:
    static AppSettings load();
    static bool save(const AppSettings& settings);
    static std::string getDefaultDownloadFolder();
    static std::string getConfigFilePath();
    static std::string getDefaultDeviceAlias();
};

} // namespace BlackHole
