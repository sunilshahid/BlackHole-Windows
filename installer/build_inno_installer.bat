@echo off
setlocal

echo ========================================================
echo   Compiling BlackHole Inno Setup Installer
echo ========================================================

set "ISCC_PATH="

if exist "C:\Users\Sunil Shahid\InnoSetup\ISCC.exe" (
    set "ISCC_PATH=C:\Users\Sunil Shahid\InnoSetup\ISCC.exe"
    goto :FoundISCC
)
if exist "C:\Program Files\Inno Setup 7\ISCC.exe" (
    set "ISCC_PATH=C:\Program Files\Inno Setup 7\ISCC.exe"
    goto :FoundISCC
)
if exist "C:\Program Files\Inno Setup 6\ISCC.exe" (
    set "ISCC_PATH=C:\Program Files\Inno Setup 6\ISCC.exe"
    goto :FoundISCC
)
if exist "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" (
    set "ISCC_PATH=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
    goto :FoundISCC
)

where iscc.exe >nul 2>&1
if %ERRORLEVEL% equ 0 (
    set "ISCC_PATH=iscc.exe"
    goto :FoundISCC
)

echo [ERROR] Inno Setup compiler ISCC.exe was not found.
echo Please install Inno Setup or add ISCC.exe to system PATH.
exit /b 1

:FoundISCC
echo Using Inno Setup Compiler: "%ISCC_PATH%"

if not exist "..\build\Release\BlackHole.exe" (
    echo [ERROR] "..\build\Release\BlackHole.exe" does not exist!
    echo Please run build.bat first to compile the native binary.
    exit /b 1
)

if not exist "..\dist" (
    mkdir "..\dist"
)

echo Compiling installer script: BlackHole.iss...
"%ISCC_PATH%" BlackHole.iss
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Inno Setup compilation failed.
    exit /b 1
)

echo.
echo ========================================================
echo   Inno Setup Installer Created Successfully!
echo   Output: dist\BlackHole-Setup-v1.0.0-x64.exe
echo ========================================================
