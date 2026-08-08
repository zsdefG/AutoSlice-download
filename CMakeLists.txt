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

target_compile_options(downloader PRIVATE -O2 -Wall -Wextra)
target_link_libraries(downloader PRIVATE winhttp)
