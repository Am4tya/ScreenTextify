@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if %ERRORLEVEL% neq 0 (
    echo Failed to initialize Visual Studio 2022 x64 environment.
    exit /b 1
)

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
if %ERRORLEVEL% neq 0 exit /b 1

cmake --build build --config Release
if %ERRORLEVEL% neq 0 exit /b 1

echo.
echo Build succeeded! Executable: build\ScreenTextify.exe
