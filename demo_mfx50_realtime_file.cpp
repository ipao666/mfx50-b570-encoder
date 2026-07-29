#include "mfx50_realtime.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

int parseProfile(const char* value) {
    if (!value || !value[0] || std::strcmp(value, "quality") == 0 ||
        std::strcmp(value, "quality_90_near") == 0) {
        return MFX50_PROFILE_QUALITY_90_NEAR;
    }
    if (std::strcmp(value, "compress") == 0 ||
        std::strcmp(value, "compress_85_probe") == 0) {
        return MFX50_PROFILE_COMPRESS_85_PROBE;
    }
    if (std::strcmp(value, "compress90a") == 0 ||
        std::strcmp(value, "compress_90_probe_a") == 0) {
        return MFX50_PROFILE_COMPRESS_90_PROBE_A;
    }
    if (std::strcmp(value, "compress90b") == 0 ||
        std::strcmp(value, "compress_90_probe_b") == 0) {
        return MFX50_PROFILE_COMPRESS_90_PROBE_B;
    }
    if (std::strcmp(value, "compress90c") == 0 ||
        std::strcmp(value, "compress_90_probe_c") == 0) {
        return MFX50_PROFILE_COMPRESS_90_PROBE_C;
    }
    if (std::strcmp(value, "compress90d") == 0 ||
        std::strcmp(value, "compress_90_probe_d") == 0) {
        return MFX50_PROFILE_COMPRESS_90_PROBE_D;
    }
    if (std::strcmp(value, "throughput") == 0 ||
        std::strcmp(value, "throughput_only") == 0) {
        return MFX50_PROFILE_THROUGHPUT_ONLY;
    }
    if (std::strcmp(value, "debug") == 0) {
        return MFX50_PROFILE_DEBUG_TRACE;
    }
    return -1;
}

bool pollAvailable(MFX50RT_Handle handle, std::ofstream& output, uint64_t& packets, uint64_t& bytes) {
    std::vector<uint8_t> buffer(8 * 1024 * 1024);
    for (;;) {
        MFX50RT_EncodedPacket pkt = {};
        pkt.struct_size = sizeof(pkt);
        pkt.data = buffer.data();
        pkt.capacity = buffer.size();

        int rc = MFX50RT_PollPacket(handle, &pkt);
        if (rc == MFX50_ERR_NO_OUTPUT) {
            return true;
        }
        if (rc == MFX50_ERR_BUFFER_TOO_SMALL || rc == MFX50_ERR_AGAIN) {
            if (pkt.size <= buffer.size()) {
                std::fprintf(stderr, "poll requested retry without larger size\n");
                return false;
            }
            buffer.resize(pkt.size);
            continue;
        }
        if (rc != MFX50_OK) {
            std::fprintf(stderr, "poll failed: rc=%d error=%s\n", rc, MFX50RT_GetLastError(handle));
            return false;
        }
        output.write(reinterpret_cast<const char*>(pkt.data), static_cast<std::streamsize>(pkt.size));
        if (!output) {
            std::fprintf(stderr, "write output failed\n");
            return false;
        }
        packets++;
        bytes += pkt.size;
    }
}

