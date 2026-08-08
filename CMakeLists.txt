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
