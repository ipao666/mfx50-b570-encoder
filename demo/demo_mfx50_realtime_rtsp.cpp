#include "mfx50_realtime.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/time.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string input;
    std::string output;
    std::string profile = "quality";
    std::string device = "auto";
    std::string outputMode = "poll";
    int seconds = 60;
    int fps = 25;
};

struct Counters {
    uint64_t inputPackets = 0;
    uint64_t outputPackets = 0;
    uint64_t outputBytes = 0;
    bool writeFailed = false;
};

struct CallbackState {
    std::ofstream* output = nullptr;
    Counters* counters = nullptr;
};

std::string avError(int rc) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(rc, buf, sizeof(buf));
    return buf;
}

int parseProfile(const std::string& value) {
    if (value == "quality" || value == "quality_90_near") return MFX50_PROFILE_QUALITY_90_NEAR;
    if (value == "compress" || value == "compress_85_probe") return MFX50_PROFILE_COMPRESS_85_PROBE;
    if (value == "compress90a" || value == "compress_90_probe_a") return MFX50_PROFILE_COMPRESS_90_PROBE_A;
    if (value == "compress90b" || value == "compress_90_probe_b") return MFX50_PROFILE_COMPRESS_90_PROBE_B;
    if (value == "compress90c" || value == "compress_90_probe_c") return MFX50_PROFILE_COMPRESS_90_PROBE_C;
    if (value == "compress90d" || value == "compress_90_probe_d") return MFX50_PROFILE_COMPRESS_90_PROBE_D;
    if (value == "throughput" || value == "throughput_only") return MFX50_PROFILE_THROUGHPUT_ONLY;
    return -1;
}

void usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --input <rtsp-url> --output <out.h265> [options]\n"
        "\n"
        "options:\n"
        "  --seconds <n>                 capture duration, default 60\n"
        "  --profile quality|compress|compress90a|compress90b|compress90c|compress90d|throughput\n"
        "  --device <selector>           default auto\n"
        "  --output-mode poll|callback   default poll\n"
        "  --fps <n>                     SDK nominal FPS, default 25\n",
        argv0);
}

bool parseInt(const char* text, int* out) {
    if (!text || !out) return false;
    char* end = nullptr;
    long value = std::strtol(text, &end, 10);
    if (!end || *end != '\0') return false;
    *out = static_cast<int>(value);
    return true;
}

bool parseArgs(int argc, char** argv, Options* opt) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto needValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            std::exit(0);
        }
        else if (arg == "--input") {
            const char* value = needValue("--input");
            if (!value) return false;
            opt->input = value;
        }
        else if (arg == "--output") {
            const char* value = needValue("--output");
            if (!value) return false;
            opt->output = value;
        }
        else if (arg == "--seconds") {
            const char* value = needValue("--seconds");
            if (!value || !parseInt(value, &opt->seconds)) return false;
        }
        else if (arg == "--profile") {
            const char* value = needValue("--profile");
            if (!value) return false;
            opt->profile = value;
        }
        else if (arg == "--device") {
            const char* value = needValue("--device");
            if (!value) return false;
            opt->device = value;
        }
        else if (arg == "--output-mode") {
            const char* value = needValue("--output-mode");
            if (!value) return false;
            opt->outputMode = value;
        }
        else if (arg == "--fps") {
            const char* value = needValue("--fps");
            if (!value || !parseInt(value, &opt->fps)) return false;
        }
        else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return false;
        }
    }

    if (opt->input.empty() || opt->output.empty()) {
        std::fprintf(stderr, "--input and --output are required\n");
        return false;
    }
    if (opt->seconds <= 0 || opt->fps <= 0) {
        std::fprintf(stderr, "--seconds and --fps must be positive\n");
        return false;
    }
    if (opt->outputMode != "poll" && opt->outputMode != "callback") {
        std::fprintf(stderr, "--output-mode must be poll or callback\n");
        return false;
    }
    return true;
}

