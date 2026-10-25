<p align="center">
  <img src="ui/app_logo.png" alt="BlackHole Logo" width="120" height="120" style="border-radius: 24px;">
</p>

<h1 align="center">BlackHole for Windows</h1>

<p align="center">
  <b>Blazingly Fast, Zero-Knowledge Native Peer-to-Peer File & Clipboard Sharing</b><br>
  <i>Transfer files, folders, screenshots, and clipboard text at wire speed across your local Wi-Fi and Hotspots — pure C++20 performance with zero Electron bloat.</i>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-Windows%2010%20%2F%2011%20(x64)-0078D6?style=flat-square&logo=windows" alt="Windows">
  <img src="https://img.shields.io/badge/Backend-Native%20C%2B%2B20%20(MSVC)-00599C?style=flat-square&logo=c%2B%2B" alt="C++20">
  <img src="https://img.shields.io/badge/Frontend-WebView2%20(Edge%20Chromium)-blue?style=flat-square&logo=microsoftedge" alt="WebView2">
  <img src="https://img.shields.io/badge/Installer-Inno%20Setup%206-orange?style=flat-square" alt="Inno Setup">
  <img src="https://img.shields.io/badge/Security-Zero--Knowledge%20%2F%20Air--Gapped-red?style=flat-square" alt="Security">
  <img src="https://img.shields.io/badge/License-MIT-yellow?style=flat-square" alt="License">
</p>

---

## 🌌 Overview

**BlackHole for Windows** is an ultra-lightweight, high-speed peer-to-peer file transfer desktop application crafted specifically for Windows 10 and Windows 11. Engineered from the ground up with a native Microsoft C++20 backend and an embedded Microsoft Edge WebView2 glassmorphic interface, BlackHole eliminates the memory overhead and sluggishness of Electron-based apps while delivering full wire-speed network throughput.

---

## 🌌 The BlackHole Ecosystem

BlackHole provides a complete 3-system cross-platform ecosystem designed for wire-speed, zero-knowledge transfers:

