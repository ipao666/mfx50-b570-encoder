#include "mfx50_realtime.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::string input;
    std::string output;
    std::string inputKind = "annexb";
    std::string packetMode = "au";
    std::string codec = "h264";
    std::string profile = "quality";
    std::string device = "auto";
    std::string outputMode = "poll";
    std::string sceneCsv;
    std::string ffmpeg = "ffmpeg";
    std::string ffprobe = "ffprobe";
    int fps = 25;
    int chunkBytes = 64 * 1024;
    int maxPackets = 0;
    int loopCount = 1;
    int streamSeconds = 0;
    int injectEmptyEvery = 0;
    int injectBadEvery = 0;
    int slowCallbackMs = 0;
    int smoothScaleFactor = 0;
    int preDenoiseStrength = 0;
    int asyncMode = 0;
    bool realtime = false;
    bool runFfprobe = true;
    bool strict = false;
    bool callbackQueryStats = true;
    bool drainViaFlush = false;
    bool enableAlgoPreprocess = false;
    bool enableSceneAnalyzer = false;
};

struct NalUnit {
    size_t start = 0;
    size_t payload = 0;
    size_t end = 0;
    int type = -1;
    bool vcl = false;
    bool key = false;
};

struct PacketView {
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool key = false;
};

struct ProbeResult {
    uint64_t inputPackets = 0;
    uint64_t inputBytes = 0;
    uint64_t pushErrors = 0;
    uint64_t outputPackets = 0;
    uint64_t outputBytes = 0;
    uint64_t outputAfterFlush = 0;
    uint64_t sdkKeyPackets = 0;
    uint64_t sdkPtsReordered = 0;
    uint64_t sdkDtsNonMonotonic = 0;
    int64_t lastSdkPts = -1;
    int64_t lastSdkDts = -1;
    bool sawSdkPts = false;
    bool sawSdkDts = false;
};

struct FfprobeResult {
    bool ok = false;
    std::string codec;
    int width = 0;
    int height = 0;
    int frames = 0;
    int keyframes = 0;
    std::string error;
};

struct CallbackState {
    std::ofstream* output = nullptr;
    ProbeResult* result = nullptr;
    MFX50RT_Handle handle = nullptr;
    int slowCallbackMs = 0;
    bool afterFlush = false;
    bool queryStats = true;
    bool writeFailed = false;
    bool statsQueryFailed = false;
};

struct LogState {
    std::ofstream* sceneCsv = nullptr;
    std::mutex* sceneCsvMu = nullptr;
    uint64_t sceneRows = 0;
    bool writeFailed = false;
};

void usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --input <input.h264|url> --output <out.h265> [options]\n"
        "\n"
        "options:\n"
        "  --input-kind annexb|ffmpeg      annexb reads a raw local ES file; ffmpeg handles RTSP/UDP/container input\n"
        "  --packet-mode au|chunk          access-unit packets for annexb, or fixed chunks\n"
        "  --codec h264|hevc               input codec, default h264\n"
        "  --output-mode poll|callback|callback-null\n"
        "  --profile quality|compress|compress90a|compress90b|compress90c|compress90d|throughput\n"
        "  --async-mode                  enable SDK input queue + worker path\n"
        "  --algo-preprocess              enable SDK preprocess stage; sub-filters stay opt-in\n"
        "  --scene-analyzer               enable log-only scene analyzer\n"
        "  --scene-csv <path>             write scene_analyzer rows to CSV\n"
        "  --smooth-scale <1..100>        enable smooth-scale with the given factor\n"
        "  --pre-denoise <1..100>         enable pre-denoise with the given strength\n"
        "  --fps <n>                       default 25\n"
        "  --device <selector>             default auto\n"
        "  --chunk-bytes <n>                default 65536\n"
        "  --max-packets <n>                stop after n pushed packets, 0 means no limit\n"
        "  --max-frames <n>                 alias for --max-packets\n"
        "  --loop <n>                       repeat local annexb input, default 1\n"
        "  --stream-seconds <n>             ffmpeg input duration limit\n"
        "  --realtime                       sleep according to fps between pushes\n"
        "  --strict-realtime                enable --strict and --realtime\n"
        "  --inject-empty-every <n>         push an empty non-EOS packet every n packets\n"
        "  --inject-bad-every <n>           push a small bad packet every n packets\n"
        "  --slow-callback-ms <n>           sleep inside output callback, default 0\n"
        "  --no-callback-query-stats        do not query stats from inside callback\n"
        "  --drain-via-flush                skip EOS packet and drain delayed output in MFX50RT_Flush\n"
        "  --no-ffprobe                     skip output validation\n"
        "  --strict                         return non-zero if ffprobe/keyframe/PTS checks fail\n"
        "\n"
        "examples:\n"
        "  %s --input sample.h264 --output /tmp/out.h265 --device /dev/dri/renderD129\n"
        "  %s --input rtsp://127.0.0.1/live --input-kind ffmpeg --output /tmp/out.h265 --stream-seconds 60\n",
        argv0, argv0, argv0);
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
        else if (arg == "--input-kind") {
            const char* value = needValue("--input-kind");
            if (!value) return false;
            opt->inputKind = value;
        }
        else if (arg == "--input-format") {
            const char* value = needValue("--input-format");
            if (!value) return false;
            opt->inputKind = value;
        }
        else if (arg == "--packet-mode") {
            const char* value = needValue("--packet-mode");
            if (!value) return false;
            opt->packetMode = value;
        }
        else if (arg == "--codec") {
            const char* value = needValue("--codec");
            if (!value) return false;
            opt->codec = value;
        }
        else if (arg == "--profile") {
            const char* value = needValue("--profile");
            if (!value) return false;
            opt->profile = value;
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
        else if (arg == "--device") {
            const char* value = needValue("--device");
            if (!value) return false;
            opt->device = value;
        }
        else if (arg == "--chunk-bytes") {
            const char* value = needValue("--chunk-bytes");
            if (!value || !parseInt(value, &opt->chunkBytes)) return false;
        }
        else if (arg == "--max-packets" || arg == "--max-frames") {
            const char* value = needValue(arg.c_str());
            if (!value || !parseInt(value, &opt->maxPackets)) return false;
        }
        else if (arg == "--loop") {
            const char* value = needValue("--loop");
            if (!value || !parseInt(value, &opt->loopCount)) return false;
        }
        else if (arg == "--stream-seconds") {
            const char* value = needValue("--stream-seconds");
            if (!value || !parseInt(value, &opt->streamSeconds)) return false;
        }
        else if (arg == "--ffmpeg") {
            const char* value = needValue("--ffmpeg");
            if (!value) return false;
            opt->ffmpeg = value;
        }
        else if (arg == "--ffprobe") {
            const char* value = needValue("--ffprobe");
            if (!value) return false;
            opt->ffprobe = value;
        }
        else if (arg == "--inject-empty-every") {
            const char* value = needValue("--inject-empty-every");
            if (!value || !parseInt(value, &opt->injectEmptyEvery)) return false;
        }
        else if (arg == "--inject-bad-every") {
            const char* value = needValue("--inject-bad-every");
            if (!value || !parseInt(value, &opt->injectBadEvery)) return false;
        }
        else if (arg == "--slow-callback-ms") {
            const char* value = needValue("--slow-callback-ms");
            if (!value || !parseInt(value, &opt->slowCallbackMs)) return false;
        }
        else if (arg == "--async-mode" || arg == "--async") {
            opt->asyncMode = 1;
        }
        else if (arg == "--algo-preprocess") {
            opt->enableAlgoPreprocess = true;
        }
        else if (arg == "--scene-analyzer") {
            opt->enableSceneAnalyzer = true;
        }
        else if (arg == "--scene-csv") {
            const char* value = needValue("--scene-csv");
            if (!value) return false;
            opt->sceneCsv = value;
            opt->enableSceneAnalyzer = true;
        }
        else if (arg == "--smooth-scale") {
            const char* value = needValue("--smooth-scale");
            if (!value || !parseInt(value, &opt->smoothScaleFactor) ||
                opt->smoothScaleFactor <= 0 || opt->smoothScaleFactor > 100) {
                std::fprintf(stderr, "--smooth-scale must be in 1..100\n");
                return false;
            }
            opt->enableAlgoPreprocess = true;
        }
        else if (arg == "--pre-denoise") {
            const char* value = needValue("--pre-denoise");
            if (!value || !parseInt(value, &opt->preDenoiseStrength) ||
                opt->preDenoiseStrength <= 0 || opt->preDenoiseStrength > 100) {
                std::fprintf(stderr, "--pre-denoise must be in 1..100\n");
                return false;
            }
            opt->enableAlgoPreprocess = true;
        }
        else if (arg == "--realtime") {
            opt->realtime = true;
        }
        else if (arg == "--strict-realtime") {
            opt->strict = true;
            opt->realtime = true;
        }
        else if (arg == "--no-callback-query-stats") {
            opt->callbackQueryStats = false;
        }
        else if (arg == "--drain-via-flush") {
            opt->drainViaFlush = true;
        }
        else if (arg == "--no-ffprobe") {
            opt->runFfprobe = false;
        }
        else if (arg == "--strict") {
            opt->strict = true;
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
    if (opt->inputKind != "annexb" && opt->inputKind != "ffmpeg") {
        std::fprintf(stderr, "--input-kind must be annexb or ffmpeg\n");
        return false;
    }
    if (opt->packetMode != "au" && opt->packetMode != "chunk") {
        std::fprintf(stderr, "--packet-mode must be au or chunk\n");
        return false;
    }
    if (opt->codec != "h264" && opt->codec != "hevc") {
        std::fprintf(stderr, "--codec must be h264 or hevc\n");
        return false;
    }
    if (opt->outputMode != "poll" && opt->outputMode != "callback" && opt->outputMode != "callback-null") {
        std::fprintf(stderr, "--output-mode must be poll, callback, or callback-null\n");
        return false;
    }
    if (opt->fps <= 0 || opt->chunkBytes <= 0 || opt->loopCount <= 0) {
        std::fprintf(stderr, "fps, chunk-bytes, and loop must be positive\n");
        return false;
    }
    if (opt->slowCallbackMs < 0) {
        std::fprintf(stderr, "--slow-callback-ms must be non-negative\n");
        return false;
    }
    if (opt->smoothScaleFactor < 0 || opt->smoothScaleFactor > 100) {
        std::fprintf(stderr, "--smooth-scale must be in 1..100\n");
        return false;
    }
    if (opt->preDenoiseStrength < 0 || opt->preDenoiseStrength > 100) {
        std::fprintf(stderr, "--pre-denoise must be in 1..100\n");
        return false;
    }
    return true;
}

int profileId(const std::string& value) {
    if (value == "quality" || value == "quality_90_near") return MFX50_PROFILE_QUALITY_90_NEAR;
    if (value == "compress" || value == "compress_85_probe") return MFX50_PROFILE_COMPRESS_85_PROBE;
    if (value == "compress90a" || value == "compress_90_probe_a") return MFX50_PROFILE_COMPRESS_90_PROBE_A;
    if (value == "compress90b" || value == "compress_90_probe_b") return MFX50_PROFILE_COMPRESS_90_PROBE_B;
    if (value == "compress90c" || value == "compress_90_probe_c") return MFX50_PROFILE_COMPRESS_90_PROBE_C;
    if (value == "compress90d" || value == "compress_90_probe_d") return MFX50_PROFILE_COMPRESS_90_PROBE_D;
    if (value == "throughput" || value == "throughput_only") return MFX50_PROFILE_THROUGHPUT_ONLY;
    return -1;
}

MFX50_Codec codecId(const std::string& value) {
    return value == "hevc" ? MFX50_CODEC_HEVC : MFX50_CODEC_H264;
}

std::string shellQuote(const std::string& value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') out += "'\\''";
        else out += ch;
    }
    out += "'";
    return out;
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    if (size <= 0) return {};
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(data.data()), size);
    if (!in) return {};
    return data;
}

