#pragma once

// std
#include <cassert>
#include <cmath>
#include <string>

// gsdk
#include <gremsnoort/sdk/forward/coro.hpp>
#include <gremsnoort/sdk/ffmpeg/smartptr.h>
#include <gremsnoort/sdk/logger/logger.h>

// conan

#ifdef __cplusplus
extern "C" {
#endif

#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>

#ifdef __cplusplus
}
#endif

namespace gremsnoort::sdk::ffmpeg {

    class demuxer_t {
    public:

        struct options_t {
            // Keep original timeline (fftools-like copy_ts behavior).
            bool copy_ts = false;
            // When copy_ts is disabled, normalize first packet to 0.
            bool start_at_zero = true;
            // Global input timestamp shift in AV_TIME_BASE units.
            int64_t ts_offset = 0;
            // Per-stream timestamp scale.
            double video_ts_scale = 1.0;
            double audio_ts_scale = 1.0;
        };

    private:

        struct stream_state_t {
            int64_t first_dts_avtb = AV_NOPTS_VALUE;
            int64_t next_dts_avtb = AV_NOPTS_VALUE;
            int64_t dts_avtb = AV_NOPTS_VALUE;
            bool wrap_correction_done = false;
            bool saw_first_ts = false;
        };

        logger_t::ptr_t logger_;

        const std::string filename_;
        options_t options_;
        input_format_uptr fmt_ctx_;

        reference_t video_;
        reference_t audio_;
        stream_state_t video_state_;
        stream_state_t audio_state_;
        int64_t start_at_zero_origin_avtb_ = AV_NOPTS_VALUE;

        inline auto get_reference(const enum AVMediaType codec_type) -> reference_t* {
            switch (codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                return &video_;
            case AVMEDIA_TYPE_AUDIO:
                return &audio_;
            case AVMEDIA_TYPE_UNKNOWN:
            case AVMEDIA_TYPE_DATA:
            case AVMEDIA_TYPE_SUBTITLE:
            case AVMEDIA_TYPE_ATTACHMENT:
            case AVMEDIA_TYPE_NB:
                break;
            }
            return nullptr;
        }

        inline auto codec_type(const unsigned int stream_index) const {
            assert(stream_index < fmt_ctx_->nb_streams);
            return fmt_ctx_->streams[stream_index]->codecpar->codec_type;
        }
        static inline auto stream2str(const enum AVMediaType codec_type) {
            switch (codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                return "VIDEO";
            case AVMEDIA_TYPE_AUDIO:
                return "AUDIO";
            case AVMEDIA_TYPE_UNKNOWN:
                return "UNKNOWN";
            case AVMEDIA_TYPE_DATA:
                return "DATA";
            case AVMEDIA_TYPE_SUBTITLE:
                return "SUBTITLE";
            case AVMEDIA_TYPE_ATTACHMENT:
                return "ATTACHMENT";
            case AVMEDIA_TYPE_NB:
                return "NB";
            default:
                break;
            }
            return "none";
        }

        static constexpr int64_t dts_delta_threshold_avtb_ = 10LL * AV_TIME_BASE;
        static constexpr int64_t dts_error_threshold_avtb_ = 30LL * 3600LL * AV_TIME_BASE;

