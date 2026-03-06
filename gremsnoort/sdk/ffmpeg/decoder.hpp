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
#include <libavutil/buffer.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>

#ifdef __cplusplus
}
#endif

namespace gremsnoort::sdk::ffmpeg {

    class decoder_t {

        logger_t::ptr_t logger_;

        codec_ctx_uptr dec_ctx_;

        // pts/estimated duration of the last decoded frame
        // * in decoder timebase for video,
        // * in last_frame_tb (may change during decoding) for audio
        int64_t             last_frame_pts = AV_NOPTS_VALUE;
        int64_t             last_frame_duration_est = 0;
        AVRational          last_frame_tb = AVRational{ .num = 1, .den = 1 };
        int64_t             last_filter_in_rescale_delta = AV_NOPTS_VALUE;
        int                 last_frame_sample_rate = 0;
        AVRational          framerate_in = AVRational{ .num = 0, .den = 1 };

        auto audio_samplerate_update(const AVFrame* frame) -> AVRational {
            assert(frame);

            const int sr = frame->sample_rate;
            if (sr <= 0) {
                return last_frame_tb;
            }

            if (sr == last_frame_sample_rate)
                return last_frame_tb;

            const int prev = last_frame_tb.den > 0 ? last_frame_tb.den : 1;
            const int64_t gcd = av_gcd(prev, sr);
            AVRational tb_new;

            if (prev / gcd >= INT_MAX / sr) {
                logger_->log<logger_t::level_e::warn, "[{}:{}:{}] audio timebase cannot represent samplerate switch exactly: {} -> {}, fallback to 1/28224000">(
                    __FILE__, __LINE__, __func__, prev, sr);
                // LCM of 192000 and 44100: can represent common sample rates.
                tb_new = AVRational{ .num = 1, .den = 28224000 };
            } else {
                tb_new = AVRational{ .num = 1, .den = static_cast<int>(prev / gcd * sr) };
            }

            // Keep frame timebase if it is strictly better and compatible.
            if (frame->time_base.num == 1 && frame->time_base.den > tb_new.den &&
                (frame->time_base.den % tb_new.den) == 0) {
                tb_new = frame->time_base;
            }

            if (last_frame_pts != AV_NOPTS_VALUE) {
                last_frame_pts = av_rescale_q(last_frame_pts, last_frame_tb, tb_new);
            }
            last_frame_duration_est = av_rescale_q(last_frame_duration_est, last_frame_tb, tb_new);

            last_frame_tb = tb_new;
            last_frame_sample_rate = sr;
            return last_frame_tb;
        }

        auto audio_ts_process(AVFrame* frame) -> void {
            assert(frame);

            if (frame->sample_rate <= 0 || frame->nb_samples < 0) {
                logger_->log<logger_t::level_e::warn, "[{}:{}:{}] invalid audio frame timestamps input: sample_rate={} nb_samples={}">(
                    __FILE__, __LINE__, __func__, frame->sample_rate, frame->nb_samples);
                return;
            }

            const AVRational tb_filter = AVRational{ .num = 1, .den = frame->sample_rate };
            const AVRational tb = audio_samplerate_update(frame);

            const int64_t pts_pred =
                last_frame_pts == AV_NOPTS_VALUE ? 0 :
                last_frame_pts + last_frame_duration_est;

            if (frame->pts == AV_NOPTS_VALUE) {
                frame->pts = pts_pred;
                frame->time_base = tb;
            } else if (last_frame_pts != AV_NOPTS_VALUE &&
                       frame->pts > av_rescale_q_rnd(pts_pred, tb, frame->time_base, AV_ROUND_UP)) {
                // Gap in timestamps: reset conversion state.
                last_filter_in_rescale_delta = AV_NOPTS_VALUE;
            }

            frame->pts = av_rescale_delta(frame->time_base, frame->pts, tb, frame->nb_samples,
                                          &last_filter_in_rescale_delta, tb);

            last_frame_pts = frame->pts;
            last_frame_duration_est = av_rescale_q(frame->nb_samples, tb_filter, tb);

            // Convert to filtering timebase.
            frame->pts = av_rescale_q(frame->pts, tb, tb_filter);
            frame->duration = frame->nb_samples;
            frame->time_base = tb_filter;
        }