bool startCodeAt(const std::vector<uint8_t>& data, size_t pos, size_t* prefix) {
    if (pos + 3 <= data.size() && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1) {
        *prefix = 3;
        return true;
    }
    if (pos + 4 <= data.size() && data[pos] == 0 && data[pos + 1] == 0 &&
        data[pos + 2] == 0 && data[pos + 3] == 1) {
        *prefix = 4;
        return true;
    }
    return false;
}

std::vector<NalUnit> findNals(const std::vector<uint8_t>& data, bool hevc) {
    std::vector<NalUnit> nals;
    size_t pos = 0;
    while (pos + 3 < data.size()) {
        size_t prefix = 0;
        if (!startCodeAt(data, pos, &prefix)) {
            ++pos;
            continue;
        }

        const size_t start = pos;
        const size_t payload = pos + prefix;
        pos = payload;
        while (pos + 3 < data.size()) {
            size_t nextPrefix = 0;
            if (startCodeAt(data, pos, &nextPrefix)) break;
            ++pos;
        }
        const size_t end = pos;
        if (payload >= end) continue;

        NalUnit nal;
        nal.start = start;
        nal.payload = payload;
        nal.end = end;
        if (hevc) {
            nal.type = (data[payload] >> 1) & 0x3f;
            nal.vcl = nal.type <= 31;
            nal.key = nal.type >= 16 && nal.type <= 21;
        }
        else {
            nal.type = data[payload] & 0x1f;
            nal.vcl = nal.type >= 1 && nal.type <= 5;
            nal.key = nal.type == 5;
        }
        nals.push_back(nal);
    }
    return nals;
}

std::vector<PacketView> makeAccessUnits(const std::vector<uint8_t>& data, bool hevc) {
    std::vector<PacketView> packets;
    const std::vector<NalUnit> nals = findNals(data, hevc);
    if (nals.empty()) return packets;

    size_t currentStart = nals[0].start;
    bool sawVcl = false;
    bool currentKey = false;
    const int audType = hevc ? 35 : 9;

    for (const NalUnit& nal : nals) {
        const bool startsNewByAud = nal.type == audType && sawVcl && nal.start > currentStart;
        const bool startsNewByVcl = nal.vcl && sawVcl && nal.start > currentStart;
        if (startsNewByAud || startsNewByVcl) {
            packets.push_back({data.data() + currentStart, nal.start - currentStart, currentKey});
            currentStart = nal.start;
            sawVcl = false;
            currentKey = false;
        }
        if (nal.vcl) sawVcl = true;
        if (nal.key) currentKey = true;
    }

    if (currentStart < data.size()) {
        packets.push_back({data.data() + currentStart, data.size() - currentStart, currentKey});
    }
    return packets;
}

