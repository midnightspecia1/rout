cmake_minimum_required(VERSION 3.20)
project(rout LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(rout WIN32
    src/main.cpp
    src/overlay.cpp
    src/overlay.h
    src/theme.h
)

# Windows 10+ for per-monitor-v2 DPI awareness.
target_compile_definitions(rout PRIVATE UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)

# MinGW needs -municode to pick wWinMain as the entry point.
if(MINGW)
    target_compile_options(rout PRIVATE -municode)
    target_link_options(rout PRIVATE -municode)
endif()

target_link_libraries(rout PRIVATE user32 gdi32)
