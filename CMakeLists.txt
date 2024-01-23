cmake_minimum_required(VERSION 3.22)

project(IO_Uring_Linux_Server)

add_subdirectory(http-parser)

# Flags used regardless of build type
# set(DEFAULT_COMPILER_FLAGS -Wall -Wextra -Wpedantic -Werror -g)
set(DEFAULT_COMPILER_FLAGS -Wall -Wextra -g -luring)

add_executable(server src/server.c)
target_include_directories(server PUBLIC ./ ./src/ ./http-parser/)

# INLINE_EPOLL_WRITE - A "best effort" event loop.
# We just try to echo what we just read and will drop the data on EAGAIN/EWOULDBLOCK or other errors
# and partial writes. The benefit is that we do not need any buffer management for writes...

target_compile_options(server PRIVATE ${DEFAULT_COMPILER_FLAGS})
target_link_libraries(server -luring)
target_link_libraries(server http_parser)

set_target_properties(server PROPERTIES
                      RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin
                      RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin
)

if (CMAKE_BUILD_TYPE MATCHES Debug)
    add_compile_definitions(DEBUG)
    add_compile_definitions(LOGGING_ENABLED)
elseif (CMAKE_BUILD_TYPE MATCHES Release)

    target_compile_options(server PRIVATE ${DEFAULT_COMPILER_FLAGS} -O3)
    # Enable LTO
    set_property(TARGET server PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
else ()
    message(FATAL_ERROR Unknown build type ${CMAKE_BUILD_TYPE})
endif ()