bool pollAvailable(MFX50RT_Handle handle, std::ofstream& output, Counters* counters) {
    std::vector<uint8_t> buffer(8 * 1024 * 1024);
    for (;;) {
        MFX50RT_EncodedPacket pkt = {};
        pkt.struct_size = sizeof(pkt);
        pkt.data = buffer.data();
        pkt.capacity = buffer.size();
        int rc = MFX50RT_PollPacket(handle, &pkt);
        if (rc == MFX50_ERR_NO_OUTPUT) return true;
        if (rc == MFX50_ERR_BUFFER_TOO_SMALL || rc == MFX50_ERR_AGAIN) {
            if (pkt.size <= buffer.size()) {
                std::fprintf(stderr, "poll requested retry without larger size\n");
                return false;
            }
            buffer.resize(pkt.size);
            continue;
        }
        if (rc != MFX50_OK) {
            std::fprintf(stderr, "MFX50RT_PollPacket failed rc=%d error=%s\n", rc, MFX50RT_GetLastError(handle));
            return false;
        }
        output.write(reinterpret_cast<const char*>(pkt.data), static_cast<std::streamsize>(pkt.size));
        if (!output) {
            std::fprintf(stderr, "write output failed\n");
            return false;
        }
        counters->outputPackets++;
        counters->outputBytes += pkt.size;
    }
}

void outputCallback(const MFX50RT_EncodedPacket* pkt, void* user) {
    CallbackState* state = static_cast<CallbackState*>(user);
    if (!state || !pkt || !state->output || !state->counters) return;
    state->output->write(reinterpret_cast<const char*>(pkt->data), static_cast<std::streamsize>(pkt->size));
    if (!*state->output) {
        state->counters->writeFailed = true;
        return;
    }
    state->counters->outputPackets++;
    state->counters->outputBytes += pkt->size;
}