        auto video_duration_estimate(const AVFrame *frame) -> int64_t {
            ///const int  ts_unreliable = dp->flags & DECODER_FLAG_TS_UNRELIABLE;
            ///const int      fr_forced = dp->flags & DECODER_FLAG_FRAMERATE_FORCED;
            int64_t codec_duration = 0;

            // XXX lavf currently makes up frame durations when they are not provided by
            // the container. As there is no way to reliably distinguish real container
            // durations from the fake made-up ones, we use heuristics based on whether
            // the container has timestamps. Eventually lavf should stop making up
            // durations, then this should be simplified.

            // prefer frame duration for containers with timestamps
            if (frame->duration > 0 /*&& (!ts_unreliable || fr_forced)*/)
                return frame->duration;

            if (dec_ctx_->framerate.den && dec_ctx_->framerate.num) {
                int fields = frame->repeat_pict + 2;
                auto field_rate = av_mul_q(dec_ctx_->framerate, AVRational{ .num=2, .den=1 });
                codec_duration = av_rescale_q(fields, av_inv_q(field_rate), frame->time_base);
            }

            // prefer codec-layer duration for containers without timestamps
            /// if (codec_duration > 0 && ts_unreliable)
            ///     return codec_duration;

            // when timestamps are available, repeat last frame's actual duration
            // (i.e. pts difference between this and last frame)
            if (frame->pts != AV_NOPTS_VALUE && last_frame_pts != AV_NOPTS_VALUE &&
                frame->pts > last_frame_pts)
                return frame->pts - last_frame_pts;

            // try frame/codec duration
            if (frame->duration > 0)
                return frame->duration;
            if (codec_duration > 0)
                return codec_duration;

            // try average framerate
            if (framerate_in.num && framerate_in.den) {
                int64_t d = av_rescale_q(1, av_inv_q(framerate_in), frame->time_base);
                if (d > 0)
                    return d;
            }

            // last resort is last frame's estimated duration, and 1
            return FFMAX(last_frame_duration_est, 1);
        }

        auto attach_video_frame_metadata(AVFrame* frame) -> void {
            assert(frame);

            if (frame->opaque_ref) {
                return;
            }

            AVRational fr = AVRational{ .num = 0, .den = 1 };
            if (dec_ctx_->framerate.num > 0 && dec_ctx_->framerate.den > 0) {
                fr = dec_ctx_->framerate;
            } else if (framerate_in.num > 0 && framerate_in.den > 0) {
                fr = framerate_in;
            }

            if (fr.num <= 0 || fr.den <= 0) {
                return;
            }

            auto* ref = av_buffer_allocz(sizeof(frame_metadata_t));
            if (!ref) {
                logger_->log<logger_t::level_e::warn, "[{}:{}:{}] failed to allocate frame metadata buffer">(
                    __FILE__, __LINE__, __func__);
                return;
            }

            auto* meta = reinterpret_cast<frame_metadata_t*>(ref->data);
            meta->magic = frame_metadata_t::magic_v;
            meta->frame_rate_filter = fr;
            frame->opaque_ref = ref;
        }

        auto frame_process(frame_uptr::pointer frame) {

            if (dec_ctx_->codec_type == AVMEDIA_TYPE_AUDIO) {
                audio_ts_process(frame);

            } else if (dec_ctx_->codec_type == AVMEDIA_TYPE_VIDEO) {

                ///if (frame->format == dp->hwaccel_pix_fmt) {
                ///    int err = hwaccel_retrieve_data(dec_ctx_, frame);
                ///    if (err < 0)
                ///        return err;
                ///}

                frame->pts = frame->best_effort_timestamp;

                /// // forced fixed framerate
                /// if (dp->flags & DECODER_FLAG_FRAMERATE_FORCED) {
                ///     frame->pts       = AV_NOPTS_VALUE;
                ///     frame->duration  = 1;
                ///     frame->time_base = av_inv_q(framerate_in);
                /// }

                // no timestamp available - extrapolate from previous frame duration
                if (frame->pts == AV_NOPTS_VALUE)
                    frame->pts =
                        last_frame_pts == AV_NOPTS_VALUE ? 0 :
                                     last_frame_pts + last_frame_duration_est;

                // update timestamp history
                last_frame_duration_est = video_duration_estimate(frame);
                last_frame_pts          = frame->pts;
                last_frame_tb           = frame->time_base;

                attach_video_frame_metadata(frame);

                /// if (debug_ts) {
                ///     av_log(dp, AV_LOG_INFO,
                ///            "decoder -> pts:%s pts_time:%s "
                ///            "pkt_dts:%s pkt_dts_time:%s "
                ///            "duration:%s duration_time:%s "
                ///            "keyframe:%d frame_type:%d time_base:%d/%d\n",
                ///            av_ts2str(frame->pts),
                ///            av_ts2timestr(frame->pts, &frame->time_base),
                ///            av_ts2str(frame->pkt_dts),
                ///            av_ts2timestr(frame->pkt_dts, &frame->time_base),
                ///            av_ts2str(frame->duration),
                ///            av_ts2timestr(frame->duration, &frame->time_base),
                ///            !!(frame->flags & AV_FRAME_FLAG_KEY), frame->pict_type,
                ///            frame->time_base.num, frame->time_base.den);
                /// }

                return 0;
            }

            return 0;
        }

