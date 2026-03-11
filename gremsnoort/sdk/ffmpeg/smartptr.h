#pragma once

// std
#include <cassert>
#include <memory>
#include <concepts>

// conan

#ifdef __cplusplus
extern "C" {
#endif

#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavutil/frame.h>
#include <libavformat/avformat.h>

#ifdef __cplusplus
}
#endif

namespace gremsnoort::sdk::ffmpeg {

    namespace __internal {

        struct codec_ctx_deleter_t {

            auto operator()(AVCodecContext* ptr) const {
                if (ptr)
                    avcodec_free_context(&ptr);
                ptr = nullptr;
            }

        };

        struct packet_deleter_t {

            auto operator()(AVPacket* ptr) const {
                if (ptr)
                    av_packet_free(&ptr);
                ptr = nullptr;
            }

        };

        struct frame_deleter_t {

            auto operator()(AVFrame* ptr) const {
                if (ptr)
                    av_frame_free(&ptr);
                ptr = nullptr;
            }

        };

        struct input_format_deleter_t {

            auto operator()(AVFormatContext* ptr) const {
                if (ptr)
                    avformat_close_input(&ptr); // avformat_free_context(ptr);
                ptr = nullptr;
            }

        };

        struct output_format_deleter_t {

            auto operator()(AVFormatContext* ptr) const -> void {
                if (ptr)
                    avformat_free_context(ptr);
                ptr = nullptr;
            }
        };

    }

    using codec_ctx_uptr = std::unique_ptr<AVCodecContext, __internal::codec_ctx_deleter_t>;
    inline auto make_codec_ctx(const AVCodec *codec) {
        assert(codec);
        return codec_ctx_uptr{avcodec_alloc_context3(codec)};
    }
    inline auto make_codec_ctx() {
        return codec_ctx_uptr{avcodec_alloc_context3(nullptr)};
    }

    using packet_uptr = std::unique_ptr<AVPacket, __internal::packet_deleter_t>;
    inline auto make_packet() {
        return packet_uptr{av_packet_alloc()};
    }

    using frame_uptr = std::unique_ptr<AVFrame, __internal::frame_deleter_t>;
    inline auto make_frame() {
        return frame_uptr{av_frame_alloc()};
    }

    using input_format_uptr = std::unique_ptr<AVFormatContext, __internal::input_format_deleter_t>;
    inline auto make_input_format() {
        return input_format_uptr{avformat_alloc_context()};
    }

    using output_format_uptr = std::unique_ptr<AVFormatContext, __internal::output_format_deleter_t>;
    inline auto make_output_format() {
        return output_format_uptr{avformat_alloc_context()};
    }

    struct reference_t {
        AVStream* stream = nullptr;
        const AVCodec* codec = nullptr;
        const AVCodecParameters* codecpar = nullptr;
        AVRational time_base;

        explicit operator bool() const noexcept {
            return stream && codec && codecpar;
        }
    };

    template<class T>
    concept referenceable = requires(T* v) {
        { v->video_reference() } -> std::same_as<const reference_t&>;
        { v->audio_reference() } -> std::same_as<const reference_t&>;
    };

    struct stream_context_t {
        AVCodecContext* video = nullptr;
        AVCodecContext* audio = nullptr;

        explicit operator bool() const noexcept {
            return video || audio;
        }
    };

    template<class T, int E, class ...Args>
    concept logger = requires(T* v, const char* fmt, Args... args) {
        { v->template log<E>(fmt, std::forward<Args>(args)...) } -> std::same_as<void>;
    };

}
