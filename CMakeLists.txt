cmake_minimum_required(VERSION 3.20)
project(rout LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(rout WIN32
    src/main.cpp
    src/overlay.cpp
    src/overlay.h
)

target_link_libraries(rout PRIVATE user32 gdi32)