std::vector<PacketView> makeChunkPackets(const std::vector<uint8_t>& data, size_t chunkBytes) {
    std::vector<PacketView> packets;
    for (size_t offset = 0; offset < data.size(); offset += chunkBytes) {
        const size_t size = std::min(chunkBytes, data.size() - offset);
        packets.push_back({data.data() + offset, size, false});
    }
    return packets;
}

void recordOutputPacket(const MFX50RT_EncodedPacket& pkt, ProbeResult* result, bool afterFlush) {
    if (result->sawSdkPts && pkt.pts < result->lastSdkPts) {
        result->sdkPtsReordered++;
    }
    result->sawSdkPts = true;
    result->lastSdkPts = pkt.pts;
    if (result->sawSdkDts && pkt.dts < result->lastSdkDts) {
        result->sdkDtsNonMonotonic++;
    }
    result->sawSdkDts = true;
    result->lastSdkDts = pkt.dts;
    result->outputPackets++;
    result->outputBytes += pkt.size;
    if (afterFlush) result->outputAfterFlush++;
    if (pkt.is_keyframe) result->sdkKeyPackets++;
}

bool pollPackets(MFX50RT_Handle handle,
                 std::ofstream& out,
                 ProbeResult* result,
                 bool afterFlush) {
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
                std::fprintf(stderr, "MFX50RT_PollPacket requested retry without a larger size\n");
                return false;
            }
            buffer.resize(pkt.size);
            continue;
        }
        if (rc != MFX50_OK) {
            std::fprintf(stderr, "MFX50RT_PollPacket failed rc=%d error=%s\n", rc, MFX50RT_GetLastError(handle));
            return false;
        }

        out.write(reinterpret_cast<const char*>(pkt.data), static_cast<std::streamsize>(pkt.size));
        if (!out) {
            std::fprintf(stderr, "write output failed\n");
            return false;
        }

        recordOutputPacket(pkt, result, afterFlush);
    }
}

void outputCallback(const MFX50RT_EncodedPacket* pkt, void* user) {
    CallbackState* state = static_cast<CallbackState*>(user);
    if (!state || !pkt || !state->output || !state->result) return;

    state->output->write(reinterpret_cast<const char*>(pkt->data), static_cast<std::streamsize>(pkt->size));
    if (!*state->output) {
        state->writeFailed = true;
        return;
    }
    recordOutputPacket(*pkt, state->result, state->afterFlush);

    if (state->queryStats && state->handle) {
        MFX50RT_Stats stats = {};
        stats.struct_size = sizeof(stats);
        if (MFX50RT_GetStats(state->handle, &stats) != MFX50_OK) {
            state->statsQueryFailed = true;
        }
    }
    if (state->slowCallbackMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(state->slowCallbackMs));
    }
}

void logCallback(int level, const char* message, void* user) {
    (void)level;
    LogState* state = static_cast<LogState*>(user);
    if (!state || !state->sceneCsv || !message) return;
    if (std::strncmp(message, "scene frame=", 12) != 0) return;

    unsigned long long frame = 0;
    long long pts = 0;
    double flat = 0.0;
    double motion = 0.0;
    double noise = 0.0;
    double edge = 0.0;
    double sceneCut = 0.0;
    double hard = 0.0;
    int suggest = 0;
    const int parsed = std::sscanf(message,
                                   "scene frame=%llu pts=%lld flat=%lf motion=%lf noise=%lf edge=%lf scene_cut=%lf hard=%lf suggest=%d",
                                   &frame,
                                   &pts,
                                   &flat,
                                   &motion,
                                   &noise,
                                   &edge,
                                   &sceneCut,
                                   &hard,
                                   &suggest);
    if (parsed != 9) return;

    std::lock_guard<std::mutex> lock(*state->sceneCsvMu);
    *state->sceneCsv << frame << ','
                     << pts << ','
                     << flat << ','
                     << motion << ','
                     << noise << ','
                     << edge << ','
                     << sceneCut << ','
                     << hard << ','
                     << suggest << '\n';
    if (!*state->sceneCsv) {
        state->writeFailed = true;
        return;
    }
    state->sceneRows++;
}

bool callbackOk(const CallbackState* state) {
    if (!state) return true;
    if (state->writeFailed) {
        std::fprintf(stderr, "callback write output failed\n");
        return false;
    }
    if (state->statsQueryFailed) {
        std::fprintf(stderr, "callback stats query failed\n");
        return false;
    }
    return true;
}

bool shouldPollDuringFeed(const Options& opt) {
    return opt.outputMode == "poll" || opt.outputMode == "callback-null";
}

