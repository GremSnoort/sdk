set(project inplaced_tests)

file(GLOB ${project}_INTERNAL_HEADERS CONFIGURE_DEPENDS
    ../forward/*.h
    ../forward/*.hpp
)

file(GLOB ${project}_INTERNAL_SOURCES CONFIGURE_DEPENDS
    ./inplaced.cpp
)

message(STATUS "TARGET = ${project}")

add_executable(${project}
    ${${project}_INTERNAL_SOURCES}
    ${${project}_INTERNAL_HEADERS}
)

target_include_directories(${project} PRIVATE
    ${GREMSNOORT_SDK_SOURCE_DIR}
)

gremsnoort_apply_build_options(${project})

target_link_libraries(${project} PRIVATE
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(${project})