        auto ts_fixup(AVPacket* packet, reference_t* ref, stream_state_t& state, const char* stream_typestr) -> void {
            assert(packet);
            assert(ref);
            assert(ref->stream);

            auto* stream = ref->stream;
            packet->time_base = stream->time_base;

            // Approximate wrap correction from fftools ts_fixup().
            if (!state.wrap_correction_done && fmt_ctx_->start_time != AV_NOPTS_VALUE && stream->pts_wrap_bits < 64) {
                const auto stime = av_rescale_q(fmt_ctx_->start_time, AV_TIME_BASE_Q, packet->time_base);
                const int64_t stime2 = stime + (1ULL << stream->pts_wrap_bits);
                state.wrap_correction_done = true;

                if (stime2 > stime && packet->dts != AV_NOPTS_VALUE &&
                    packet->dts > stime + (1LL << (stream->pts_wrap_bits - 1))) {
                    packet->dts -= (1ULL << stream->pts_wrap_bits);
                    state.wrap_correction_done = false;
                }
                if (stime2 > stime && packet->pts != AV_NOPTS_VALUE &&
                    packet->pts > stime + (1LL << (stream->pts_wrap_bits - 1))) {
                    packet->pts -= (1ULL << stream->pts_wrap_bits);
                    state.wrap_correction_done = false;
                }
            }

            // Apply global input timestamp offset (AV_TIME_BASE units), fftools-style.
            if (options_.ts_offset != 0) {
                const auto offset_tb = av_rescale_q(options_.ts_offset, AV_TIME_BASE_Q, packet->time_base);
                if (packet->dts != AV_NOPTS_VALUE)
                    packet->dts += offset_tb;
                if (packet->pts != AV_NOPTS_VALUE)
                    packet->pts += offset_tb;
            }

            // Apply per-stream timestamp scale, fftools-style.
            const auto ts_scale = (ref == &video_) ? options_.video_ts_scale : options_.audio_ts_scale;
            if (std::fabs(ts_scale - 1.0) > 1e-12) {
                if (packet->dts != AV_NOPTS_VALUE)
                    packet->dts = static_cast<int64_t>(std::llround(static_cast<double>(packet->dts) * ts_scale));
                if (packet->pts != AV_NOPTS_VALUE)
                    packet->pts = static_cast<int64_t>(std::llround(static_cast<double>(packet->pts) * ts_scale));
            }

            const auto pkt_dts_avtb_before_norm =
                packet->dts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE :
                av_rescale_q(packet->dts, packet->time_base, AV_TIME_BASE_Q);

            // copy_ts=false + start_at_zero=true: normalize all selected streams to one shared origin.
            if (!options_.copy_ts && options_.start_at_zero) {
                const auto base_avtb =
                    pkt_dts_avtb_before_norm != AV_NOPTS_VALUE ? pkt_dts_avtb_before_norm :
                    (packet->pts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE :
                     av_rescale_q(packet->pts, packet->time_base, AV_TIME_BASE_Q));
                if (start_at_zero_origin_avtb_ == AV_NOPTS_VALUE && base_avtb != AV_NOPTS_VALUE)
                    start_at_zero_origin_avtb_ = base_avtb;

                if (start_at_zero_origin_avtb_ != AV_NOPTS_VALUE) {
                    const auto norm_tb = av_rescale_q(start_at_zero_origin_avtb_, AV_TIME_BASE_Q, packet->time_base);
                    if (packet->dts != AV_NOPTS_VALUE)
                        packet->dts -= norm_tb;
                    if (packet->pts != AV_NOPTS_VALUE)
                        packet->pts -= norm_tb;
                }
            }

            // Recompute DTS in AV_TIME_BASE after all timestamp normalization steps above.
            const auto pkt_dts_avtb =
                packet->dts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE :
                av_rescale_q(packet->dts, packet->time_base, AV_TIME_BASE_Q);

            // Timestamp discontinuity handling (fftools-like split for discont/non-discont formats).
            if (state.next_dts_avtb != AV_NOPTS_VALUE && pkt_dts_avtb != AV_NOPTS_VALUE) {
                const auto delta = pkt_dts_avtb - state.next_dts_avtb;
                const bool fmt_is_discont = fmt_ctx_->iformat && ((fmt_ctx_->iformat->flags & AVFMT_TS_DISCONT) != 0);
                bool disable_discontinuity_correction = options_.copy_ts;

                // fftools: when copy_ts is enabled, keep discontinuity correction disabled
                // except a specific wrap-related case.
                if (options_.copy_ts && fmt_is_discont && stream->pts_wrap_bits < 60 &&
                    packet->dts != AV_NOPTS_VALUE) {
                    const auto wrap_dts_avtb =
                        av_rescale_q(packet->dts + (1LL << stream->pts_wrap_bits), packet->time_base, AV_TIME_BASE_Q);
                    if (std::llabs(wrap_dts_avtb - state.next_dts_avtb) <
                        std::llabs(pkt_dts_avtb - state.next_dts_avtb) / 10) {
                        disable_discontinuity_correction = false;
                    }
                }

                if (fmt_is_discont) {
                    if (!disable_discontinuity_correction &&
                        (std::llabs(delta) > dts_delta_threshold_avtb_ ||
                         pkt_dts_avtb + AV_TIME_BASE / 10 < state.dts_avtb)) {
                        logger_->log<logger_t::level_e::warn, "[{}:{}:{}] DTS discontinuity in stream {}: correcting by delta {}">(
                            __FILE__, __LINE__, __func__, stream_typestr, delta);

                        const auto delta_tb = av_rescale_q(delta, AV_TIME_BASE_Q, packet->time_base);
                        packet->dts -= delta_tb;
                        if (packet->pts != AV_NOPTS_VALUE)
                            packet->pts -= delta_tb;
                    }
                } else {
                    if (std::llabs(delta) > dts_error_threshold_avtb_) {
                        logger_->log<logger_t::level_e::warn, "[{}:{}:{}] invalid DTS in stream {} (delta {}), dropping DTS">(
                            __FILE__, __LINE__, __func__, stream_typestr, delta);
                        packet->dts = AV_NOPTS_VALUE;
                    }

                    if (packet->pts != AV_NOPTS_VALUE) {
                        const auto pkt_pts_avtb = av_rescale_q(packet->pts, packet->time_base, AV_TIME_BASE_Q);
                        const auto pts_delta = pkt_pts_avtb - state.next_dts_avtb;
                        if (std::llabs(pts_delta) > dts_error_threshold_avtb_) {
                            logger_->log<logger_t::level_e::warn, "[{}:{}:{}] invalid PTS in stream {} (delta {}), dropping PTS">(
                                __FILE__, __LINE__, __func__, stream_typestr, pts_delta);
                            packet->pts = AV_NOPTS_VALUE;
                        }
                    }
                }
            }

            const auto fixed_dts_avtb =
                packet->dts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE :
                av_rescale_q(packet->dts, packet->time_base, AV_TIME_BASE_Q);
            const auto fixed_pts_avtb =
                packet->pts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE :
                av_rescale_q(packet->pts, packet->time_base, AV_TIME_BASE_Q);

            // Keep dts/next_dts estimation close to fftools ist_dts_update().
            if (!state.saw_first_ts) {
                state.first_dts_avtb = 0;
                state.dts_avtb = 0;

                if (fixed_pts_avtb != AV_NOPTS_VALUE) {
                    state.first_dts_avtb = fixed_pts_avtb;
                    state.dts_avtb = fixed_pts_avtb;
                } else if (fixed_dts_avtb != AV_NOPTS_VALUE) {
                    state.first_dts_avtb = fixed_dts_avtb;
                    state.dts_avtb = fixed_dts_avtb;
                }

                state.saw_first_ts = true;
            }

            if (state.next_dts_avtb == AV_NOPTS_VALUE)
                state.next_dts_avtb = state.dts_avtb;

            if (fixed_dts_avtb != AV_NOPTS_VALUE)
                state.next_dts_avtb = state.dts_avtb = fixed_dts_avtb;

            state.dts_avtb = state.next_dts_avtb;

            switch (stream->codecpar->codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                if (stream->codecpar->sample_rate > 0 && stream->codecpar->frame_size > 0) {
                    state.next_dts_avtb +=
                        (static_cast<int64_t>(AV_TIME_BASE) * stream->codecpar->frame_size) /
                        stream->codecpar->sample_rate;
                } else if (packet->duration > 0) {
                    state.next_dts_avtb += av_rescale_q(packet->duration, packet->time_base, AV_TIME_BASE_Q);
                }
                break;
            case AVMEDIA_TYPE_VIDEO: {
                AVRational fr = stream->avg_frame_rate;
                if (fr.num <= 0 || fr.den <= 0)
                    fr = stream->r_frame_rate;

                if (fr.num > 0 && fr.den > 0) {
                    state.next_dts_avtb += av_rescale_q(1, av_inv_q(fr), AV_TIME_BASE_Q);
                } else if (packet->duration > 0) {
                    state.next_dts_avtb += av_rescale_q(packet->duration, packet->time_base, AV_TIME_BASE_Q);
                }
                break;
            }
            default:
                if (packet->duration > 0)
                    state.next_dts_avtb += av_rescale_q(packet->duration, packet->time_base, AV_TIME_BASE_Q);
                break;
            }
        }