MFX50_Codec mapCodec(AVCodecID id) {
    if (id == AV_CODEC_ID_HEVC) return MFX50_CODEC_HEVC;
    return MFX50_CODEC_H264;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, &opt)) {
        usage(argv[0]);
        return 2;
    }

    const int profile = parseProfile(opt.profile);
    if (profile < 0) {
        std::fprintf(stderr, "unknown profile: %s\n", opt.profile.c_str());
        return 2;
    }

    std::ofstream output(opt.output, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::fprintf(stderr, "cannot open output: %s\n", opt.output.c_str());
        return 1;
    }

    avformat_network_init();
    AVFormatContext* fmt = nullptr;
    AVDictionary* openOpts = nullptr;
    av_dict_set(&openOpts, "rtsp_transport", "tcp", 0);
    int rc = avformat_open_input(&fmt, opt.input.c_str(), nullptr, &openOpts);
    av_dict_free(&openOpts);
    if (rc < 0) {
        std::fprintf(stderr, "avformat_open_input failed: %s\n", avError(rc).c_str());
        return 1;
    }

    rc = avformat_find_stream_info(fmt, nullptr);
    if (rc < 0) {
        std::fprintf(stderr, "avformat_find_stream_info failed: %s\n", avError(rc).c_str());
        avformat_close_input(&fmt);
        return 1;
    }

    const int videoIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIndex < 0) {
        std::fprintf(stderr, "no video stream found\n");
        avformat_close_input(&fmt);
        return 1;
    }

    AVCodecParameters* codecpar = fmt->streams[videoIndex]->codecpar;
    if (codecpar->codec_id != AV_CODEC_ID_H264 &&
        codecpar->codec_id != AV_CODEC_ID_HEVC) {
        std::fprintf(stderr, "unsupported input codec_id=%d; expected H.264 or HEVC\n", codecpar->codec_id);
        avformat_close_input(&fmt);
        return 1;
    }

    MFX50RT_Config cfg = {};
    rc = MFX50RT_DefaultConfig(&cfg);
    if (rc != MFX50_OK) {
        std::fprintf(stderr, "MFX50RT_DefaultConfig failed: %s\n", MFX50RT_GetLastError(nullptr));
        avformat_close_input(&fmt);
        return 1;
    }
    cfg.input_mode = MFX50_INPUT_ENCODED_PACKET;
    cfg.output_mode = opt.outputMode == "callback" ? MFX50_OUTPUT_CALLBACK : MFX50_OUTPUT_POLL;
    cfg.input_codec = mapCodec(codecpar->codec_id);
    cfg.output_codec = MFX50_CODEC_HEVC;
    cfg.profile = static_cast<MFX50_Profile>(profile);
    cfg.device_selector = opt.device.c_str();
    cfg.route_count = 1;
    cfg.fps_num = opt.fps;
    cfg.fps_den = 1;

    MFX50RT_Handle handle = nullptr;
    rc = MFX50RT_Create(&cfg, &handle);
    if (rc != MFX50_OK) {
        std::fprintf(stderr, "MFX50RT_Create failed rc=%d error=%s\n", rc, MFX50RT_GetLastError(nullptr));
        avformat_close_input(&fmt);
        return 1;
    }

    Counters counters;
    CallbackState callbackState{&output, &counters};
    if (opt.outputMode == "callback") {
        rc = MFX50RT_SetOutputCallback(handle, outputCallback, &callbackState);
        if (rc != MFX50_OK) {
            std::fprintf(stderr, "MFX50RT_SetOutputCallback failed rc=%d error=%s\n", rc, MFX50RT_GetLastError(handle));
            MFX50RT_Close(handle);
            avformat_close_input(&fmt);
            return 1;
        }
    }

    const int64_t deadlineUs = av_gettime_relative() + static_cast<int64_t>(opt.seconds) * 1000000;
    int64_t generatedPts = 0;
    bool ok = true;
    AVPacket* packet = av_packet_alloc();
    while (packet && av_gettime_relative() < deadlineUs) {
        rc = av_read_frame(fmt, packet);
        if (rc == AVERROR_EOF) break;
        if (rc < 0) {
            std::fprintf(stderr, "av_read_frame failed: %s\n", avError(rc).c_str());
            ok = false;
            break;
        }
        if (packet->stream_index == videoIndex && packet->data && packet->size > 0) {
            MFX50RT_Packet in = {};
            in.struct_size = sizeof(in);
            in.stream_id = 0;
            in.data = packet->data;
            in.size = static_cast<size_t>(packet->size);
            in.pts = packet->pts == AV_NOPTS_VALUE ? generatedPts : packet->pts;
            in.dts = packet->dts == AV_NOPTS_VALUE ? in.pts : packet->dts;
            in.is_keyframe = (packet->flags & AV_PKT_FLAG_KEY) ? 1 : 0;

            rc = MFX50RT_PushPacket(handle, &in);
            if (rc < 0) {
                std::fprintf(stderr, "MFX50RT_PushPacket failed rc=%d error=%s\n", rc, MFX50RT_GetLastError(handle));
                ok = false;
                av_packet_unref(packet);
                break;
            }
            counters.inputPackets++;
            generatedPts++;
            if (opt.outputMode == "poll" && !pollAvailable(handle, output, &counters)) {
                ok = false;
                av_packet_unref(packet);
                break;
            }
            if (counters.writeFailed) {
                std::fprintf(stderr, "callback write output failed\n");
                ok = false;
                av_packet_unref(packet);
                break;
            }
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);

    if (ok) {
        rc = MFX50RT_Flush(handle);
        if (rc != MFX50_OK) {
            std::fprintf(stderr, "MFX50RT_Flush failed rc=%d error=%s\n", rc, MFX50RT_GetLastError(handle));
            ok = false;
        }
    }
    if (ok && opt.outputMode == "poll" && !pollAvailable(handle, output, &counters)) {
        ok = false;
    }
    if (counters.writeFailed) {
        ok = false;
    }

    MFX50RT_Stats stats = {};
    stats.struct_size = sizeof(stats);
    MFX50RT_GetStats(handle, &stats);
    MFX50RT_Close(handle);
    avformat_close_input(&fmt);
    avformat_network_deinit();
    output.close();

    std::printf("input_packets=%llu\n", static_cast<unsigned long long>(counters.inputPackets));
    std::printf("decoded_frames=%llu\n", static_cast<unsigned long long>(stats.decoded_frames));
    std::printf("encoded_frames=%llu\n", static_cast<unsigned long long>(stats.encoded_frames));
    std::printf("output_packets=%llu\n", static_cast<unsigned long long>(counters.outputPackets));
    std::printf("output_bytes=%llu\n", static_cast<unsigned long long>(counters.outputBytes));
    std::printf("stats_decode_errors=%llu\n", static_cast<unsigned long long>(stats.decode_errors));
    std::printf("stats_encode_errors=%llu\n", static_cast<unsigned long long>(stats.encode_errors));

    return ok ? 0 : 1;
}
