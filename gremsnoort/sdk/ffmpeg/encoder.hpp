#pragma once

// std
#include <climits>

// gsdk
#include <gremsnoort/sdk/forward/coro.hpp>
#include <gremsnoort/sdk/ffmpeg/frame_metadata.hpp>
#include <gremsnoort/sdk/ffmpeg/smartptr.h>
#include <gremsnoort/sdk/logger/logger.h>

// conan

#ifdef __cplusplus
extern "C" {
#endif

#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>

#ifdef __cplusplus
}
#endif

namespace gremsnoort::sdk::ffmpeg {

    class encoder_t {

        logger_t::ptr_t logger_;

        const AVCodec* codec_;
        codec_ctx_uptr enc_ctx_;
        bool opened_ = false;
        bool failed_ = false;
        AVRational source_framerate_ = AVRational{ .num = 0, .den = 1 };
        AVMediaType media_type_ = AVMEDIA_TYPE_UNKNOWN;

        auto init_from_reference(const reference_t& ref) {
            if (!codec_ || !enc_ctx_) {
                failed_ = true;
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! FAILED to INIT encoder context !!!">(
                    __FILE__, __LINE__, __func__);
                return;
            }
            if (!ref || !ref.codecpar) {
                failed_ = true;
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! FAILED to init from NULL reference !!!">(
                    __FILE__, __LINE__, __func__);
                enc_ctx_.reset();
                return;
            }

            media_type_ = ref.codecpar->codec_type;

            if (ref.time_base.num > 0 && ref.time_base.den > 0) {
                enc_ctx_->time_base = ref.time_base;
            } else {
                enc_ctx_->time_base = AVRational{ .num = 1, .den = 1 };
            }

