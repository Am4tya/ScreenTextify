cmake_minimum_required(VERSION 3.20)
project(ScreenTextify VERSION 3.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /O2 /utf-8 /EHsc /permissive- /bigobj)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

set(ORT_DIR ${CMAKE_SOURCE_DIR}/third_party/onnxruntime)
set(MODELS_DIR ${CMAKE_SOURCE_DIR}/third_party/models)

add_executable(ScreenTextify WIN32
    src/main.cpp
    src/AppStorage.cpp
    src/ScreenCapture.cpp
    src/OcrService.cpp
    src/PaddleOcr.cpp
    src/TextFormatter.cpp
    src/SyntaxHighlighter.cpp
    src/OverlayWindow.cpp
    src/HistoryWindow.cpp
)

target_include_directories(ScreenTextify PRIVATE src)
# ONNX Runtime headers only: the DLL is loaded at runtime (LoadLibraryExW) so no import library is linked.
target_include_directories(ScreenTextify SYSTEM PRIVATE ${ORT_DIR}/include)

target_link_libraries(ScreenTextify PRIVATE
    user32
    gdi32
    advapi32
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

# Ship the OCR runtime + models next to the executable
add_custom_command(TARGET ScreenTextify POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${ORT_DIR}/bin/onnxruntime.dll
        ${ORT_DIR}/bin/onnxruntime_providers_shared.dll
        $<TARGET_FILE_DIR:ScreenTextify>
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:ScreenTextify>/models
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MODELS_DIR}/rec_en.onnx
        ${MODELS_DIR}/en_dict.txt
        ${MODELS_DIR}/rec_en_v5.onnx
        ${MODELS_DIR}/rec_en_v5_dict.txt
        $<TARGET_FILE_DIR:ScreenTextify>/models
)