bool pushOne(MFX50RT_Handle handle,
             const uint8_t* data,
             size_t size,
             int64_t pts,
             bool key,
             std::ofstream& output,
             ProbeResult* result,
             bool pollAfterPush) {
    MFX50RT_Packet pkt = {};
    pkt.struct_size = sizeof(pkt);
    pkt.stream_id = 0;
    pkt.data = data;
    pkt.size = size;
    pkt.pts = pts;
    pkt.dts = pts;
    pkt.is_keyframe = key ? 1 : 0;

    int rc = MFX50RT_PushPacket(handle, &pkt);
    if (rc < 0) {
        result->pushErrors++;
        std::fprintf(stderr, "MFX50RT_PushPacket failed rc=%d pts=%lld error=%s\n",
                     rc,
                     static_cast<long long>(pts),
                     MFX50RT_GetLastError(handle));
        return false;
    }
    result->inputPackets++;
    result->inputBytes += size;
    if (!pollAfterPush) return true;
    return pollPackets(handle, output, result, false);
}

bool sleepForRealtime(const Options& opt) {
    if (!opt.realtime) return true;
    std::this_thread::sleep_for(std::chrono::microseconds(1000000 / std::max(1, opt.fps)));
    return true;
}

bool feedAnnexBFile(const Options& opt, MFX50RT_Handle handle, std::ofstream& output, ProbeResult* result) {
    const std::vector<uint8_t> data = readFile(opt.input);
    if (data.empty()) {
        std::fprintf(stderr, "cannot read input or input is empty: %s\n", opt.input.c_str());
        return false;
    }

    std::vector<PacketView> packets = opt.packetMode == "chunk"
        ? makeChunkPackets(data, static_cast<size_t>(opt.chunkBytes))
        : makeAccessUnits(data, opt.codec == "hevc");
    if (packets.empty()) {
        std::fprintf(stderr, "no Annex-B packets found in %s\n", opt.input.c_str());
        return false;
    }

    int64_t pts = 0;
    const uint8_t badBytes[8] = {0, 0, 1, 0xff, 0xde, 0xad, 0xbe, 0xef};
    for (int loop = 0; loop < opt.loopCount; ++loop) {
        for (const PacketView& view : packets) {
            if (opt.maxPackets > 0 && result->inputPackets >= static_cast<uint64_t>(opt.maxPackets)) {
                return true;
            }
            if (!pushOne(handle, view.data, view.size, pts++, view.key, output, result, shouldPollDuringFeed(opt))) return false;
            if (opt.injectEmptyEvery > 0 && result->inputPackets % static_cast<uint64_t>(opt.injectEmptyEvery) == 0) {
                if (!pushOne(handle, nullptr, 0, pts++, false, output, result, shouldPollDuringFeed(opt))) return false;
            }
            if (opt.injectBadEvery > 0 && result->inputPackets % static_cast<uint64_t>(opt.injectBadEvery) == 0) {
                if (!pushOne(handle, badBytes, sizeof(badBytes), pts++, false, output, result, shouldPollDuringFeed(opt))) return false;
            }
            sleepForRealtime(opt);
        }
    }
    return true;
}

std::string ffmpegCommand(const Options& opt) {
    std::ostringstream cmd;
    cmd << shellQuote(opt.ffmpeg)
        << " -hide_banner -loglevel warning -nostdin -fflags +genpts+discardcorrupt";
    if (opt.streamSeconds > 0) {
        cmd << " -t " << opt.streamSeconds;
    }
    cmd << " -i " << shellQuote(opt.input)
        << " -map 0:v:0 -c:v copy ";
    if (opt.codec == "h264") {
        cmd << "-bsf:v h264_mp4toannexb -f h264 -";
    }
    else {
        cmd << "-bsf:v hevc_mp4toannexb -f hevc -";
    }
    return cmd.str();
}

bool feedFfmpegStream(const Options& opt, MFX50RT_Handle handle, std::ofstream& output, ProbeResult* result) {
    const std::string cmd = ffmpegCommand(opt);
    std::fprintf(stderr, "ffmpeg_input_command=%s\n", cmd.c_str());
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        std::fprintf(stderr, "failed to start ffmpeg input command\n");
        return false;
    }

    std::vector<uint8_t> chunk(static_cast<size_t>(opt.chunkBytes));
    int64_t pts = 0;
    bool ok = true;
    while (!std::feof(pipe)) {
        if (opt.maxPackets > 0 && result->inputPackets >= static_cast<uint64_t>(opt.maxPackets)) {
            break;
        }
        const size_t got = std::fread(chunk.data(), 1, chunk.size(), pipe);
        if (got == 0) break;
        if (!pushOne(handle, chunk.data(), got, pts++, false, output, result, shouldPollDuringFeed(opt))) {
            ok = false;
            break;
        }
        sleepForRealtime(opt);
    }
    const int closeRc = pclose(pipe);
    if (closeRc != 0 && ok) {
        std::fprintf(stderr, "ffmpeg input command exited with status %d\n", closeRc);
        ok = false;
    }
    return ok;
}

std::string runCapture(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::string output;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), pipe)) {
        output += buf;
    }
    const int rc = pclose(pipe);
    if (rc != 0) return "";
    return output;
}

