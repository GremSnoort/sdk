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

# Stable SDK root for subdirectories, including superproject add_subdirectory() usage.
set(GREMSNOORT_SDK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

option(WITH_SANITIZER "Enable Address/Leak sanitizer for supported compilers" OFF)

set(CMAKE_VERBOSE_MAKEFILE OFF)

set(GREMSNOORT_COMPILE_OPTIONS)
set(GREMSNOORT_LINK_OPTIONS)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  # using Clang
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # using GCC
    list(APPEND GREMSNOORT_COMPILE_OPTIONS
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

    list(APPEND GREMSNOORT_LINK_OPTIONS
        $<$<CONFIG:Release>:-s>
    )

    if (WITH_SANITIZER)
        list(APPEND GREMSNOORT_COMPILE_OPTIONS
            -fsanitize=address
            -fsanitize=leak
            -g
        )
        list(APPEND GREMSNOORT_LINK_OPTIONS
            -fsanitize=address
            -fsanitize=leak
            -g
        )
    endif()

elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Intel")
  # using Intel C++
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # using Visual Studio C++
    list(APPEND GREMSNOORT_COMPILE_OPTIONS
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

function(gremsnoort_apply_build_options target_name)
    if (GREMSNOORT_COMPILE_OPTIONS)
        target_compile_options(${target_name} PRIVATE ${GREMSNOORT_COMPILE_OPTIONS})
    endif()

    get_target_property(target_type ${target_name} TYPE)
    if (GREMSNOORT_LINK_OPTIONS AND NOT target_type STREQUAL "STATIC_LIBRARY")
        target_link_options(${target_name} PRIVATE ${GREMSNOORT_LINK_OPTIONS})
    endif()
endfunction()

find_package(cxxopts REQUIRED)
find_package(Boost REQUIRED)
find_package(Catch2 REQUIRED)
find_package(ffmpeg REQUIRED)
find_package(magic_enum REQUIRED)

if (NOT TARGET ffmpeg::ffmpeg)
    message(FATAL_ERROR "ffmpeg package found, but target 'ffmpeg::ffmpeg' is missing")
endif()

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

include(CTest)
enable_testing()

add_subdirectory(gremsnoort/sdk)
