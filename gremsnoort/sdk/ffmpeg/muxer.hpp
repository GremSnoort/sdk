#pragma once

// std
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// gsdk
#include <gremsnoort/sdk/forward/inplaced.hpp>
#include <gremsnoort/sdk/ffmpeg/smartptr.h>
#include <gremsnoort/sdk/logger/logger.h>

// conan
#ifdef __cplusplus
extern "C" {
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/timestamp.h>

#ifdef __cplusplus
}
#endif

namespace gremsnoort::sdk::ffmpeg {

    class muxer_t final {
    public:

        struct options_t {
            std::string output_filename;
        };

    private:

        logger_t::ptr_t logger_;
        options_t options_;
        output_format_uptr out_ctx_{nullptr};

        struct stream_t {
            AVStream* stream = nullptr;
            int64_t last_dts = AV_NOPTS_VALUE;
            std::vector<packet_uptr> pending_packets;
        };
        gremsnoort::sdk::inplaced_t<stream_t> muxed_streams_;

        stream_context_t stream_ctxs_{};
        bool initialized_ = false;
        bool header_written_ = false;
        bool trailer_written_ = false;

        inline auto index(AVMediaType media_type) const {
            const auto i = static_cast<std::size_t>(media_type);
            assert(muxed_streams_.check_index(i));
            return i;
        }

        inline auto& get_stream(AVMediaType media_type) {
            return muxed_streams_.at(index(media_type)).stream;
        }

        inline auto& get_last_dts(AVMediaType media_type) {
            return muxed_streams_.at(index(media_type)).last_dts;
        }

        inline auto has_streams() const {
            auto check = false;
            for (std::size_t i = 0; i < muxed_streams_.size(); ++i)
                check = check || muxed_streams_.at(i).stream;
            return check;
        }

        inline auto& pending_packets(AVMediaType media_type) {
            return muxed_streams_.at(index(media_type)).pending_packets;
        }

        inline auto pending_packets_count() const {
            std::size_t n = 0;
            for (std::size_t i = 0; i < muxed_streams_.size(); ++i) {
                n += muxed_streams_.at(i).pending_packets.size();
            }
            return n;
        }

        inline auto required_streams_ready() const -> bool {
            const bool need_video = stream_ctxs_.video != nullptr;
            const bool need_audio = stream_ctxs_.audio != nullptr;
            const bool has_video = !need_video || muxed_streams_.at(static_cast<std::size_t>(AVMEDIA_TYPE_VIDEO)).stream != nullptr;
            const bool has_audio = !need_audio || muxed_streams_.at(static_cast<std::size_t>(AVMEDIA_TYPE_AUDIO)).stream != nullptr;
            return has_video && has_audio;
        }

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

            auto& selected_stream = get_stream(media_type);
            if (selected_stream) {
                logger_->log<logger_t::level_e::warn, "[{}:{}:{}] {} stream already added">(
                    __FILE__, __LINE__, __func__,
                    (media_type == AVMEDIA_TYPE_VIDEO) ? "video" : "audio");
                return true;
            }

