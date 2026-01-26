@echo off
REM Build script for Smart Proxy Service (C++ on Windows)

echo Building Smart Proxy Service...

REM Check if CMake is installed
where cmake >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo Error: CMake is not installed. Please install CMake from https://cmake.org/
    exit /b 1
)

REM Create build directory
if not exist build mkdir build
cd build

REM Configure with CMake
echo Configuring build...
cmake .. -DCMAKE_BUILD_TYPE=Release

REM Build
echo Building...
cmake --build . --config Release

REM Check if build was successful
if %ERRORLEVEL% EQU 0 (
    echo.
    echo Build complete!
    echo.
    echo Binary is located in: build\Release\smartproxy.exe
    echo.
    echo To run the service:
    echo   build\Release\smartproxy.exe
) else (
    echo Build failed!
    exit /b 1
)

cd ..
