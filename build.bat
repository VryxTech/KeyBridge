@echo off
title KeyBridge Build System

echo.
echo ============================================
echo              KeyBridge Build
echo ============================================
echo.

echo [1/2] Compiling resources...

windres resource.rc -O coff -o resource.o

if errorlevel 1 (
    echo.
    echo [ERROR] Resource compilation failed.
    echo Build aborted.
    pause
    exit /b 1
)

echo [OK] Resources compiled successfully.
echo.

echo [2/2] Building KeyBridge...

g++ ^
-std=c++17 ^
-O2 ^
-flto ^
-ffunction-sections ^
-fdata-sections ^
-static ^
-static-libgcc ^
-static-libstdc++ ^
-Wl,--gc-sections ^
-s ^
-municode ^
-mwindows ^
-o KeyBridge.exe ^
keybridge.cpp ^
resource.o ^
-lgdiplus ^
-ldwmapi ^
-lshell32 ^
-lshlwapi ^
-lole32 ^
-luser32 ^
-lgdi32

if errorlevel 1 (
    echo.
    echo ============================================
    echo               BUILD FAILED
    echo ============================================
    pause
    exit /b 1
)

echo.
echo ============================================
echo      BUILD COMPLETED SUCCESSFULLY
echo ============================================
echo.
echo Output: KeyBridge.exe
echo.

pause