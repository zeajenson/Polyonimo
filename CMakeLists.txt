cmake_minimum_required(VERSION 3.16.0)
project(polyonimo CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

add_executable(mo polyonimo.cpp)
target_link_libraries(mo wayland-client wayland-server)
