cmake_minimum_required(VERSION 3.16)
project(gconf LANGUAGES CXX)

add_library(gconf INTERFACE)
target_include_directories(gconf INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(gconf INTERFACE cxx_std_17)
add_library(gconf::gconf ALIAS gconf)
