set(project pipeline_example)

message(STATUS "TARGET = ${project}")

add_executable(${project}
    pipeline.cpp
)

target_include_directories(${project} PRIVATE
    ${GREMSNOORT_SDK_SOURCE_DIR}
)

gremsnoort_apply_build_options(${project})

target_link_libraries(${project} PRIVATE
    ffmpeg::ffmpeg
    cxxopts::cxxopts
    boost::boost
    magic_enum::magic_enum
    sdk_benchmark_time
    gremsnoort_logger
)
