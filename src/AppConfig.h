#pragma once
#include <string>

namespace BlackHole {

struct AppSettings {
    std::string alias = "BlackHole-PC";
    std::string destinationFolder = "";
    bool quickSave = true;
    int port = 53317;
    bool encryptionEnabled = true;
    bool minimizeToTray = true;
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