| Platform | Type | Tech Stack | Highlights | Repository & Downloads |
| :--- | :--- | :--- | :--- | :--- |
| **🌐 BlackHole Web** | Browser-to-Browser Web App | React 19, TypeScript, WebRTC, Vite, Tailwind CSS v4, WebSocket | Zero-install, 6-digit room codes, QR pairing, Web Crypto AES-256-GCM, LAN ICE racing | [🌐 **Live Web App (share.sunilshahid.com)**](https://share.sunilshahid.com)<br>[📁 **GitHub Repository**](https://github.com/sunilshahid/BlackHole) |
| **📱 BlackHole for Android** | Native Android Application | Kotlin 2.0, Jetpack Compose Material 3, Coroutines | Air-gapped wire-speed TCP streaming, UDP multicast discovery (`224.0.0.167:53317`), in-memory clipboard copy, APK sharing, Foreground Service | [📁 **GitHub Repository**](https://github.com/sunilshahid/BlackHole-Andriod)<br>[⬇️ **Download v1.0.0 APK**](https://github.com/sunilshahid/BlackHole-Andriod/releases/tag/v1.0.0) |
| **💻 BlackHole for Windows** | Native Windows Desktop Client | Pure C++20 (MSVC), Win32 API, Microsoft Edge WebView2 | Near-zero CPU overhead, multi-homed UDP discovery, clipboard image auto-detection (`CF_DIBV5`, `PNG`), Inno Setup Installer & Portable ZIP | [📁 **GitHub Repository**](https://github.com/sunilshahid/BlackHole-Windows)<br>[⬇️ **Download v1.0.0 Installer & ZIP**](https://github.com/sunilshahid/BlackHole-Windows/releases/tag/v1.0.0) |

---

## ⚡ Key Features

* **🚀 Pure Native C++20 Engine:** Direct Winsock2 asynchronous socket architecture with multi-threaded streaming pipelines (64KB - 256KB chunk buffers) and near-zero background CPU usage.
* **🔒 100% Offline & Zero-Knowledge:** Operates strictly within your local Wi-Fi network or Mobile Hotspot. No telemetry, no user accounts, no cloud servers, and no third-party data tracking.
* **📡 Universal Zero-Config Peer Discovery:**
  * Multi-interface UDP broadcast and multicast on group `224.0.0.167:53317`.
  * Multi-homed interface binding to communicate across Wi-Fi hotspots, secondary network adapters, and Ethernet connections simultaneously.
  * Live dynamic radar scanner displaying nearby peer devices, operating systems, and network addresses.
* **📋 Smart Clipboard & Snipping Tool Sync:**
  * Auto-detects text, links, and code snippets from the Windows clipboard for 1-click transmission.
  * Native clipboard image handling supporting `CF_DIBV5`, `PNG`, and `CF_BITMAP` (instantly shares screenshots captured via Windows `Win + Shift + S` or copied from browsers).
* **🖥️ Windows Shell Integration:**
  * **System Tray Minimization:** Sits silently in the system tray when minimized, ready to accept incoming transfers in the background.
  * **Windows Notifications:** Native balloon notifications for incoming file requests and completed transfers.
  * **Explorer File Dialogs:** Uses the native Windows `IFileDialog` COM interface for fast, responsive file and folder selection.
* **⭐ Favorites & Quick Save:** Whitelist trusted computers and phones to automatically accept transfers without manual dialog interruptions.
* **📱 Cross-Platform Ecosystem:** Connects effortlessly with [BlackHole for Android](../BlackHole-Android) and open network peers across your LAN.
* **📦 Production Inno Setup & Portable Packages:** Available as both a standard Inno Setup 6 installer (`.exe`) with Start Menu/Desktop shortcuts, and a standalone zero-install Portable archive (`.zip`).

---

## 🏗️ Architecture & Project Structure

```text
BlackHole-Windows/
├── CMakeLists.txt              # CMake build definitions (C++20, MSVC, x64)
├── build.bat                   # 1-Click native C++ build script (MSVC)
├── src/
│   ├── main.cpp                # Win32 window, WebView2 host, tray icon & clipboard handler
│   ├── AppConfig.h/.cpp        # Persistent settings manager (%LOCALAPPDATA%\BlackHole)
│   ├── NetworkEngine.h/.cpp    # UDP multi-interface discovery & high-speed TCP socket streamer
│   ├── SimpleJson.h            # High-performance zero-dependency JSON parser & serializer
│   ├── Resource.rc / resource.h# Windows application icon and version metadata
│   └── app.ico                 # Multi-resolution Windows application icon
├── ui/
│   ├── index.html              # Modern glassmorphism UI layout
│   ├── style.css               # Space-grade cyber dark styling
│   ├── app.js                  # Reactive UI controller & IPC message bridge
│   ├── app_logo.png            # High-resolution raster branding logo
│   └── logo.svg                # Accretion disk vector branding logo
├── installer/
│   ├── BlackHole.iss           # Production Inno Setup 6 packaging script
│   ├── build_inno_installer.bat# Inno Setup compilation automation script
│   ├── Setup.cpp / Setup.rc    # Native C++ fallback installer
│   └── build_installer.bat     # Fallback installer compiler script
└── packages/                   # Microsoft Edge WebView2 SDK headers & static loader
```

---

## 🛠️ Building & Packaging

### Prerequisites
* **Operating System:** Windows 10 (Build 19041+) or Windows 11 (64-bit)
* **Compiler:** Microsoft Visual Studio 2022/2026 with the *"Desktop development with C++"* workload (MSVC x64 + Windows 10/11 SDK)
* **Installer Compiler (Optional):** [Inno Setup 6](https://jrsoftware.org/isdl.php) (for building the `.exe` setup installer)

### Step 1: Compile the Native Executable
Run the automated build script in Command Prompt or PowerShell:
```cmd
build.bat
```
The compiled binary will be placed at:
```text
build\Release\BlackHole.exe
```

### Step 2: Build the Inno Setup Installer
To create the official Windows setup installer (`BlackHole-Setup-v1.0.0-x64.exe`):
```cmd
cd installer
build_inno_installer.bat
```
The generated installer will be output to:
```text
dist\BlackHole-Setup-v1.0.0-x64.exe
```

> [!NOTE]
> Release installers and portable ZIP archives are uploaded directly to **GitHub Releases**. Compiled binaries and build caches are excluded from source version control to keep the repository clean.

---

## ⚙️ Configuration & Settings

Settings are automatically persisted to `%LOCALAPPDATA%\BlackHole\settings.json`:

* **Device Alias:** Customize how your PC appears on the local radar to other devices.
* **Destination Folder:** Select any folder on your PC where incoming files will be saved.
* **Quick Save Mode:**
  * `Off`: Prompt for every transfer request.
  * `Favorites Only`: Auto-accept transfers from bookmarked favorite devices.
  * `Always`: Auto-accept all incoming transfers on your local network.
* **Port Configuration:** Default `53317` (customizable for corporate network environments).

---

## 📄 License

This project is licensed under the [MIT License](LICENSE).
