cmake_minimum_required(VERSION 3.20)
project(rout LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Third-party code, fetched at configure time and pinned to releases.
include(FetchContent)

# WebSocket server for the adapter IPC (M2). Plain ws:// on loopback:
# no TLS, no permessage-deflate.
set(USE_TLS OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG v12.1.0
    GIT_SHALLOW TRUE)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(ixwebsocket nlohmann_json)

add_executable(rout WIN32
    src/app_window.cpp
    src/app_window.h
    src/com_ptr.h
    src/hint_painter.cpp
    src/hint_painter.h
    src/ipc_server.cpp
    src/ipc_server.h
    src/ipc_token.cpp
    src/ipc_token.h
    src/main.cpp
    src/overlay.cpp
    src/overlay.h
    src/resource.h
    src/rout.rc
    src/scroller.cpp
    src/scroller.h
    src/settings.cpp
    src/settings.h
    src/theme.h
    src/uia_hints.cpp
    src/uia_hints.h
)

# Windows 10+ for per-monitor-v2 DPI awareness.
target_compile_definitions(rout PRIVATE UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)

# MinGW needs -municode to pick wWinMain as the entry point.
if(MINGW)
    target_compile_options(rout PRIVATE -municode)
    target_link_options(rout PRIVATE -municode)
endif()

# UI strings are UTF-8 (Cyrillic) literals.
if(MSVC)
    target_compile_options(rout PRIVATE /utf-8)
endif()

target_link_libraries(rout PRIVATE
    ixwebsocket nlohmann_json::nlohmann_json
    user32 gdi32 shell32 comctl32 comdlg32 msimg32 windowscodecs ole32 oleaut32 uiautomationcore uuid advapi32 bcrypt)

# Test client for the adapter IPC (M2); also a reference adapter.
add_executable(fake_adapter tools/fake_adapter.cpp)
target_compile_definitions(fake_adapter PRIVATE UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_link_libraries(fake_adapter PRIVATE ixwebsocket nlohmann_json::nlohmann_json shell32 ole32)