        auto init_from_reference(const reference_t& ref) {
            if (!ref) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! FAILED to init from NULL reference !!!">(
                    __FILE__, __LINE__, __func__);
                return;
            }

            if (dec_ctx_ = make_codec_ctx(ref.codec); !dec_ctx_) {
                return;
            }
            assert(dec_ctx_);

            if (const auto* codecpar = ref.codecpar; codecpar) {
                if (auto ret = avcodec_parameters_to_context(dec_ctx_.get(), codecpar); ret < 0) {
                    logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! FAILED avcodec_parameters_to_context: {} !!!">(
                        __FILE__, __LINE__, __func__, ret);
                    dec_ctx_.reset();
                    return;
                }

                if (ref.time_base.num > 0 && ref.time_base.den > 0) {
                    dec_ctx_->pkt_timebase = ref.time_base;
                } else {
                    logger_->log<logger_t::level_e::warn, "[{}:{}:{}] invalid source time_base {}/{}; fallback to 1/1">(
                        __FILE__, __LINE__, __func__, ref.time_base.num, ref.time_base.den);
                    dec_ctx_->pkt_timebase = AVRational{ .num = 1, .den = 1 };
                }

                if (ref.stream) {
                    framerate_in = ref.stream->avg_frame_rate;
                }

                dec_ctx_->codec_id = ref.codec->id;