FfprobeResult runFfprobe(const Options& opt) {
    FfprobeResult result;
    const std::string base = shellQuote(opt.ffprobe) + " -v error -select_streams v:0 ";
    const std::string streamCmd =
        base + "-count_frames -show_entries stream=codec_name,width,height,nb_read_frames "
        "-of default=noprint_wrappers=1 " + shellQuote(opt.output);
    const std::string streamOut = runCapture(streamCmd);
    if (streamOut.empty()) {
        result.error = "ffprobe stream query failed";
        return result;
    }

    std::istringstream lines(streamOut);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "codec_name") result.codec = value;
        else if (key == "width") result.width = std::atoi(value.c_str());
        else if (key == "height") result.height = std::atoi(value.c_str());
        else if (key == "nb_read_frames" && value != "N/A") result.frames = std::atoi(value.c_str());
    }

    const std::string frameCmd =
        base + "-show_entries frame=key_frame,pict_type -of csv=p=0 " + shellQuote(opt.output);
    const std::string frameOut = runCapture(frameCmd);
    if (!frameOut.empty()) {
        std::istringstream frameLines(frameOut);
        while (std::getline(frameLines, line)) {
            if (line.empty()) continue;
            if (line.rfind("1,", 0) == 0 || line == "1" || line.find(",I") != std::string::npos) {
                result.keyframes++;
            }
            if (result.frames == 0) {
                result.frames++;
            }
        }
    }

    result.ok = result.codec == "hevc" && result.width > 0 && result.height > 0 && result.frames > 0;
    if (!result.ok) result.error = "ffprobe did not identify a playable HEVC video stream";
    return result;
}