void printUsage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s <input.h264> <output.hevc> [profile] [fps] [device] [chunk_bytes]\n"
        "\n"
        "profiles:\n"
        "  quality            MFX50_PROFILE_QUALITY_90_NEAR (default)\n"
        "  compress           MFX50_PROFILE_COMPRESS_85_PROBE\n"
        "  compress90a        MFX50_PROFILE_COMPRESS_90_PROBE_A\n"
        "  compress90b        MFX50_PROFILE_COMPRESS_90_PROBE_B\n"
        "  compress90c        MFX50_PROFILE_COMPRESS_90_PROBE_C\n"
        "  compress90d        MFX50_PROFILE_COMPRESS_90_PROBE_D\n"
        "  throughput         MFX50_PROFILE_THROUGHPUT_ONLY\n"
        "\n"
        "example:\n"
        "  %s sample.h264 /tmp/out.hevc quality 25 auto 65536\n",
        argv0, argv0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        printUsage(argv[0]);
        return 2;
    }

    const char* inputPath = argv[1];
    const char* outputPath = argv[2];
    int profile = argc >= 4 ? parseProfile(argv[3]) : MFX50_PROFILE_QUALITY_90_NEAR;
    if (profile < 0) {
        std::fprintf(stderr, "unknown profile: %s\n", argv[3]);
        return 2;
    }

    int fps = argc >= 5 ? std::atoi(argv[4]) : 25;
    if (fps <= 0) {
        std::fprintf(stderr, "fps must be positive\n");
        return 2;
    }

    std::string device = argc >= 6 ? argv[5] : "auto";
    size_t chunkBytes = argc >= 7 ? static_cast<size_t>(std::strtoull(argv[6], nullptr, 10)) : 64 * 1024;
    if (chunkBytes == 0) {
        std::fprintf(stderr, "chunk_bytes must be positive\n");
        return 2;
    }

    std::ifstream input(inputPath, std::ios::binary);
    if (!input) {
        std::fprintf(stderr, "cannot open input: %s\n", inputPath);
        return 1;
    }

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::fprintf(stderr, "cannot open output: %s\n", outputPath);
        return 1;
    }

    MFX50RT_Config cfg = {};
    int rc = MFX50RT_DefaultConfig(&cfg);
    if (rc != MFX50_OK) {
        std::fprintf(stderr, "MFX50RT_DefaultConfig failed: %s\n", MFX50RT_GetLastError(nullptr));
        return 1;
    }

    cfg.input_mode = MFX50_INPUT_ENCODED_PACKET;
    cfg.output_mode = MFX50_OUTPUT_POLL;
    cfg.input_codec = MFX50_CODEC_H264;
    cfg.output_codec = MFX50_CODEC_HEVC;
    cfg.profile = static_cast<MFX50_Profile>(profile);
    cfg.device_selector = device.c_str();
    cfg.route_count = 1;
    cfg.fps_num = fps;
    cfg.fps_den = 1;

    MFX50RT_Handle handle = nullptr;
    rc = MFX50RT_Create(&cfg, &handle);
    if (rc != MFX50_OK) {
        std::fprintf(stderr, "MFX50RT_Create failed: rc=%d error=%s\n", rc, MFX50RT_GetLastError(nullptr));
        return 1;
    }

    std::vector<uint8_t> chunk(chunkBytes);
    uint64_t inputPackets = 0;
    uint64_t outputPackets = 0;
    uint64_t outputBytes = 0;
    int64_t pts = 0;
    bool ok = true;

    while (input) {
        input.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        std::streamsize got = input.gcount();
        if (got <= 0) {
            break;
        }

        MFX50RT_Packet pkt = {};
        pkt.struct_size = sizeof(pkt);
        pkt.stream_id = 0;
        pkt.data = chunk.data();
        pkt.size = static_cast<size_t>(got);
        pkt.pts = pts++;
        pkt.dts = pkt.pts;

        rc = MFX50RT_PushPacket(handle, &pkt);
        if (rc < 0) {
            std::fprintf(stderr, "MFX50RT_PushPacket failed: rc=%d error=%s\n", rc, MFX50RT_GetLastError(handle));
            ok = false;
            break;
        }
        inputPackets++;
        if (!pollAvailable(handle, output, outputPackets, outputBytes)) {
            ok = false;
            break;
        }
    }

    if (ok) {
        MFX50RT_Packet eos = {};
        eos.struct_size = sizeof(eos);
        eos.stream_id = 0;
        eos.end_of_stream = 1;
        eos.pts = pts;
        eos.dts = pts;
        rc = MFX50RT_PushPacket(handle, &eos);
        if (rc < 0) {
            std::fprintf(stderr, "MFX50RT_PushPacket(EOS) failed: rc=%d error=%s\n", rc, MFX50RT_GetLastError(handle));
            ok = false;
        }
    }

    if (ok) {
        rc = MFX50RT_Flush(handle);
        if (rc != MFX50_OK) {
            std::fprintf(stderr, "MFX50RT_Flush failed: rc=%d error=%s\n", rc, MFX50RT_GetLastError(handle));
            ok = false;
        }
    }

    if (ok && !pollAvailable(handle, output, outputPackets, outputBytes)) {
        ok = false;
    }

    MFX50RT_Stats stats = {};
    stats.struct_size = sizeof(stats);
    MFX50RT_GetStats(handle, &stats);
    MFX50RT_Close(handle);

    output.close();
    std::printf("input_packets=%llu\n", static_cast<unsigned long long>(inputPackets));
    std::printf("decoded_frames=%llu\n", static_cast<unsigned long long>(stats.decoded_frames));
    std::printf("encoded_frames=%llu\n", static_cast<unsigned long long>(stats.encoded_frames));
    std::printf("output_packets=%llu\n", static_cast<unsigned long long>(outputPackets));
    std::printf("output_bytes=%llu\n", static_cast<unsigned long long>(outputBytes));
    std::printf("fps_out=%.3f\n", stats.fps_out);

    return ok ? 0 : 1;
}