                if (auto ret = avcodec_open2(dec_ctx_.get(), nullptr, nullptr); ret < 0) {
                    logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! FAILED avcodec_open2: {} !!!">(
                        __FILE__, __LINE__, __func__, ret);
                    dec_ctx_.reset();
                }
            } else {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! FAILED to init from NULL codecpar !!!">(
                    __FILE__, __LINE__, __func__);
            }
        }

    public:

        explicit decoder_t(const reference_t& reference, logger_t::ptr_t logger)
            : logger_(logger->clone())
            , dec_ctx_(nullptr) {
            init_from_reference(reference);
        }

        ~decoder_t() = default;

        explicit operator bool() const noexcept {
            return dec_ctx_.get();
        }

        coro::produce::generator_t<frame_uptr>
        decode(packet_uptr packet) {

            assert(dec_ctx_);

            // packet != nullptr: regular decode step.
            // packet == nullptr: EOF flush/drain step.
            // For delayed codecs (e.g. with B-frames), call with nullptr and
            // keep receiving until AVERROR_EOF to get all tail frames.

            // static int packet_decode(DecoderPriv *dp, AVPacket *pkt, AVFrame *frame)

            if (packet && packet->time_base.num > 0 && packet->time_base.den > 0) {
                dec_ctx_->pkt_timebase = packet->time_base;
            }

            // fftools: skip zero-sized packets (edge-case) and don't treat them as EOF/flush.
            if (packet && packet->size == 0) {
                logger_->log<logger_t::level_e::trace, "[{}:{}:{}] skip zero-sized packet">(
                    __FILE__, __LINE__, __func__);
                co_return;
            }

            auto ret = avcodec_send_packet(dec_ctx_.get(), packet.get());

            switch(ret) {
            case AVERROR(EAGAIN): {
                // we don't expect AVERROR(EAGAIN), because we read all decoded frames with avcodec_receive_frame() until done
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! EAGAIN on avcodec_send_packet: "
                                "input is not accepted in the current state - user must read output with avcodec_receive_frame() "
                                "(once all output is read, the packet should be resent, and the call will not fail with EAGAIN) !!!">(
                    __FILE__, __LINE__, __func__);

                co_return;
            }
            case AVERROR(EINVAL): {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! EINVAL on avcodec_send_packet: "
                                "codec not opened, it is an encoder, or requires flush !!!">(
                    __FILE__, __LINE__, __func__);

                co_return;
            }
            case AVERROR(ENOMEM): {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! ENOMEM on avcodec_send_packet: "
                                "failed to add packet to internal queue, or similar !!!">(
                    __FILE__, __LINE__, __func__);

                co_return;
            }
            case AVERROR_EOF: {
                if (!packet) {
                    logger_->log<logger_t::level_e::trace, "[{}:{}:{}] AVERROR_EOF on avcodec_send_packet during flush: "
                                    "decoder is already fully flushed">(
                        __FILE__, __LINE__, __func__);
                } else {
                    logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! AVERROR_EOF on avcodec_send_packet: "
                                    "the decoder has been flushed, and no new packets can be sent to it !!!">(
                        __FILE__, __LINE__, __func__);
                }

                co_return;
            }
            default:
                break;
            }

            if (ret < 0) {
                logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! FAILED to avcodec_send_packet: {} !!!">(
                    __FILE__, __LINE__, __func__, ret);

                co_return;
            }

            assert(ret == 0);

            while(true) {

                if (auto frame = make_frame(); frame) {

                    ret = avcodec_receive_frame(dec_ctx_.get(), frame.get());

                    switch(ret) {
                    case AVERROR(EAGAIN): {
                        logger_->log<logger_t::level_e::trace, "[{}:{}:{}] EAGAIN on avcodec_receive_frame: "
                                        "output is not available in this state - user must try to send new input">(
                            __FILE__, __LINE__, __func__);

                        co_return;
                    }
                    case AVERROR(EINVAL): {
                        logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! EINVAL on avcodec_receive_frame: "
                                        "codec not opened, or it is an encoder without the AV_CODEC_FLAG_RECON_FRAME flag enabled !!!">(
                            __FILE__, __LINE__, __func__);

                        co_return;
                    }
                    case AVERROR_EOF: {
                        logger_->log<logger_t::level_e::trace, "[{}:{}:{}] AVERROR_EOF on avcodec_receive_frame: "
                                        "the codec has been fully flushed, and there will be no more output frames">(
                            __FILE__, __LINE__, __func__);

                        co_return;
                    }
                    default:
                        break;
                    }

                    if (ret < 0) {
                        logger_->log<logger_t::level_e::critical, "[{}:{}:{}] !!! FAILED to avcodec_receive_frame: {} !!!">(
                            __FILE__, __LINE__, __func__, ret);

                        co_return;
                    }

                    // success, a frame was returned

                    assert(ret == 0);
                    assert(frame);

                    if (frame->decode_error_flags || (frame->flags & AV_FRAME_FLAG_CORRUPT)) {
                        logger_->log<logger_t::level_e::warn, "[{}:{}:{}] avcodec_receive_frame gets corrupted frame">(
                            __FILE__, __LINE__, __func__);

                        continue;
                    }

                    frame->time_base = dec_ctx_->pkt_timebase;

                    frame_process(frame.get());

                    co_yield std::move(frame);

                } else
                    co_return;
            }
        }

    };

    auto decode_with(decoder_t& decoder) {
        return [&decoder](auto packets) -> coro::produce::generator_t<frame_uptr> {
            while (packets) {
                auto p = packets();
                if (!p) continue;

                auto frames = decoder.decode(std::move(p));
                while (frames) {
                    auto f = frames();
                    if (f) co_yield std::move(f);
                }
            }
        };
    }

    auto flush_decoder(decoder_t& decoder) -> coro::produce::generator_t<frame_uptr> {
        auto frames = decoder.decode(nullptr); // EOF/drain
        while (frames) {
            auto f = frames();
            if (f) co_yield std::move(f);
        }
    }

}
