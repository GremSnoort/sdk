#pragma once

// std
#include <cstddef>
#include <cstdio>
#include <memory>
#include <utility>

// gsdk
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
        { v(packet_uptr{}) } -> std::same_as<void>;
    };

    template<class LeftSink, class RightSink>
    requires sink_type<LeftSink> && sink_type<RightSink>
    auto tee(LeftSink&& left_sink, RightSink&& right_sink) {
        return [left = std::forward<LeftSink>(left_sink), right = std::forward<RightSink>(right_sink)](auto packets) mutable {
            while (packets) {
                auto packet = packets();
                if (!packet)
                    continue;

                if (left.is_mine(packet.get())) {
                    left(std::move(packet));
                }
                else if (right.is_mine(packet.get())) {
                    right(std::move(packet));
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

        std::unique_ptr<decoder_t> decoder_video = nullptr;
        std::unique_ptr<encoder_t> encoder_video = nullptr;
        std::unique_ptr<decoder_t> decoder_audio = nullptr;
        std::unique_ptr<encoder_t> encoder_audio = nullptr;

        muxer_t muxer;

        class sink_t {
        protected:
            enum AVMediaType media_type;
            demuxer_t& demuxer;
            decoder_t& decoder;
            encoder_t& encoder;
            muxer_t& muxer;
            logger_t::ptr_t logger;

        public:
            virtual ~sink_t() = default;

            inline auto is_mine(AVPacket* p) const {
                assert(p);
                return demuxer.packet_type(p) == media_type;
            }

            virtual auto operator()(packet_uptr packet) const -> void = 0;

            explicit sink_t(
                enum AVMediaType media_type_,
                demuxer_t& demuxer_,
                decoder_t& decoder_,
                encoder_t& encoder_,
                muxer_t& muxer_,
                logger_t::ptr_t logger_)
                : media_type(media_type_)
                , demuxer(demuxer_)
                , decoder(decoder_)
                , encoder(encoder_)
                , muxer(muxer_)
                , logger(logger_)
            {}
        };

        class video_sink_t final : public sink_t {
        public:
            virtual auto operator()(packet_uptr packet) const -> void final {
                if (!packet)
                    return;

                consume_video(muxer, logger)(
                    single_packet(std::move(packet))
                    | decode_with(decoder)
                    | encode_with(encoder)
                );
            }

            explicit video_sink_t(
                demuxer_t& demuxer,
                decoder_t& decoder,
                encoder_t& encoder,
                muxer_t& muxer,
                logger_t::ptr_t logger)
                : sink_t(AVMEDIA_TYPE_VIDEO, demuxer, decoder, encoder, muxer, logger)
            {}
        };

        class audio_sink_t final : public sink_t {
        public:
            virtual auto operator()(packet_uptr packet) const -> void final {
                if (!packet)
                    return;

                consume_audio(muxer, logger)(
                    single_packet(std::move(packet))
                    | decode_with(decoder)
                    | encode_with(encoder)
                );
            }

            explicit audio_sink_t(
                demuxer_t& demuxer,
                decoder_t& decoder,
                encoder_t& encoder,
                muxer_t& muxer,
                logger_t::ptr_t logger)
                : sink_t(AVMEDIA_TYPE_AUDIO, demuxer, decoder, encoder, muxer, logger)
            {}
        };

        auto run_pipeline() {

            demuxer.retrieve() | tee(
                video_sink_t(demuxer, *decoder_video, *encoder_video, muxer, logger),
                audio_sink_t(demuxer, *decoder_audio, *encoder_audio, muxer, logger)
            );

            if (decoder_video && encoder_video) {
                // lifetime bug repro:
                // coroutine возобновилась после того, как объект, чьи данные она использует (stage closure/его captures), уже мертв;
                // auto gen = flush_decoder(*decoder_video) | encode_with(*encoder_video); + std::move(gen)
                consume_video(muxer, logger)(flush_decoder(*decoder_video) | encode_with(*encoder_video));
                consume_video(muxer, logger)(flush_encoder(*encoder_video));
            }

            if (decoder_audio && encoder_audio) {
                consume_audio(muxer, logger)(flush_decoder(*decoder_audio) | encode_with(*encoder_audio));
                consume_audio(muxer, logger)(flush_encoder(*encoder_audio));
            }
        }

    public:

        explicit pipeline_t(const options_t& options, logger_t::ptr_t logger_)
            : logger(logger_->clone())
            , demuxer(options.source, logger)
            , decoder_video(nullptr)
            , encoder_video(nullptr)
            , decoder_audio(nullptr)
            , encoder_audio(nullptr)
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

            if (demuxer.video_reference()) {
                decoder_video = std::make_unique<decoder_t>(demuxer.video_reference(), logger);
                if (!(*decoder_video)) {
                    logger->log<logger_t::level_e::critical, "[{}:{}:{}] !!! pipeline: failed to initialize video decoder from source `{}` !!!">(
                        __FILE__, __LINE__, __func__, options.source);
                    return;
                }

                encoder_video = std::make_unique<encoder_t>(demuxer.video_reference(), options.video_codec_id, logger);
                if (!(*encoder_video)) {
                    logger->log<logger_t::level_e::critical, "[{}:{}:{}] !!! pipeline: failed to initialize video encoder (codec_id={}) !!!">(
                        __FILE__, __LINE__, __func__, static_cast<int>(options.video_codec_id));
                    return;
                }
            }

            if (demuxer.audio_reference()) {
                decoder_audio = std::make_unique<decoder_t>(demuxer.audio_reference(), logger);
                if (!(*decoder_audio)) {
                    logger->log<logger_t::level_e::critical, "[{}:{}:{}] !!! pipeline: failed to initialize audio decoder from source `{}` !!!">(
                        __FILE__, __LINE__, __func__, options.source);
                    return;
                }

                encoder_audio = std::make_unique<encoder_t>(demuxer.audio_reference(), options.audio_codec_id, logger);
                if (!(*encoder_audio)) {
                    logger->log<logger_t::level_e::critical, "[{}:{}:{}] !!! pipeline: failed to initialize audio encoder (codec_id={}) !!!">(
                        __FILE__, __LINE__, __func__, static_cast<int>(options.audio_codec_id));
                    return;
                }
            }

            auto sc = stream_context_t{
                .video = encoder_video ? encoder_video->context() : nullptr,
                .audio = encoder_audio ? encoder_audio->context() : nullptr
            };
            muxer.set_stream_context(sc);
        }
        

        static auto run(const options_t& options, logger_t::ptr_t logger) {
            do {
                pipeline_t(options, logger).run_pipeline();
            } while(options.need_restart);
        }
    };

}
