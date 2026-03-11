// gsdk
#include <gremsnoort/sdk/forward/program_options.hpp>
#include <gremsnoort/sdk/ffmpeg/pipeline.hpp>

namespace {

auto parse_video_codec_id(const std::string& name) -> AVCodecID {
    if (name == "h264" || name == "libx264")
        return AV_CODEC_ID_H264;
    if (name == "h265" || name == "hevc" || name == "libx265")
        return AV_CODEC_ID_HEVC;
    if (name == "vp9")
        return AV_CODEC_ID_VP9;
    if (name == "av1")
        return AV_CODEC_ID_AV1;

    return AV_CODEC_ID_NONE;
}

auto parse_audio_codec_id(const std::string& name) -> AVCodecID {
    if (name == "aac")
        return AV_CODEC_ID_AAC;
    if (name == "opus")
        return AV_CODEC_ID_OPUS;
    if (name == "mp3")
        return AV_CODEC_ID_MP3;

    return AV_CODEC_ID_NONE;
}

} // namespace

int main(int argc, char* argv[]) {

    auto logger = gremsnoort::sdk::make_trivial_logger();

    using pipeline_t = gremsnoort::sdk::ffmpeg::pipeline_t;
    using options_t = pipeline_t::options_t;

    auto po = gremsnoort::sdk::program_options_t("pipeline_example", "Simple FFmpeg transcoder pipeline example");
    po.add_options()
        ("h,help", "Show help")
        ("s,source", "Input media file path", cxxopts::value<std::string>())
        ("d,dest", "Output media file path", cxxopts::value<std::string>())
        ("v,vcodec", "Target video codec [h264|h265|hevc|vp9|av1]", cxxopts::value<std::string>()->default_value("h264"))
        ("a,acodec", "Target audio codec [aac|opus|mp3]", cxxopts::value<std::string>()->default_value("aac"))
        ("r,restart", "Restart pipeline after completion", cxxopts::value<bool>()->default_value("false"));

    auto result = po.parse(argc, argv);
    if (result.count("help")) {
        std::printf("%s\n", po.help().c_str());
        return 0;
    }

    options_t options{
        .source = "",
        .destination = "",
        .video_codec_id = AV_CODEC_ID_H264,
        .audio_codec_id = AV_CODEC_ID_AAC,
        .need_restart = false
    };

    po.retrieve_opt(options.source, "source", "Option --source", result);
    po.retrieve_opt(options.destination, "dest", "Option --dest", result);

    auto vcodec_name = std::string{};
    po.retrieve_opt(vcodec_name, "vcodec", "Option --vcodec", result, false);
    options.video_codec_id = parse_video_codec_id(vcodec_name);
    if (options.video_codec_id == AV_CODEC_ID_NONE) {
        logger->log<gremsnoort::sdk::logger_t::level_e::critical, "[{}:{}:{}] unsupported video codec `{}`">(
            __FILE__, __LINE__, __func__, vcodec_name);
        std::printf("%s\n", po.help().c_str());
        return 2;
    }

    auto acodec_name = std::string{};
    po.retrieve_opt(acodec_name, "acodec", "Option --acodec", result, false);
    options.audio_codec_id = parse_audio_codec_id(acodec_name);
    if (options.audio_codec_id == AV_CODEC_ID_NONE) {
        logger->log<gremsnoort::sdk::logger_t::level_e::critical, "[{}:{}:{}] unsupported audio codec `{}`">(
            __FILE__, __LINE__, __func__, acodec_name);
        std::printf("%s\n", po.help().c_str());
        return 2;
    }

    po.retrieve_opt(options.need_restart, "restart", "Option --restart", result, false);

    pipeline_t::run(options, logger);

    [[maybe_unused]] auto c = std::getc(stdin);

    return 0;
}