            if (media_type_ == AVMEDIA_TYPE_VIDEO) {
                if (ref.stream) {
                    if (ref.stream->avg_frame_rate.num > 0 && ref.stream->avg_frame_rate.den > 0) {
                        source_framerate_ = ref.stream->avg_frame_rate;
                    } else if (ref.stream->r_frame_rate.num > 0 && ref.stream->r_frame_rate.den > 0) {
                        source_framerate_ = ref.stream->r_frame_rate;
                    }
                }
            }
        }

        auto open_if_needed(frame_uptr::pointer frame) -> bool {
            if (opened_)
                return true;

            if (!frame) {
                logger_->log<logger_t::level_e::trace, "[{}:{}:{}] flush requested before encoder open; nothing to flush">(
                    __FILE__, __LINE__, __func__);
                return false;
            }

            if (frame->time_base.num > 0 && frame->time_base.den > 0) {
                // fftools: encoder time_base is defined by filtered input frame.
                enc_ctx_->time_base = frame->time_base;
            } else {
                logger_->log<logger_t::level_e::warn, "[{}:{}:{}] invalid frame time_base {}/{}; using preconfigured encoder time_base {}/{}">(
                    __FILE__, __LINE__, __func__,
                    frame->time_base.num, frame->time_base.den,
                    enc_ctx_->time_base.num, enc_ctx_->time_base.den);
            }

            // fftools: video encoder framerate is initialized from explicit frame metadata.
            if (media_type_ == AVMEDIA_TYPE_VIDEO) {
                if (frame->format == AV_PIX_FMT_NONE || frame->width <= 0 || frame->height <= 0) {
                    failed_ = true;
                    logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! invalid input video frame params for encoder open: fmt={} w={} h={} !!!">(
                        __FILE__, __LINE__, __func__, frame->format, frame->width, frame->height);
                    return false;
                }

                enc_ctx_->pix_fmt = static_cast<AVPixelFormat>(frame->format);
                enc_ctx_->width = frame->width;
                enc_ctx_->height = frame->height;
                if (frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0) {
                    enc_ctx_->sample_aspect_ratio = frame->sample_aspect_ratio;
                }

                AVRational fr = AVRational{ .num = 0, .den = 1 };
                if (frame->opaque_ref && frame->opaque_ref->size >= static_cast<int>(sizeof(frame_metadata_t))) {
                    const auto* meta = reinterpret_cast<const frame_metadata_t*>(frame->opaque_ref->data);
                    if (meta->magic == frame_metadata_t::magic_v) {
                        fr = meta->frame_rate_filter;
                    }
                }

                if (fr.num <= 0 || fr.den <= 0) {
                    fr = source_framerate_;
                }

                if (fr.num > 0 && fr.den > 0) {
                    enc_ctx_->framerate = fr;
                } else {
                    logger_->log<logger_t::level_e::warn, "[{}:{}:{}] source framerate metadata is unavailable; encoder framerate remains unset">(
                        __FILE__, __LINE__, __func__);
                }
            } else if (media_type_ == AVMEDIA_TYPE_AUDIO) {
                if (frame->format == AV_SAMPLE_FMT_NONE || frame->sample_rate <= 0 || frame->ch_layout.nb_channels <= 0) {
                    failed_ = true;
                    logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! invalid input audio frame params for encoder open: fmt={} sample_rate={} channels={} !!!">(
                        __FILE__, __LINE__, __func__, frame->format, frame->sample_rate, frame->ch_layout.nb_channels);
                    return false;
                }

                enc_ctx_->sample_fmt = static_cast<AVSampleFormat>(frame->format);
                enc_ctx_->sample_rate = frame->sample_rate;
                if (const auto bps = av_get_bytes_per_sample(enc_ctx_->sample_fmt); bps > 0) {
                    enc_ctx_->bits_per_raw_sample = bps << 3;
                }
                av_channel_layout_uninit(&enc_ctx_->ch_layout);
                if (auto ret = av_channel_layout_copy(&enc_ctx_->ch_layout, &frame->ch_layout); ret < 0) {
                    failed_ = true;
                    logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! failed to copy input audio channel layout: {} !!!">(
                        __FILE__, __LINE__, __func__, ret);
                    return false;
                }
            }

            if (auto ret = avcodec_open2(enc_ctx_.get(), codec_, nullptr); ret < 0) {
                failed_ = true;
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! FAILED avcodec_open2: {} !!!">(
                    __FILE__, __LINE__, __func__, ret);
                enc_ctx_.reset();
                return false;
            }

            opened_ = true;
            return true;
        }

        auto frame_compatible_with_opened_encoder(const AVFrame* frame) const -> bool {
            assert(frame);
            assert(enc_ctx_);

            if (media_type_ == AVMEDIA_TYPE_VIDEO) {
                const auto frame_fmt = static_cast<AVPixelFormat>(frame->format);
                return frame->width == enc_ctx_->width &&
                       frame->height == enc_ctx_->height &&
                       frame_fmt == enc_ctx_->pix_fmt;
            }

            if (media_type_ == AVMEDIA_TYPE_AUDIO) {
                const auto frame_fmt = static_cast<AVSampleFormat>(frame->format);
                if (frame->sample_rate != enc_ctx_->sample_rate || frame_fmt != enc_ctx_->sample_fmt)
                    return false;
                const auto layout_cmp = av_channel_layout_compare(&frame->ch_layout, &enc_ctx_->ch_layout);
                return layout_cmp == 0;
            }

            return true;
        }

    public:

        explicit encoder_t(const reference_t& reference, const AVCodecID codec_id, logger_t::ptr_t logger)
            : logger_(logger->clone())
            , codec_(avcodec_find_encoder(codec_id))
            , enc_ctx_(codec_ ? make_codec_ctx(codec_) : nullptr) {
            init_from_reference(reference);
            // avcodec_parameters_from_context() // @TODO !!!
        }

        explicit encoder_t(const reference_t& reference, const std::string& codec_name, logger_t::ptr_t logger)
            : logger_(logger->clone())
            , codec_(avcodec_find_encoder_by_name(codec_name.data()))
            , enc_ctx_(codec_ ? make_codec_ctx(codec_) : nullptr) {
            init_from_reference(reference);
            // avcodec_parameters_from_context() // @TODO !!!
        }

        ~encoder_t() = default;

        explicit operator bool() const noexcept {
            return enc_ctx_.get();
        }

        auto context() const -> AVCodecContext* {
            return enc_ctx_.get();
        }

        auto failed() const noexcept -> bool {
            return failed_;
        }

        coro::produce::generator_t<packet_uptr>
        encode(frame_uptr frame) {

            if (!enc_ctx_) {
                failed_ = true;
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! encoder context is not initialized !!!">(
                    __FILE__, __LINE__, __func__);
                co_return;
            }
            if (!frame && !opened_) {
                logger_->log<logger_t::level_e::trace, "[{}:{}:{}] flush requested before first frame; nothing to flush">(
                    __FILE__, __LINE__, __func__);
                co_return;
            }
            if (!open_if_needed(frame.get())) {
                if (frame)
                    failed_ = true;
                co_return;
            }

            if (frame && opened_ && !frame_compatible_with_opened_encoder(frame.get())) {
                failed_ = true;
                if (media_type_ == AVMEDIA_TYPE_VIDEO) {
                    logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! input video frame params changed after encoder open: "
                                    "frame(w={},h={},fmt={}) != enc(w={},h={},fmt={}) !!!">(
                        __FILE__, __LINE__, __func__,
                        frame->width, frame->height, frame->format,
                        enc_ctx_->width, enc_ctx_->height, static_cast<int>(enc_ctx_->pix_fmt));
                } else if (media_type_ == AVMEDIA_TYPE_AUDIO) {
                    logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! input audio frame params changed after encoder open: "
                                    "frame(sr={},fmt={},ch={}) != enc(sr={},fmt={},ch={}) !!!">(
                        __FILE__, __LINE__, __func__,
                        frame->sample_rate, frame->format, frame->ch_layout.nb_channels,
                        enc_ctx_->sample_rate, static_cast<int>(enc_ctx_->sample_fmt), enc_ctx_->ch_layout.nb_channels);
                } else {
                    logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! input frame params changed after encoder open !!!">(
                        __FILE__, __LINE__, __func__);
                }
                co_return;
            }

            auto ret = int{AVERROR(EAGAIN)};
            auto frame_sent = false;

            while(true) {
                if (!frame_sent) {
                    ret = avcodec_send_frame(enc_ctx_.get(), frame.get());

                    switch(ret) {
                    case AVERROR(EAGAIN): {
                        logger_->log<logger_t::level_e::trace, "[{}:{}:{}] EAGAIN on avcodec_send_frame: "
                                        "draining encoder output before retrying the same frame">(
                            __FILE__, __LINE__, __func__);
                        break;
                    }
                    case AVERROR(EINVAL): {
                        failed_ = true;
                        logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! EINVAL on avcodec_send_frame: "
                                        "codec not opened, it is a decoder, or requires flush !!!">(
                            __FILE__, __LINE__, __func__);

                        co_return;
                    }
                    case AVERROR(ENOMEM): {
                        failed_ = true;
                        logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! ENOMEM on avcodec_send_frame: "
                                        "failed to add packet to internal queue, or similar !!!">(
                            __FILE__, __LINE__, __func__);

                        co_return;
                    }
                    case AVERROR_EOF: {
                        if (!frame) {
                            logger_->log<logger_t::level_e::trace, "[{}:{}:{}] AVERROR_EOF on avcodec_send_frame during flush: "
                                            "encoder is already fully flushed">(
                                __FILE__, __LINE__, __func__);
                        } else {
                            failed_ = true;
                            logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! AVERROR_EOF on avcodec_send_frame: "
                                            "the encoder has been flushed, and no new frames can be sent to it !!!">(
                                __FILE__, __LINE__, __func__);
                        }

                        co_return;
                    }
                    default:
                        break;
                    }

                    if (ret < 0 && ret != AVERROR(EAGAIN)) {
                        failed_ = true;
                        logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! FAILED to avcodec_send_frame: {} !!!">(
                            __FILE__, __LINE__, __func__, ret);

                        co_return;
                    }

                    if (ret == 0)
                        frame_sent = true;
                }

                if (auto packet = make_packet(); packet) {

                    ret = avcodec_receive_packet(enc_ctx_.get(), packet.get());

                    switch(ret) {
                    case AVERROR(EAGAIN): {
                        logger_->log<logger_t::level_e::trace, "[{}:{}:{}] EAGAIN on avcodec_receive_packet: "
                                        "output is not available in this state - user must try to send new input">(
                            __FILE__, __LINE__, __func__);

                        if (frame_sent)
                            co_return;
                        continue;
                    }
                    case AVERROR(EINVAL): {
                        failed_ = true;
                        logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! EINVAL on avcodec_receive_packet: "
                                        "codec not opened, or it is a decoder !!!">(
                            __FILE__, __LINE__, __func__);

                        co_return;
                    }
                    case AVERROR_EOF: {
                        logger_->log<logger_t::level_e::trace, "[{}:{}:{}] AVERROR_EOF on avcodec_receive_packet: "
                                        "the encoder has been fully flushed, and there will be no more output packets">(
                            __FILE__, __LINE__, __func__);

                        co_return;
                    }
                    default:
                        break;
                    }

                    if (ret < 0) {
                        failed_ = true;
                        logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! FAILED to avcodec_receive_packet: {} !!!">(
                            __FILE__, __LINE__, __func__, ret);

                        co_return;
                    }

                    assert(ret == 0);
                    assert(packet);

                    packet->time_base = enc_ctx_->time_base;

                    co_yield std::move(packet);

                } else
                    co_return;
            }
        }
    };

    auto encode_with(encoder_t& encoder) {
        // std::printf("[%s] start with &encoder %p\n", __func__, (void*)&encoder);
        return [&encoder](auto frames) -> coro::produce::generator_t<packet_uptr> {
            // std::printf("start with &encoder %p\n", (void*)&encoder);
            while (frames) {
                // std::printf("iter with &encoder %p\n", (void*)&encoder);
                auto f = frames();
                if (!f) continue;

                auto packets = encoder.encode(std::move(f));
                while (packets) {
                    auto p = packets();
                    if (p) co_yield std::move(p);
                }
            }
        };
    }

    auto flush_encoder(encoder_t& encoder) -> coro::produce::generator_t<packet_uptr> {
        auto packets = encoder.encode(nullptr); // EOF/drain
        while (packets) {
            auto p = packets();
            if (p) co_yield std::move(p);
        }
    }

}
