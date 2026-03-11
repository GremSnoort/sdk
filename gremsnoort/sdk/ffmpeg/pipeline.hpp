#pragma once

// std
#include <cassert>
#include <concepts>
#include <cstdio>
#include <memory>
#include <type_traits>
#include <utility>

// gsdk
#include <gremsnoort/sdk/forward/coro.hpp>
#include <gremsnoort/sdk/ffmpeg/smartptr.h>
#include <gremsnoort/sdk/ffmpeg/demuxer.hpp>
#include <gremsnoort/sdk/ffmpeg/decoder.hpp>
#include <gremsnoort/sdk/ffmpeg/encoder.hpp>
#include <gremsnoort/sdk/ffmpeg/muxer.hpp>
#include <gremsnoort/sdk/logger/logger.h>

namespace gremsnoort::sdk::ffmpeg {

    template<class Stream, class Stage>
    auto operator|(Stream&& s, Stage&& stage) {
        return std::forward<Stage>(stage)(std::forward<Stream>(s));
    }

    template<class T>
    concept sink_type = requires(const T& v, AVPacket* p) {
        { v.is_mine(p) } -> std::same_as<bool>;
        { v.operator()(packet_uptr{}) } -> std::same_as<void>;
    };

    template<class... Sinks>
    requires (sink_type<std::remove_reference_t<Sinks>> &&...)
    auto tee(Sinks&&... sinks) {
        return [...ss = std::forward<Sinks>(sinks)](auto packets) mutable {
            while (packets) {
                if (auto packet = packets(); packet) {
                    const auto routed = ((ss.is_mine(packet.get()) ? (ss(std::move(packet)), true) : false) || ...);
                    (void)routed;
                }
            }
        };
    }

    inline auto single_packet(packet_uptr packet) -> coro::produce::generator_t<packet_uptr> {
        if (packet)
            co_yield std::move(packet);
    }

    class pipeline_t {
    public:

        struct options_t {
            std::string source;
            std::string destination;
            AVCodecID video_codec_id;
            AVCodecID audio_codec_id;
            bool need_restart;
        };

    private:

        logger_t::ptr_t logger;
        demuxer_t demuxer;

        struct stream_t final {

            std::unique_ptr<decoder_t> decoder = nullptr;
            std::unique_ptr<encoder_t> encoder = nullptr;

            explicit stream_t(const auto& ref, const AVCodecID codec_id, logger_t::ptr_t logger)
                : decoder(ref ? std::make_unique<decoder_t>(ref, logger) : nullptr)
                , encoder(ref ? std::make_unique<encoder_t>(ref, codec_id, logger) : nullptr)
            {
                if (ref && !(*decoder)) {
                    logger->log<logger_t::level_e::critical, "[{}:{}:{}] !!! pipeline: failed to initialize decoder !!!">(
                        __FILE__, __LINE__, __func__);
                }
                if (ref && !(*encoder)) {
                    logger->log<logger_t::level_e::critical, "[{}:{}:{}] !!! pipeline: failed to initialize encoder (codec_id={}) !!!">(
                        __FILE__, __LINE__, __func__, static_cast<int>(codec_id));
                }
            }

            ~stream_t() = default;

            operator bool() const noexcept {
                return decoder && *decoder && encoder && *encoder;
            }

            inline auto context() const {
                return encoder ? encoder->context() : nullptr;
            }

            inline auto check_encoder_alive() const -> bool {
                return encoder && *encoder && encoder->context() && !encoder->failed();
            }
        };

        stream_t video;
        stream_t audio;

        muxer_t muxer;

        bool ready_ = false;
        bool valid_ = true;

        class sink_t final {

            enum AVMediaType media_type;
            demuxer_t& demuxer;
            stream_t& stream;
            muxer_t& muxer;
            logger_t::ptr_t logger;
            bool& valid;

            inline auto stream_alive() const {
                return (bool)stream;
            }

        public:

            ~sink_t() = default;

            inline auto is_mine(AVPacket* p) const {
                assert(p);
                return stream_alive() && p && demuxer.packet_type(p) == media_type;
            }

            auto operator()(packet_uptr packet) const -> void {

                if (stream_alive() && packet) {

                    if (valid = stream.check_encoder_alive(); valid) {

                        if (stream)
                            consume(muxer, media_type, logger)(
                                single_packet(std::move(packet))
                                | decode_with(*stream.decoder)
                                | encode_with(*stream.encoder)
                            );

                        if (valid = stream.check_encoder_alive(); valid)
                            valid = check_muxer_alive();
                    }
                }
            }

            auto operator()(auto packets) const -> void {

                if (!stream_alive())
                    return;

                while (packets && valid) {
                    if (auto packet = packets(); packet && is_mine(packet.get()))
                        operator()(std::move(packet));
                }
            }

