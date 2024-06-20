set(project unit_example)

PROJECT(${project} VERSION "${CMAKE_PROJECT_VERSION}" LANGUAGES CXX)

file(GLOB ${PROJECT_NAME}_INTERNAL_HEADERS
    ../forward/*.h
    ../forward/*.hpp
)

file(GLOB ${PROJECT_NAME}_INTERNAL_SOURCES
    ./unit.cpp
)

set(${PROJECT_NAME}_HEADERS
    ${${PROJECT_NAME}_INTERNAL_HEADERS}
)

set(${PROJECT_NAME}_SOURCES
    ${${PROJECT_NAME}_INTERNAL_SOURCES}
)

message(STATUS "PROJECT_NAME = ${PROJECT_NAME}")

add_executable(${PROJECT_NAME}
    ${${PROJECT_NAME}_SOURCES}
    ${${PROJECT_NAME}_HEADERS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    cxxopts::cxxopts
    boost::boost
    sdk_benchmark_time
)
