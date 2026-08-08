cmake_minimum_required(VERSION 3.16)
project(pcl_downloader CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(downloader
    src/main.cpp
    src/downloader.cpp
)

target_compile_options(downloader PRIVATE -O2 -Wall -Wextra -municode)
target_link_options(downloader PRIVATE -municode)
target_link_libraries(downloader PRIVATE winhttp)

# Win32 GUI 版 (复用同一引擎, 无控制台窗口)
add_executable(downloadergui
    src/gui.cpp
    src/downloader.cpp
)

target_compile_options(downloadergui PRIVATE -O2 -Wall -Wextra -municode)
target_link_options(downloadergui PRIVATE -municode)
target_link_libraries(downloadergui PRIVATE winhttp comctl32)

# WebView2 版 GUI: 宿主窗口 + 加载 prototype/index.html
add_executable(downloadergui2
    src/gui_webview.cpp
    src/downloader.cpp
)

target_include_directories(downloadergui2 PRIVATE ${CMAKE_SOURCE_DIR}/third_party/webview2/build/native/include)
target_compile_options(downloadergui2 PRIVATE -O2 -Wall -Wextra -municode)
target_link_options(downloadergui2 PRIVATE -municode)
target_link_libraries(downloadergui2 PRIVATE winhttp comctl32 ole32)
# 构建后拷贝 WebView2Loader.dll 到 exe 目录 (运行时依赖)
add_custom_command(TARGET downloadergui2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/third_party/webview2/build/native/x64/WebView2Loader.dll
        $<TARGET_FILE_DIR:downloadergui2>/WebView2Loader.dll)
