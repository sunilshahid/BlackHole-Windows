@echo off
setlocal

echo ========================================================
echo   Building BlackHole-Windows (Native C++ Release Build)
echo ========================================================

set "VS_VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set "VS_CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

if exist "%VS_VCVARS%" (
    call "%VS_VCVARS%"
) else (
    echo [ERROR] Could not find vcvars64.bat at %VS_VCVARS%
    exit /b 1
)

if not exist "%VS_CMAKE%" (
    set "VS_CMAKE=cmake.exe"
)

if not exist build (
    mkdir build
)

cd build
"%VS_CMAKE%" .. -G "Visual Studio 18 2026" -A x64
if errorlevel 1 (
    echo Fallback to NMake / Ninja / Direct MSVC...
    "%VS_CMAKE%" .. -G "NMake Makefiles"
    nmake
) else (
    "%VS_CMAKE%" --build . --config Release
)

echo.
echo ========================================================
echo ========================================================
cd ..
