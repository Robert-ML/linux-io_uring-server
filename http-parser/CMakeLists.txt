cmake_minimum_required(VERSION 3.22)

set(DEFAULT_COMPILER_FLAGS -Wall -Wextra -g -luring)

project(http_parser VERSION 1.0 DESCRIPTION "http-parser library")

add_library(http_parser SHARED
    http_parser.c
)
target_include_directories(http_parser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
