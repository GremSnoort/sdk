#pragma once

// std
#include <cstdint>

// conan
#ifdef __cplusplus
extern "C" {
#endif

#include <libavutil/rational.h>

#ifdef __cplusplus
}
#endif

namespace gremsnoort::sdk::ffmpeg {

    struct frame_metadata_t {
        static constexpr std::uint32_t magic_v = 0x4753464D; // "GSFM"

        std::uint32_t magic = magic_v;
        AVRational frame_rate_filter = AVRational{ .num = 0, .den = 1 };
    };

}
