# BlackHole - Windows Native Client

A blazingly fast, zero-knowledge peer-to-peer file transfer desktop application built with pure Microsoft C/C++ (MSVC / Windows SDK) and embedded Microsoft WebView2.

![BlackHole Logo](ui/logo.svg)

---

## 🚀 Key Features

* **Zero Internet Required:** Transfers files wire-speed directly across local Wi-Fi or Mobile Hotspots using direct Winsock TCP streaming.
* **LocalSend-Compatible Offline Discovery:** Automatic LAN/Hotspot peer discovery broadcasting and listening on UDP port `53317`.
* **Ultra-Lightweight & Fast:** Standalone native C++ binary (~200 KB) with static loader linking and zero Electron runtime overhead.
* **Modern Space-Grade UI:** Deep-space dark theme, animated relativistic accretion disk logo, and live nearby radar peer grid.
* **Integrated Settings:**
  * **Device Alias:** Customize how your PC appears to nearby devices.
  * **Destination Folder:** Choose where incoming files land with native Windows folder selection.
  * **Quick Save:** Auto-accept incoming transfers from trusted peers without interrupting prompts.
  * **Port Configuration:** Default `53317` (customizable).
  * **Hardware Encryption:** Toggle AES-256-GCM encryption.

---

## 🛠️ How to Build

### Prerequisites
* Windows 10 or Windows 11
* Microsoft Visual Studio 2022/2026 with "Desktop development with C++" (MSVC x64 + Windows SDK)

### 1-Click Build
Run the automated build script in PowerShell or Command Prompt:
```cmd
build.bat
```

The resulting executable will be created at:
```text
build\Release\BlackHole.exe
```

---

## 📦 Directory Structure

```text
BlackHole-Windows/
├── CMakeLists.txt              # CMake build definitions
├── build.bat                   # 1-Click build script invoking MSVC
├── src/
│   ├── main.cpp                # Win32 window, WebView2 host, IPC message bridge
│   ├── AppConfig.h/.cpp        # Persistent settings manager (%APPDATA%\BlackHole\settings.json)
│   ├── NetworkEngine.h/.cpp    # UDP discovery broadcaster/listener & TCP 1MB chunk streamer
│   └── SimpleJson.h            # Header-only zero-dependency JSON parser & serializer
├── ui/
│   ├── index.html              # Modern dark UI layout
│   ├── style.css               # Neon glowing space-grade styling
│   ├── app.js                  # Reactive UI controller & IPC bridge
│   └── logo.svg                # Accretion disk BlackHole SVG logo
└── packages/                   # Pre-bundled Microsoft WebView2 SDK
```
