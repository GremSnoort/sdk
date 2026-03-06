#pragma once

// std
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

// gsdk
#include <gremsnoort/sdk/ffmpeg/smartptr.h>
#include <gremsnoort/sdk/logger/logger.h>

// conan
#ifdef __cplusplus
extern "C" {
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/timestamp.h>

#ifdef __cplusplus
}
#endif

namespace gremsnoort::sdk::ffmpeg {

    class muxer_t {
    public:

        struct options_t {
            std::string output_filename;
        };

    private:

        logger_t::ptr_t logger_;
        options_t options_;
        output_format_uptr out_ctx_{nullptr};
        AVStream* video_stream_ = nullptr;
        AVStream* audio_stream_ = nullptr;
        int64_t last_video_mux_dts_ = AV_NOPTS_VALUE;
        int64_t last_audio_mux_dts_ = AV_NOPTS_VALUE;
        stream_context_t stream_ctxs_{};
        bool initialized_ = false;
        bool header_written_ = false;
        bool trailer_written_ = false;

        static auto format_from_extension(const std::string& filename) -> const char* {
            const auto dot = filename.find_last_of('.');
            if (dot == std::string::npos || dot + 1 >= filename.size())
                return nullptr;

            auto ext = filename.substr(dot + 1);
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });

            static const std::unordered_map<std::string, const char*> k_ext2fmt{
                {"mp4",  "mp4"},
                {"m4v",  "mp4"},
                {"mov",  "mov"},
                {"mkv",  "matroska"},
                {"webm", "webm"},
                {"ts",   "mpegts"},
                {"flv",  "flv"},
                {"avi",  "avi"},
                {"mpg",  "mpeg"},
                {"mpeg", "mpeg"},
            };

            if (const auto it = k_ext2fmt.find(ext); it != k_ext2fmt.end())
                return it->second;

