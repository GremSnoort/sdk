cmake_minimum_required(VERSION 3.15)

cmake_policy(SET CMP0048 NEW)
set(project sdk)
PROJECT(${project} VERSION "${CMAKE_PROJECT_VERSION}" LANGUAGES CXX)

message(STATUS "CMAKE_C_COMPILER: " ${CMAKE_C_COMPILER})
message(STATUS "CMAKE_CXX_COMPILER: " ${CMAKE_CXX_COMPILER})

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CONAN_EXPERIMENTAL_INCLUDE "CONAN EXPERIMENT INCLUDE" ON)

set(CMAKE_VERBOSE_MAKEFILE OFF)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  # using Clang
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # using GCC
    if (${WITH_SANITIZER} MATCHES "ON")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fsanitize=leak -g")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fsanitize=leak -g")
    endif()

    add_compile_options(
#    -Werror
    -Wunused
#    -Wall
    -Wextra
    -Wpedantic
    -Wcast-align
    -Wcast-qual
    -Wconversion
    -Wctor-dtor-privacy
    -Wenum-compare
    -Wfloat-equal
    -Wnon-virtual-dtor
#    -Wold-style-cast
    -Woverloaded-virtual
    -Wredundant-decls
#    -Wsign-conversion
    -Wsign-promo
    -Wno-deprecated
    -Wno-deprecated-declarations
    )
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -s")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -s")

elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Intel")
  # using Intel C++
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # using Visual Studio C++
    add_compile_options(
    /bigobj
    )
    if (NOT CMAKE_RUNTIME)
        set(CMAKE_RUNTIME MD) # dynamic/static
    endif()

    if (CMAKE_RUNTIME STREQUAL "MT" OR CMAKE_RUNTIME STREQUAL "MTd")
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # MT/MTd
    elseif(CMAKE_RUNTIME STREQUAL "MD" OR CMAKE_RUNTIME STREQUAL "MDd")
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL") # MD/MDd
    endif()

    message(STATUS "CMAKE_MSVC_RUNTIME_LIBRARY: " ${CMAKE_MSVC_RUNTIME_LIBRARY})
    message(STATUS "CMAKE_RUNTIME: " ${CMAKE_RUNTIME})
endif()

find_package(cxxopts REQUIRED)
find_package(Boost REQUIRED)
find_package(Catch2 REQUIRED)

# configuration
#############################################################################################
# ccahe

find_program(CCACHE_FOUND ccache)
if (CCACHE_FOUND)
    message(STATUS "Using ccache: ${CCACHE_FOUND}")
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ${CCACHE_FOUND})
    set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ${CCACHE_FOUND})
    set(ENV{CCACHE_COMMENTS} "1")
endif (CCACHE_FOUND)

# ccahe
#############################################################################################

get_filename_component(ROOT_PATH "${CMAKE_CURRENT_LIST_DIR}/" ABSOLUTE)
include_directories(${ROOT_PATH})

include(CTest)
enable_testing()

add_subdirectory(gremsnoort/sdk)