void printStats(const ProbeResult& result, const MFX50RT_Stats& stats, const FfprobeResult* ffprobe) {
    std::printf("sdk_version=%s\n", MFX50RT_GetVersion());
    std::printf("sdk_abi_version=%d\n", MFX50RT_GetAbiVersion());
    std::printf("input_packets=%llu\n", static_cast<unsigned long long>(result.inputPackets));
    std::printf("input_bytes=%llu\n", static_cast<unsigned long long>(result.inputBytes));
    std::printf("push_errors=%llu\n", static_cast<unsigned long long>(result.pushErrors));
    std::printf("decoded_frames=%llu\n", static_cast<unsigned long long>(stats.decoded_frames));
    std::printf("encoded_frames=%llu\n", static_cast<unsigned long long>(stats.encoded_frames));
    std::printf("output_packets=%llu\n", static_cast<unsigned long long>(result.outputPackets));
    std::printf("output_bytes=%llu\n", static_cast<unsigned long long>(result.outputBytes));
    std::printf("output_packets_after_flush=%llu\n", static_cast<unsigned long long>(result.outputAfterFlush));
    std::printf("sdk_key_packets=%llu\n", static_cast<unsigned long long>(result.sdkKeyPackets));
    std::printf("sdk_pts_reordered=%llu\n", static_cast<unsigned long long>(result.sdkPtsReordered));
    std::printf("sdk_dts_nonmonotonic=%llu\n", static_cast<unsigned long long>(result.sdkDtsNonMonotonic));
    std::printf("stats_fps_out=%.3f\n", stats.fps_out);
    std::printf("stats_input_bytes=%llu\n", static_cast<unsigned long long>(stats.input_bytes));
    std::printf("stats_output_bytes=%llu\n", static_cast<unsigned long long>(stats.output_bytes));
    std::printf("stats_decode_errors=%llu\n", static_cast<unsigned long long>(stats.decode_errors));
    std::printf("stats_encode_errors=%llu\n", static_cast<unsigned long long>(stats.encode_errors));
    std::printf("stats_route_count=%d\n", stats.route_count);
    std::printf("stats_abi_version=%d\n", stats.abi_version);
    std::printf("stats_preprocess_frames=%llu\n", static_cast<unsigned long long>(stats.preprocess_frames));
    std::printf("stats_smooth_scale_frames=%llu\n", static_cast<unsigned long long>(stats.smooth_scale_frames));
    std::printf("stats_pre_denoise_frames=%llu\n", static_cast<unsigned long long>(stats.pre_denoise_frames));
    std::printf("stats_scene_analyzed_frames=%llu\n", static_cast<unsigned long long>(stats.scene_analyzed_frames));
    std::printf("stats_adaptive_profile_switches=%llu\n",
                static_cast<unsigned long long>(stats.adaptive_profile_switches));
    std::printf("stats_adaptive_qp_frames=%llu\n", static_cast<unsigned long long>(stats.adaptive_qp_frames));
    std::printf("stats_mbqp_frames=%llu\n", static_cast<unsigned long long>(stats.mbqp_frames));
    std::printf("stats_mbqp_fallback_frames=%llu\n", static_cast<unsigned long long>(stats.mbqp_fallback_frames));
    std::printf("stats_avg_preprocess_ms=%.3f\n", stats.avg_preprocess_ms);
    std::printf("stats_avg_scene_analyze_ms=%.3f\n", stats.avg_scene_analyze_ms);
    std::printf("stats_mbqp_supported=%d\n", stats.mbqp_supported);
    std::printf("stats_mbqp_disabled_reason=%d\n", stats.mbqp_disabled_reason);
    std::printf("stats_active_profile=%d\n", stats.active_profile);
    std::printf("stats_active_algo_flags=%d\n", stats.active_algo_flags);
    std::printf("stats_current_input_queue_packets=%d\n", stats.current_input_queue_packets);
    std::printf("stats_async_mode=%d\n", stats.async_mode);
    std::printf("stats_async_enqueued_packets=%llu\n",
                static_cast<unsigned long long>(stats.async_enqueued_packets));
    std::printf("stats_async_processed_packets=%llu\n",
                static_cast<unsigned long long>(stats.async_processed_packets));
    std::printf("stats_backpressure_events=%llu\n",
                static_cast<unsigned long long>(stats.backpressure_events));
    std::printf("last_error_code=%d\n", stats.last_error_code);
    if (stats.last_error_msg[0]) std::printf("last_error_msg=%s\n", stats.last_error_msg);
    if (ffprobe) {
        std::printf("ffprobe_ok=%d\n", ffprobe->ok ? 1 : 0);
        std::printf("ffprobe_codec=%s\n", ffprobe->codec.c_str());
        std::printf("ffprobe_width=%d\n", ffprobe->width);
        std::printf("ffprobe_height=%d\n", ffprobe->height);
        std::printf("ffprobe_frames=%d\n", ffprobe->frames);
        std::printf("ffprobe_keyframes=%d\n", ffprobe->keyframes);
        if (!ffprobe->error.empty()) std::printf("ffprobe_error=%s\n", ffprobe->error.c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, &opt)) {
        usage(argv[0]);
        return 2;
    }

    const int profile = profileId(opt.profile);
    if (profile < 0) {
        std::fprintf(stderr, "unknown profile: %s\n", opt.profile.c_str());
        return 2;
    }

    std::ofstream output(opt.output, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::fprintf(stderr, "cannot open output: %s\n", opt.output.c_str());
        return 1;
    }

    MFX50RT_Config cfg = {};
    int rc = MFX50RT_DefaultConfig(&cfg);
    if (rc != MFX50_OK) {
        std::fprintf(stderr, "MFX50RT_DefaultConfig failed: %s\n", MFX50RT_GetLastError(nullptr));
        return 1;
    }
    cfg.input_mode = MFX50_INPUT_ENCODED_PACKET;
    cfg.output_mode = opt.outputMode == "poll" ? MFX50_OUTPUT_POLL : MFX50_OUTPUT_CALLBACK;
    cfg.input_codec = codecId(opt.codec);
    cfg.output_codec = MFX50_CODEC_HEVC;
    cfg.profile = static_cast<MFX50_Profile>(profile);
    cfg.device_selector = opt.device.c_str();
    cfg.route_count = 1;
    cfg.fps_num = opt.fps;
    cfg.fps_den = 1;
    cfg.async_mode = opt.asyncMode;
    cfg.max_input_queue_packets = 256;
    cfg.max_output_queue_packets = 256;
    cfg.drop_policy = MFX50RT_DROP_NONE;

    MFX50RT_Handle handle = nullptr;
    rc = MFX50RT_Create(&cfg, &handle);
    if (rc != MFX50_OK) {
        std::fprintf(stderr, "MFX50RT_Create failed rc=%d error=%s\n", rc, MFX50RT_GetLastError(nullptr));
        return 1;
    }
    if (opt.enableAlgoPreprocess || opt.smoothScaleFactor > 0 ||
        opt.preDenoiseStrength > 0 || opt.enableSceneAnalyzer) {
        MFX50RT_AlgoConfig algo = {};
        rc = MFX50RT_DefaultAlgoConfig(&algo);
        if (rc != MFX50_OK) {
            std::fprintf(stderr, "MFX50RT_DefaultAlgoConfig failed rc=%d error=%s\n",
                         rc,
                         MFX50RT_GetLastError(handle));
            MFX50RT_Close(handle);
            return 1;
        }
        algo.enable_preprocess = opt.enableAlgoPreprocess ? 1 : 0;
        if (opt.smoothScaleFactor > 0) {
            algo.enable_smooth_scale = 1;
            algo.smooth_scale_factor = opt.smoothScaleFactor;
        }
        if (opt.preDenoiseStrength > 0) {
            algo.enable_pre_denoise = 1;
            algo.pre_denoise_strength = opt.preDenoiseStrength;
        }
        if (opt.enableSceneAnalyzer) {
            algo.enable_scene_analyzer = 1;
        }
        rc = MFX50RT_SetAlgoConfig(handle, &algo);
        if (rc != MFX50_OK) {
            std::fprintf(stderr, "MFX50RT_SetAlgoConfig failed rc=%d error=%s\n", rc, MFX50RT_GetLastError(handle));
            MFX50RT_Close(handle);
            return 1;
        }
    }

    ProbeResult result;
    CallbackState callbackState;
    std::ofstream sceneCsv;
    std::mutex sceneCsvMu;
    LogState logState;
    callbackState.output = &output;
    callbackState.result = &result;
    callbackState.handle = handle;
    callbackState.slowCallbackMs = opt.slowCallbackMs;
    callbackState.queryStats = opt.callbackQueryStats;
    if (!opt.sceneCsv.empty()) {
        sceneCsv.open(opt.sceneCsv, std::ios::out | std::ios::trunc);
        if (!sceneCsv) {
            std::fprintf(stderr, "cannot open scene CSV: %s\n", opt.sceneCsv.c_str());
            MFX50RT_Close(handle);
            return 1;
        }
        sceneCsv << "frame,pts,flat_score,motion_score,noise_score,edge_score,scene_cut_score,hard_score,suggested_profile\n";
        logState.sceneCsv = &sceneCsv;
        logState.sceneCsvMu = &sceneCsvMu;
        rc = MFX50RT_SetLogCallback(handle, logCallback, &logState);
        if (rc != MFX50_OK) {
            std::fprintf(stderr, "MFX50RT_SetLogCallback failed rc=%d error=%s\n", rc, MFX50RT_GetLastError(handle));
            MFX50RT_Close(handle);
            return 1;
        }
    }
    if (opt.outputMode == "callback") {
        rc = MFX50RT_SetOutputCallback(handle, outputCallback, &callbackState);
        if (rc != MFX50_OK) {
            std::fprintf(stderr, "MFX50RT_SetOutputCallback failed rc=%d error=%s\n", rc, MFX50RT_GetLastError(handle));
            MFX50RT_Close(handle);
            return 1;
        }
    }

    bool ok = false;
    if (opt.inputKind == "annexb") {
        ok = feedAnnexBFile(opt, handle, output, &result);
    }
    else {
        ok = feedFfmpegStream(opt, handle, output, &result);
    }
    ok = ok && callbackOk(&callbackState);

    if (ok && !opt.drainViaFlush) {
        MFX50RT_Packet eos = {};
        eos.struct_size = sizeof(eos);
        eos.stream_id = 0;
        eos.end_of_stream = 1;
        eos.pts = static_cast<int64_t>(result.inputPackets);
        eos.dts = eos.pts;
        rc = MFX50RT_PushPacket(handle, &eos);
        if (rc < 0) {
            std::fprintf(stderr, "MFX50RT_PushPacket(EOS) failed rc=%d error=%s\n", rc, MFX50RT_GetLastError(handle));
            ok = false;
        }
        ok = ok && callbackOk(&callbackState);
    }

    if (ok) {
        callbackState.afterFlush = true;
        rc = MFX50RT_Flush(handle);
        callbackState.afterFlush = false;
        if (rc != MFX50_OK) {
            std::fprintf(stderr, "MFX50RT_Flush failed rc=%d error=%s\n", rc, MFX50RT_GetLastError(handle));
            ok = false;
        }
        if (!callbackOk(&callbackState)) {
            ok = false;
        }
        if (shouldPollDuringFeed(opt) && !pollPackets(handle, output, &result, true)) {
            ok = false;
        }
    }

    MFX50RT_Stats stats = {};
    stats.struct_size = sizeof(stats);
    MFX50RT_GetStats(handle, &stats);
    MFX50RT_Close(handle);
    output.close();
    if (sceneCsv.is_open()) sceneCsv.close();
    if (logState.writeFailed) {
        std::fprintf(stderr, "scene CSV write failed\n");
        ok = false;
    }

    FfprobeResult ffprobe;
    FfprobeResult* ffprobePtr = nullptr;
    if (opt.runFfprobe) {
        ffprobe = runFfprobe(opt);
        ffprobePtr = &ffprobe;
    }
    std::printf("output_mode=%s\n", opt.outputMode.c_str());
    if (!opt.sceneCsv.empty()) {
        std::printf("scene_csv=%s\n", opt.sceneCsv.c_str());
        std::printf("scene_csv_rows=%llu\n", static_cast<unsigned long long>(logState.sceneRows));
    }
    printStats(result, stats, ffprobePtr);

    bool strictOk = ok && result.pushErrors == 0 && result.outputPackets > 0;
    if (opt.runFfprobe) {
        strictOk = strictOk && ffprobe.ok && ffprobe.codec == "hevc" && ffprobe.keyframes > 0;
        if (stats.encoded_frames > 0 && ffprobe.frames > 0) {
            strictOk = strictOk && static_cast<uint64_t>(ffprobe.frames) == stats.encoded_frames;
        }
    }
    if (opt.strict) {
        strictOk = strictOk && result.sdkDtsNonMonotonic == 0;
        strictOk = strictOk && stats.output_packets == result.outputPackets;
        strictOk = strictOk && stats.output_bytes == result.outputBytes;
        strictOk = strictOk && stats.decode_errors == 0;
        strictOk = strictOk && stats.encode_errors == 0;
        if (opt.drainViaFlush) {
            strictOk = strictOk && result.outputAfterFlush > 0;
        }
    }

    return strictOk ? 0 : 1;
}