            return nullptr;
        }

        auto add_stream_from_encoder(const AVCodecContext* enc_ctx, AVMediaType media_type) -> bool {
            if (!out_ctx_) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! muxer context is not initialized !!!">(
                    __FILE__, __LINE__, __func__);
                return false;
            }
            if (!enc_ctx) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! add_stream_from_encoder got NULL enc_ctx !!!">(
                    __FILE__, __LINE__, __func__);
                return false;
            }
            if (enc_ctx->codec_type != media_type) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! codec context type {} does not match requested stream type {} !!!">(
                    __FILE__, __LINE__, __func__, static_cast<int>(enc_ctx->codec_type), static_cast<int>(media_type));
                return false;
            }

            auto*& selected_stream = (media_type == AVMEDIA_TYPE_VIDEO) ? video_stream_ : audio_stream_;
            if (selected_stream) {
                logger_->log<logger_t::level_e::warn, "[{}:{}:{}] {} stream already added">(
                    __FILE__, __LINE__, __func__,
                    (media_type == AVMEDIA_TYPE_VIDEO) ? "video" : "audio");
                return true;
            }

            auto* stream = avformat_new_stream(out_ctx_.get(), nullptr);
            if (!stream) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! FAILED avformat_new_stream !!!">(
                    __FILE__, __LINE__, __func__);
                return false;
            }

            if (const auto ret = avcodec_parameters_from_context(stream->codecpar, enc_ctx); ret < 0) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! FAILED avcodec_parameters_from_context: {} !!!">(
                    __FILE__, __LINE__, __func__, ret);
                return false;
            }

            stream->time_base = enc_ctx->time_base;
            if (media_type == AVMEDIA_TYPE_VIDEO) {
                stream->avg_frame_rate = enc_ctx->framerate;
            }

            selected_stream = stream;
            return true;
        }

        auto initialize_if_needed(const AVPacket* first_packet) -> bool {
            if (initialized_)
                return true;
            if (!out_ctx_) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! muxer context is not initialized !!!">(
                    __FILE__, __LINE__, __func__);
                return false;
            }
            if (!stream_ctxs_) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! stream context is not configured for lazy muxer init !!!">(
                    __FILE__, __LINE__, __func__);
                return false;
            }

            // first_packet is intentionally accepted as input to keep lazy-init anchored to the first real packet.
            [[maybe_unused]] const auto* first = first_packet;

            if (stream_ctxs_.video) {
                if (!add_video_stream_from_encoder(stream_ctxs_.video))
                    return false;
            }
            if (stream_ctxs_.audio) {
                if (!add_audio_stream_from_encoder(stream_ctxs_.audio))
                    return false;
            }

            initialized_ = write_header();
            return initialized_;
        }

        auto write_packet_for(packet_uptr packet, AVMediaType media_type) -> bool {
            if (!out_ctx_) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! muxer context is not initialized !!!">(
                    __FILE__, __LINE__, __func__);
                return false;
            }
            if (!packet)
                return true;
            if (!initialize_if_needed(packet.get()))
                return false;
            if (!header_written_)
                return false;

            auto* stream = (media_type == AVMEDIA_TYPE_VIDEO) ? video_stream_ : audio_stream_;
            auto& last_mux_dts = (media_type == AVMEDIA_TYPE_VIDEO) ? last_video_mux_dts_ : last_audio_mux_dts_;
            if (!stream) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! target stream for type {} is not initialized !!!">(
                    __FILE__, __LINE__, __func__, static_cast<int>(media_type));
                return false;
            }
            const auto stream_index = stream->index;
            if (stream_index < 0 || static_cast<unsigned int>(stream_index) >= out_ctx_->nb_streams) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! invalid stream index {} for type {} (nb_streams={}) !!!">(
                    __FILE__, __LINE__, __func__, stream_index, static_cast<int>(media_type), out_ctx_->nb_streams);
                return false;
            }
            if (out_ctx_->streams[stream_index] != stream) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! stream pointer/index mismatch for index {} !!!">(
                    __FILE__, __LINE__, __func__, stream_index);
                return false;
            }
            if (!out_ctx_->streams[stream_index]) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! output stream pointer is NULL for index {} !!!">(
                    __FILE__, __LINE__, __func__, stream_index);
                return false;
            }
            packet->stream_index = stream_index;
            if (packet->time_base.num <= 0 || packet->time_base.den <= 0) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! invalid packet time_base {}/{} for stream index {} !!!">(
                    __FILE__, __LINE__, __func__, packet->time_base.num, packet->time_base.den, stream_index);
                return false;
            }
            if (stream->time_base.num <= 0 || stream->time_base.den <= 0) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! invalid output stream time_base {}/{} for index {} !!!">(
                    __FILE__, __LINE__, __func__, stream->time_base.num, stream->time_base.den, stream_index);
                return false;
            }
            av_packet_rescale_ts(packet.get(), packet->time_base, stream->time_base);
            packet->time_base = stream->time_base;

            if (!(out_ctx_->oformat->flags & AVFMT_NOTIMESTAMPS)) {
                if (packet->dts != AV_NOPTS_VALUE &&
                    packet->pts != AV_NOPTS_VALUE &&
                    packet->dts > packet->pts) {

                    const auto pivot = (last_mux_dts == AV_NOPTS_VALUE)
                                           ? packet->pts
                                           : (last_mux_dts + 1);
                    const auto minv = std::min({packet->pts, packet->dts, pivot});
                    const auto maxv = std::max({packet->pts, packet->dts, pivot});
                    const auto guessed = packet->pts + packet->dts + pivot - minv - maxv;

                    logger_->log<logger_t::level_e::warn, "[{}:{}:{}] Invalid DTS/PTS (dts={}, pts={}), replacing by guess={}">(
                        __FILE__, __LINE__, __func__, packet->dts, packet->pts, guessed);
                    packet->dts = guessed;
                    packet->pts = guessed;
                }

                if ((media_type == AVMEDIA_TYPE_AUDIO || media_type == AVMEDIA_TYPE_VIDEO || media_type == AVMEDIA_TYPE_SUBTITLE) &&
                    packet->dts != AV_NOPTS_VALUE &&
                    last_mux_dts != AV_NOPTS_VALUE) {

                    const auto strict_step = (out_ctx_->oformat->flags & AVFMT_TS_NONSTRICT) ? int64_t{0} : int64_t{1};
                    const auto max_dts = last_mux_dts + strict_step;

                    if (packet->dts < max_dts) {
                        logger_->log<logger_t::level_e::warn, "[{}:{}:{}] Non-monotonic DTS; previous={}, current={}, clamped={}">(
                            __FILE__, __LINE__, __func__, last_mux_dts, packet->dts, max_dts);

                        if (packet->pts >= packet->dts)
                            packet->pts = std::max(packet->pts, max_dts);
                        packet->dts = max_dts;
                    }
                }
            }

            last_mux_dts = packet->dts;

            if (const auto ret = av_interleaved_write_frame(out_ctx_.get(), packet.get()); ret < 0) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! FAILED av_interleaved_write_frame: {} !!!">(
                    __FILE__, __LINE__, __func__, ret);
                return false;
            }

            return true;
        }

        auto add_video_stream_from_encoder(const AVCodecContext* enc_ctx) -> bool {
            return add_stream_from_encoder(enc_ctx, AVMEDIA_TYPE_VIDEO);
        }

        auto add_audio_stream_from_encoder(const AVCodecContext* enc_ctx) -> bool {
            return add_stream_from_encoder(enc_ctx, AVMEDIA_TYPE_AUDIO);
        }

        auto write_header() -> bool {
            if (!out_ctx_) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! muxer context is not initialized !!!">(
                    __FILE__, __LINE__, __func__);
                return false;
            }
            if (header_written_)
                return true;
            if (!video_stream_ && !audio_stream_) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! write_header called before adding any stream !!!">(
                    __FILE__, __LINE__, __func__);
                return false;
            }

            if (!(out_ctx_->oformat->flags & AVFMT_NOFILE)) {
                if (const auto ret = avio_open(&out_ctx_->pb, options_.output_filename.c_str(), AVIO_FLAG_WRITE); ret < 0) {
                    logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! FAILED avio_open `{}`: {} !!!">(
                        __FILE__, __LINE__, __func__, options_.output_filename, ret);
                    return false;
                }
            }

            if (const auto ret = avformat_write_header(out_ctx_.get(), nullptr); ret < 0) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! FAILED avformat_write_header: {} !!!">(
                    __FILE__, __LINE__, __func__, ret);
                return false;
            }

            header_written_ = true;
            return true;
        }

        auto write_trailer() -> bool {
            if (!out_ctx_) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! muxer context is not initialized !!!">(
                    __FILE__, __LINE__, __func__);
                return false;
            }
            if (!header_written_)
                return true;
            if (trailer_written_)
                return true;

            if (const auto ret = av_write_trailer(out_ctx_.get()); ret < 0) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! FAILED av_write_trailer: {} !!!">(
                    __FILE__, __LINE__, __func__, ret);
                return false;
            }

            trailer_written_ = true;
            return true;
        }

    public:

        explicit muxer_t(const options_t& options, logger_t::ptr_t logger)
            : logger_(logger->clone())
            , options_(options) {

            if (options_.output_filename.empty()) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! output_filename is empty !!!">(
                    __FILE__, __LINE__, __func__);
                return;
            }

            AVFormatContext* ptr = nullptr;
            const auto* format_name = format_from_extension(options_.output_filename);
            if (const auto ret = avformat_alloc_output_context2(
                    &ptr,
                    nullptr,
                    format_name,
                    options_.output_filename.c_str()); ret < 0 || !ptr) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! FAILED avformat_alloc_output_context2 for `{}`: {} !!!">(
                    __FILE__, __LINE__, __func__, options_.output_filename, ret);
                return;
            }

            out_ctx_.reset(ptr);
        }

        auto set_stream_context(const stream_context_t& stream_ctxs) -> void {
            stream_ctxs_ = stream_ctxs;
        }

        ~muxer_t() {
            if (!out_ctx_)
                return;

            write_trailer();

            if (!(out_ctx_->oformat->flags & AVFMT_NOFILE) && out_ctx_->pb) {
                avio_closep(&out_ctx_->pb);
            }
        }

        explicit operator bool() const noexcept {
            return out_ctx_.get() != nullptr;
        }

        auto write_video_packet(packet_uptr packet) -> bool {
            return write_packet_for(std::move(packet), AVMEDIA_TYPE_VIDEO);
        }

        auto write_audio_packet(packet_uptr packet) -> bool {
            return write_packet_for(std::move(packet), AVMEDIA_TYPE_AUDIO);
        }
    };

    auto consume_video(muxer_t& muxer, logger_t::ptr_t logger) {
        return [&muxer, logger = std::move(logger)](auto&& packets) {
            std::size_t n = 0;
            while (packets) {
                auto p = packets();
                if (!p) continue;
                ++n;

                if (!muxer.write_video_packet(std::move(p))) {
                    logger->log<logger_t::level_e::critical, "[{}:{}:{}] !!! failed to mux video packet !!!">(
                                __FILE__, __LINE__, __func__);
                    return;
                }
            }
            std::printf("muxed video packets: %zu\n", n);
        };
    }

    auto consume_audio(muxer_t& muxer, logger_t::ptr_t logger) {
        return [&muxer, logger = std::move(logger)](auto&& packets) {
            std::size_t n = 0;
            while (packets) {
                auto p = packets();
                if (!p) continue;
                ++n;

                if (!muxer.write_audio_packet(std::move(p))) {
                    logger->log<logger_t::level_e::critical, "[{}:{}:{}] !!! failed to mux audio packet !!!">(
                                __FILE__, __LINE__, __func__);
                    return;
                }
            }
            std::printf("muxed audio packets: %zu\n", n);
        };
    }

}