            auto stream = avformat_new_stream(out_ctx_.get(), nullptr);
            if (!stream) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! FAILED avformat_new_stream !!!">(
                    __FILE__, __LINE__, __func__);
                return false;
            }

            if (const auto ret = avcodec_parameters_from_context(stream->codecpar, enc_ctx); ret < 0) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! FAILED avcodec_parameters_from_context: {} !!!">(
                    __FILE__, __LINE__, __func__, ret);
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! muxer context is invalidated to avoid partially added stream state !!!">(
                    __FILE__, __LINE__, __func__);

                for (std::size_t i = 0; i < muxed_streams_.size(); ++i) {
                    auto& state = muxed_streams_.at(i);
                    state.stream = nullptr;
                    state.last_dts = AV_NOPTS_VALUE;
                    state.pending_packets.clear();
                }
                out_ctx_.reset();
                header_written_ = false;
                initialized_ = false;
                trailer_written_ = false;
                return false;
            }

            stream->time_base = enc_ctx->time_base;
            if (media_type == AVMEDIA_TYPE_VIDEO) {
                stream->avg_frame_rate = enc_ctx->framerate;
            }

            selected_stream = stream;
            return true;
        }

        auto write_packet_prepared(packet_uptr packet, AVMediaType media_type) -> bool {
            if (!out_ctx_) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! muxer context is not initialized !!!">(
                    __FILE__, __LINE__, __func__);
                return false;
            }
            if (!packet)
                return true;
            if (!header_written_)
                return false;

            auto& stream = get_stream(media_type);
            auto& last_mux_dts = get_last_dts(media_type);
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

        auto flush_pending_packets() -> bool {

            for (std::size_t i = 0; i < muxed_streams_.size(); ++i) {
                const auto media_type = static_cast<AVMediaType>(i);
                auto& stream = muxed_streams_.at(i);
                for (auto& packet : stream.pending_packets) {
                    if (packet && !write_packet_prepared(std::move(packet), media_type))
                        return false;
                }
                stream.pending_packets.clear();
            }
            return true;
        }

        auto initialize_if_needed(AVMediaType media_type, const AVPacket* first_packet) -> bool {
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
            [[maybe_unused]] const auto first = first_packet;
            const auto need_video = (media_type == AVMEDIA_TYPE_VIDEO);
            const auto need_audio = (media_type == AVMEDIA_TYPE_AUDIO);
            auto& requested_stream = get_stream(media_type);

            // FFmpeg contract: new output streams must be created before avformat_write_header().
            if (header_written_ && !requested_stream) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! cannot add new stream type {} after avformat_write_header() !!!">(
                    __FILE__, __LINE__, __func__, static_cast<int>(media_type));
                return false;
            }

            if (need_video && !get_stream(media_type)) {
                if (!stream_ctxs_.video) {
                    logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! video stream context is not configured for lazy muxer init !!!">(
                        __FILE__, __LINE__, __func__);
                    return false;
                }
                if (!add_video_stream_from_encoder(stream_ctxs_.video))
                    return false;
            }

            if (need_audio && !get_stream(media_type)) {
                if (!stream_ctxs_.audio) {
                    logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! audio stream context is not configured for lazy muxer init !!!">(
                        __FILE__, __LINE__, __func__);
                    return false;
                }
                if (!add_audio_stream_from_encoder(stream_ctxs_.audio))
                    return false;
            }

            if (!header_written_ && required_streams_ready()) {
                if (!write_header())
                    return false;
                initialized_ = true;
                if (!flush_pending_packets())
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
            if (!has_streams()) {
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
            if (!header_written_) {
                const auto pending = pending_packets_count();
                const bool needs_streams = stream_ctxs_.video || stream_ctxs_.audio;
                if (pending > 0 || needs_streams) {
                    logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! finalize failed: header is not written (pending_packets={}, needs_video={}, needs_audio={}, has_video_stream={}, has_audio_stream={}) !!!">(
                        __FILE__, __LINE__, __func__,
                        pending,
                        static_cast<int>(stream_ctxs_.video != nullptr),
                        static_cast<int>(stream_ctxs_.audio != nullptr),
                        static_cast<int>(get_stream(AVMEDIA_TYPE_VIDEO) != nullptr),
                        static_cast<int>(get_stream(AVMEDIA_TYPE_AUDIO) != nullptr));
                    return false;
                }
                return true;
            }
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

        static inline constexpr auto streams_count = static_cast<std::size_t>(AVMEDIA_TYPE_NB);

    public:

        explicit muxer_t(const options_t& options, logger_t::ptr_t logger)
            : logger_(logger->clone())
            , options_(options)
            , muxed_streams_(streams_count) {

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

            std::size_t index_out = 0;
            while (index_out < streams_count) {

                if (!muxed_streams_.add(index_out, stream_t{ .stream = nullptr, .last_dts = AV_NOPTS_VALUE, .pending_packets = {} }))
                    break;
            }
            assert(muxed_streams_.size() == streams_count);

            out_ctx_.reset(ptr);
        }

        auto set_stream_context(const stream_context_t& stream_ctxs) -> void {
            stream_ctxs_ = stream_ctxs;
        }

        ~muxer_t() {
            if (!out_ctx_)
                return;

            if (!write_trailer()) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! muxer finalize failed in destructor !!!">(
                    __FILE__, __LINE__, __func__);
            }

            if (!(out_ctx_->oformat->flags & AVFMT_NOFILE) && out_ctx_->pb) {
                avio_closep(&out_ctx_->pb);
            }
        }

        explicit operator bool() const noexcept {
            return out_ctx_.get() != nullptr;
        }

        auto write_packet_for(packet_uptr packet, AVMediaType media_type) -> bool {
            if (!out_ctx_) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! muxer context is not initialized !!!">(
                    __FILE__, __LINE__, __func__);
                return false;
            }
            if (!packet)
                return true;
            if (!initialize_if_needed(media_type, packet.get()))
                return false;
            if (!header_written_) {
                pending_packets(media_type).emplace_back(std::move(packet));
                return true;
            }

            return write_packet_prepared(std::move(packet), media_type);
        }
    };

    auto consume(muxer_t& muxer, const enum AVMediaType media_type, logger_t::ptr_t logger) {
        return [&muxer, media_type = std::move(media_type), logger = std::move(logger)](auto&& packets) {
            std::size_t n = 0;
            while (packets) {
                auto p = packets();
                if (!p) continue;
                ++n;

                if (!muxer.write_packet_for(std::move(p), media_type)) {
                    logger->log<logger_t::level_e::critical, "[{}:{}:{}] !!! failed to mux packet !!!">(
                                __FILE__, __LINE__, __func__);
                    return;
                }
            }
            std::printf("muxed packets: %zu\n", n);
        };
    }

}
