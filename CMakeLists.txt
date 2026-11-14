cmake_minimum_required(VERSION 3.20)
project(ScreenTextify VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /O2 /utf-8 /EHsc /permissive- /bigobj)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_executable(ScreenTextify WIN32
    src/main.cpp
    src/ScreenCapture.cpp
    src/OcrService.cpp
    src/TextFormatter.cpp
    src/OverlayWindow.cpp
    src/HistoryWindow.cpp
)

target_include_directories(ScreenTextify PRIVATE src)

target_link_libraries(ScreenTextify PRIVATE
    user32
    gdi32
    shell32
    comdlg32
    ole32
    oleaut32
    uuid
    comctl32
    dwmapi
    d2d1
    dwrite
    windowscodecs
    gdiplus
    shcore
    windowsapp
)
