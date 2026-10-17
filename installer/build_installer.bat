@echo off
setlocal

echo ========================================================
echo   Compiling BlackHole-Setup-x64.exe (Native Windows Installer)
echo ========================================================

set "VS_VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VS_VCVARS%" (
    call "%VS_VCVARS%"
) else (
    echo [ERROR] Could not find vcvars64.bat
    exit /b 1
)

cd installer
rc.exe /v Setup.rc
if errorlevel 1 (
    echo [ERROR] Resource compilation failed.
    exit /b 1
)

cl.exe /O2 /std:c++17 /EHsc /DUNICODE /D_UNICODE Setup.cpp Setup.res /Fe:"..\dist\BlackHole-Setup-x64.exe" /link /SUBSYSTEM:WINDOWS shell32.lib ole32.lib user32.lib advapi32.lib
if errorlevel 1 (
    echo [ERROR] Installer compilation failed.
    exit /b 1
)

del *.res *.obj 2>nul
cd ..

echo.
echo ========================================================
echo   Installer Built Successfully: dist\BlackHole-Setup-x64.exe
echo ========================================================