            auto check_muxer_alive() const -> bool {
                if (!muxer) {
                    logger->log<logger_t::level_e::critical, "[{}:{}:{}] !!! muxer became invalid (type={}) !!!">(
                        __FILE__, __LINE__, __func__, static_cast<int>(media_type));
                    return false;
                }
                return true;
            }

            explicit sink_t(
                enum AVMediaType media_type_,
                demuxer_t& demuxer_,
                stream_t& stream_,
                muxer_t& muxer_,
                logger_t::ptr_t logger_,
                bool& valid_)
                : media_type(media_type_)
                , demuxer(demuxer_)
                , stream(stream_)
                , muxer(muxer_)
                , logger(logger_)
                , valid(valid_)
            {}
        };

        auto run_pipeline() -> bool {
            if (!ready_) {
                logger->log<logger_t::level_e::critical, "[{}:{}:{}] !!! pipeline is not ready to run !!!">(
                    __FILE__, __LINE__, __func__);
                return false;
            }

            if (video && audio) {
                demuxer.retrieve() | tee(
                    sink_t(AVMEDIA_TYPE_VIDEO, demuxer, video, muxer, logger, valid_),
                    sink_t(AVMEDIA_TYPE_AUDIO, demuxer, audio, muxer, logger, valid_)
                );

            } else if (video) {
                demuxer.retrieve() | sink_t(AVMEDIA_TYPE_VIDEO, demuxer, video, muxer, logger, valid_);

            } else if (audio) {
                demuxer.retrieve() | sink_t(AVMEDIA_TYPE_AUDIO, demuxer, audio, muxer, logger, valid_);

            } else {
                logger->log<logger_t::level_e::critical, "[{}:{}:{}] !!! pipeline has no active audio/video branches !!!">(
                    __FILE__, __LINE__, __func__);
                return false;
            }

            if (!valid_)
                return false;

            if (video) {

                consume(muxer, AVMEDIA_TYPE_VIDEO, logger)(flush_decoder(*video.decoder) | encode_with(*video.encoder));
                if (!video.check_encoder_alive()) {
                    logger->log<logger_t::level_e::critical, "[{}:{}:{}] !!! video encoder failed on decoder-flush stage !!!">(
                        __FILE__, __LINE__, __func__);
                    return false;
                }
                consume(muxer, AVMEDIA_TYPE_VIDEO, logger)(flush_encoder(*video.encoder));
                if (!video.check_encoder_alive()) {
                    logger->log<logger_t::level_e::critical, "[{}:{}:{}] !!! video encoder failed on encoder-flush stage !!!">(
                        __FILE__, __LINE__, __func__);
                    return false;
                }
            }

            if (audio) {

                consume(muxer, AVMEDIA_TYPE_AUDIO, logger)(flush_decoder(*audio.decoder) | encode_with(*audio.encoder));
                if (!audio.check_encoder_alive()) {
                    logger->log<logger_t::level_e::critical, "[{}:{}:{}] !!! audio encoder failed on decoder-flush stage !!!">(
                        __FILE__, __LINE__, __func__);
                    return false;
                }
                consume(muxer, AVMEDIA_TYPE_AUDIO, logger)(flush_encoder(*audio.encoder));
                if (!audio.check_encoder_alive()) {
                    logger->log<logger_t::level_e::critical, "[{}:{}:{}] !!! audio encoder failed on encoder-flush stage !!!">(
                        __FILE__, __LINE__, __func__);
                    return false;
                }
            }

            return true;
        }

    public:

        explicit pipeline_t(const options_t& options, logger_t::ptr_t logger_)
            : logger(logger_->clone())
            , demuxer(options.source, logger)
            , video(demuxer.video_reference(), options.video_codec_id, logger)
            , audio(demuxer.audio_reference(), options.audio_codec_id, logger)
            , muxer(muxer_t::options_t{ .output_filename = options.destination }, logger)
        {
            if (!demuxer) {
                logger->log<logger_t::level_e::critical, "[{}:{}:{}] !!! pipeline: failed to initialize demuxer for source `{}` !!!">(
                    __FILE__, __LINE__, __func__, options.source);
                return;
            }
            if (!muxer) {
                logger->log<logger_t::level_e::critical, "[{}:{}:{}] !!! pipeline: failed to initialize muxer for destination `{}` !!!">(
                    __FILE__, __LINE__, __func__, options.destination);
                return;
            }

            if (auto sc = stream_context_t{
                    .video = video.context(),
                    .audio = audio.context(),
                }; sc) {
                muxer.set_stream_context(sc);
                ready_ = true;
            }
        }

        static auto run(const options_t& options, logger_t::ptr_t logger) {
            do {
                if (!pipeline_t(options, logger).run_pipeline())
                    break;
            } while (options.need_restart);
        }
    };

}
