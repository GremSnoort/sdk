set(project sdk_benchmark_time)

PROJECT(${project} VERSION "${CMAKE_PROJECT_VERSION}" LANGUAGES CXX)

file(GLOB ${PROJECT_NAME}_INTERNAL_HEADERS
    ./*.h
    ./*.hpp
)

file(GLOB ${PROJECT_NAME}_INTERNAL_SOURCES
    ./*.cpp
    ./*.cc
)

set(${PROJECT_NAME}_HEADERS
    ${${PROJECT_NAME}_INTERNAL_HEADERS}
)

set(${PROJECT_NAME}_SOURCES
    ${${PROJECT_NAME}_INTERNAL_SOURCES}
)

message(STATUS "PROJECT_NAME = ${PROJECT_NAME}")

add_library(${PROJECT_NAME} STATIC
    ${${PROJECT_NAME}_SOURCES}
    ${${PROJECT_NAME}_HEADERS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
)

#add_subdirectory(example)
#add_subdirectory(testing)
