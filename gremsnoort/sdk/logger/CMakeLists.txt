set(project gremsnoort_logger)

file(GLOB ${project}_INTERNAL_HEADERS CONFIGURE_DEPENDS
    ./*.h
    ./*.hpp
)

file(GLOB ${project}_INTERNAL_SOURCES CONFIGURE_DEPENDS
    ./*.cpp
)

message(STATUS "TARGET = ${project}")

add_library(${project} STATIC
    ${${project}_INTERNAL_SOURCES}
    ${${project}_INTERNAL_HEADERS}
)

target_include_directories(${project} PUBLIC
    ${GREMSNOORT_SDK_SOURCE_DIR}
)

gremsnoort_apply_build_options(${project})

#target_link_libraries(${project} PRIVATE
#    fmt::fmt-header-only
#    spdlog::spdlog_header_only
#)

#add_subdirectory(example)
#add_subdirectory(testing)