        auto input_packet_process(AVPacket* packet, reference_t* ref, stream_state_t& state, const char* stream_typestr) -> bool {
            assert(packet);
            assert(ref);
            assert(ref->stream);

            ts_fixup(packet, ref, state, stream_typestr);

            if (packet->duration < 0) {
                logger_->log<logger_t::level_e::warn, "[{}:{}:{}] negative packet duration in stream {}, forcing to 0">(
                    __FILE__, __LINE__, __func__, stream_typestr);
                packet->duration = 0;
            }

            return true;
        }

    public:

        explicit demuxer_t(const std::string& filename, logger_t::ptr_t logger)
            : demuxer_t(filename, options_t{}, logger)
        {}

        explicit demuxer_t(const std::string& filename, const options_t& options, logger_t::ptr_t logger)
            : logger_(logger->clone())
            , filename_(filename)
            , options_(options)
            , fmt_ctx_(nullptr) {

            auto sanitize_ts_scale = [&](double& ts_scale, const char* name) {
                if (!std::isfinite(ts_scale) || ts_scale <= 0.0) {
                    logger_->log<logger_t::level_e::warn, "[{}:{}:{}] invalid {} {}, fallback to 1.0">(
                        __FILE__, __LINE__, __func__, name, ts_scale);
                    ts_scale = 1.0;
                }
            };
            sanitize_ts_scale(options_.video_ts_scale, "video_ts_scale");
            sanitize_ts_scale(options_.audio_ts_scale, "audio_ts_scale");

            AVFormatContext* ptr = nullptr;
            if (auto ret = avformat_open_input(&ptr, filename_.data(), nullptr, nullptr); ret >= 0) {
                assert(ptr);
                fmt_ctx_.reset(ptr);

                if (ret = avformat_find_stream_info(fmt_ctx_.get(), nullptr); ret < 0) {
                    logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! FAILED avformat_find_stream_info for file `{}`: {} !!!">(
                        __FILE__, __LINE__, __func__, filename_, ret);
                    fmt_ctx_.reset();
                    return;
                }

                auto init_best_ref = [&](const AVMediaType media_type, reference_t& ref) {
                    const auto stream_index = av_find_best_stream(fmt_ctx_.get(), media_type, -1, -1, nullptr, 0);
                    if (stream_index < 0) {
                        logger_->log<logger_t::level_e::warn, "[{}:{}:{}] best {} stream is not found: {}">(
                            __FILE__, __LINE__, __func__, stream2str(media_type), stream_index);
                        return;
                    }

                    assert(static_cast<unsigned int>(stream_index) < fmt_ctx_->nb_streams);
                    auto* stream = fmt_ctx_->streams[stream_index];
                    ref.stream = stream;
                    ref.codec = avcodec_find_decoder(stream->codecpar->codec_id);
                    ref.codecpar = stream->codecpar;
                    ref.time_base = stream->time_base;

                    if (!ref.codec) {
                        logger_->log<logger_t::level_e::warn, "[{}:{}:{}] decoder is not found for best {} stream index {} codec_id {}">(
                            __FILE__, __LINE__, __func__, stream2str(media_type), stream_index, static_cast<int>(stream->codecpar->codec_id));
                    }
                };

                init_best_ref(AVMEDIA_TYPE_VIDEO, video_);
                init_best_ref(AVMEDIA_TYPE_AUDIO, audio_);

            } else {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! FAILED avformat_open_input for file `{}`: {} !!!">(
                    __FILE__, __LINE__, __func__, filename_, ret);
            }

        }

        ~demuxer_t() = default;

        auto video_reference() const -> const reference_t& {
            return video_;
        }
        auto audio_reference() const -> const reference_t& {
            return audio_;
        }

        explicit operator bool() const {
            return fmt_ctx_.get() && (video_ || audio_);
        }

        auto packet_type(AVPacket* packet) const {
            assert(packet);
            return codec_type(packet->stream_index);
        }

        coro::produce::generator_t<packet_uptr>
        retrieve() {

            assert(fmt_ctx_);

            while (true) {

                if (auto packet = make_packet(); packet) {

                    auto ret = av_read_frame(fmt_ctx_.get(), packet.get());

                    if (ret == AVERROR(EAGAIN)) {
                        logger_->log<logger_t::level_e::warn, "[{}:{}:{}] av_read_frame AVERROR(EAGAIN)">(
                            __FILE__, __LINE__, __func__);

                        continue;
                    }
                    if (ret < 0) {
                        if (ret == AVERROR_EOF) {
                            logger_->log<logger_t::level_e::trace, "[{}:{}:{}] EOF while reading input">(
                                __FILE__, __LINE__, __func__);
                        } else {
                            logger_->log<logger_t::level_e::err, "[{}:{}:{}] !!! Error during demuxing: {} !!!">(
                                __FILE__, __LINE__, __func__, ret);
                        }

                        co_return;
                    }

                    // On success, the returned packet is reference-counted (pkt->buf is set) and valid indefinitely.
                    // The packet must be freed with av_packet_unref() when it is no longer needed.

                    // For video, the packet contains exactly one frame.
                    // For audio, the packet contains an integer number of frames if each frame has a known fixed size (e.g. PCM or ADPCM data).
                    //     If the audio frames have a variable size (e.g. MPEG audio), then the packet contains one frame.

                    // pkt->pts, pkt->dts and pkt->duration are always set to correct values in AVStream.time_base units (and guessed if the format cannot provide them).
                    // pkt->pts can be AV_NOPTS_VALUE if the video format has B-frames, so it is better to rely on pkt->dts if you do not decompress the payload.

                    if (packet->stream_index < 0 || static_cast<unsigned int>(packet->stream_index) >= fmt_ctx_->nb_streams) {
                        logger_->log<logger_t::level_e::warn, "[{}:{}:{}] packet has invalid stream index {}, skipping">(
                            __FILE__, __LINE__, __func__, packet->stream_index);
                        continue;
                    }

                    const auto& stream_type = codec_type(static_cast<unsigned int>(packet->stream_index));
                    const auto stream_typestr = stream2str(stream_type);

                    if (packet->flags & AV_PKT_FLAG_CORRUPT) {
                        logger_->log<logger_t::level_e::warn, "[{}:{}:{}] av_read_frame gets corrupted input packet in stream {}">(
                            __FILE__, __LINE__, __func__, stream_typestr);

                        continue;
                    }

                    auto ref = get_reference(stream_type);
                    if (!ref || !ref->stream) {
                        continue;
                    }
                    if (packet->stream_index != ref->stream->index) {
                        logger_->log<logger_t::level_e::trace, "[{}:{}:{}] skip non-selected stream packet index {} (selected {} for {})">(
                            __FILE__, __LINE__, __func__, packet->stream_index, ref->stream->index, stream_typestr);
                        continue;
                    }
                    auto& state = (ref == &video_) ? video_state_ : audio_state_;
                    if (!input_packet_process(packet.get(), ref, state, stream_typestr)) {
                        continue;
                    }

                    logger_->log<logger_t::level_e::trace, "[{}:{}:{}] av_read_frame gets packet size {} in stream {}">(
                        __FILE__, __LINE__, __func__, packet->size, stream_typestr);

                    co_yield std::move(packet);

                } else
                    co_return;
            }
        }
    };

    auto stream_video(demuxer_t& demuxer) {
        return [&demuxer](auto stream) -> coro::produce::generator_t<packet_uptr> {
            while (stream) {
                auto p = stream();
                if (!p) continue;
                if (demuxer.packet_type(p.get()) != AVMEDIA_TYPE_VIDEO) continue;
                co_yield std::move(p);
            }
        };
    }

    auto stream_audio(demuxer_t& demuxer) {
        return [&demuxer](auto stream) -> coro::produce::generator_t<packet_uptr> {
            while (stream) {
                auto p = stream();
                if (!p) continue;
                if (demuxer.packet_type(p.get()) != AVMEDIA_TYPE_AUDIO) continue;
                co_yield std::move(p);
            }
        };
    }

}
