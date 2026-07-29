#include "onevpl_realtime_adapter.h"

#include <dlfcn.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace mfx50rt::onevpl {

namespace {

constexpr int kLegacyOk = 0;
constexpr int kLegacyInvalidArg = -1;
constexpr int kLegacyDevice = -2;
constexpr int kLegacyDecode = -3;
constexpr int kLegacyEncode = -4;
constexpr int kLegacyNotImplemented = -5;
constexpr int kLegacyBackpressure = -44;
constexpr int kLegacyBufferTooSmall = -43;
constexpr int kLegacyNoOutput = 1;
constexpr int kLegacyNeedMoreInput = 2;
constexpr int kLegacyAgain = 3;

constexpr int kLegacyInputEncodedPacket = 1;
constexpr int kLegacyOutputPoll = 1;
constexpr int kLegacyCodecH264 = 1;
constexpr int kLegacyCodecHevc = 2;
constexpr int kLegacyProfileThroughput = 0;
constexpr int kLegacyProfileQuality90 = 1;
constexpr int kLegacyProfileCompress85 = 2;
constexpr int kLegacyProfileCompress90B = 5;
constexpr int kLegacyProfileCompress90D = 7;

struct LegacyConfig {
    uint32_t struct_size;
    int input_mode;
    int output_mode;
    int input_codec;
    int output_codec;
    int width;
    int height;
    int fps_num;
    int fps_den;
    const char* device_selector;
    int profile;
    int route_count;
    int async_depth;
    int max_queue_packets;
    int max_queue_surfaces;
    int algo_budget_us;
    int target_usage;
    int gop;
    int gop_ref_dist;
    int num_ref_frame;
    int qpi;
    int qpp;
    int qpb;
    int bref_type;
    int enable_trace;
    const char* trace_path;
    void* user_opaque;
    uint32_t abi_version;
    int async_mode;
    int max_input_queue_packets;
    int max_output_queue_packets;
    int drop_policy;
    int enable_static_reuse;
};

struct LegacyPacket {
    uint32_t struct_size;
    int stream_id;
    const uint8_t* data;
    size_t size;
    int64_t pts;
    int64_t dts;
    int is_keyframe;
    int end_of_stream;
    void* user_opaque;
};

struct LegacyEncodedPacket {
    uint32_t struct_size;
    int stream_id;
    uint8_t* data;
    size_t size;
    size_t capacity;
    int64_t pts;
    int64_t dts;
    int is_keyframe;
    int frame_type;
    void* user_opaque;
};

using LegacyHandle = void*;
using DefaultConfigFn = int (*)(LegacyConfig*);
using CreateFn = int (*)(const LegacyConfig*, LegacyHandle*);
using PushPacketFn = int (*)(LegacyHandle, const LegacyPacket*);
using PollPacketFn = int (*)(LegacyHandle, LegacyEncodedPacket*);
using FlushFn = int (*)(LegacyHandle);
using CloseFn = int (*)(LegacyHandle);
using LastErrorFn = const char* (*)(LegacyHandle);
using SetFrameDecisionCallbackFn = int (*)(
    LegacyHandle,
    MFX50RT_InternalFrameDecisionCallback,
    void*);
using SetEncodeControlEventCallbackFn = int (*)(
    LegacyHandle,
    MFX50RT_InternalEncodeControlEventCallback,
    void*);

MFX50RT_Status mapStatus(int status) {
    switch (status) {
        case kLegacyOk:
        case kLegacyNeedMoreInput:
            return MFX50RT_OK;
        case kLegacyNoOutput:
        case kLegacyAgain:
        case kLegacyBackpressure:
            return MFX50RT_ERR_AGAIN;
        case kLegacyBufferTooSmall:
            return MFX50RT_ERR_BUFFER_TOO_SMALL;
        case kLegacyInvalidArg:
            return MFX50RT_ERR_INVALID_ARG;
        case kLegacyNotImplemented:
            return MFX50RT_ERR_UNSUPPORTED;
        case kLegacyDevice:
        case kLegacyDecode:
        case kLegacyEncode:
            return MFX50RT_ERR_DEVICE;
        default:
            return status < 0 ? MFX50RT_ERR_UNKNOWN : MFX50RT_OK;
    }
}

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool boolOptionEnabled(const char* json, const char* key) {
    if (!json || !key) return false;
    const std::string source = lowerCopy(json);
    const std::string needle = std::string("\"") + lowerCopy(key) + "\"";
    const size_t key_pos = source.find(needle);
    if (key_pos == std::string::npos) return false;
    const size_t colon = source.find(':', key_pos + needle.size());
    if (colon == std::string::npos) return false;
    size_t value_pos = source.find_first_not_of(" \t\r\n\"", colon + 1);
    if (value_pos == std::string::npos) return false;
    return source.compare(value_pos, 1, "1") == 0 ||
           source.compare(value_pos, 4, "true") == 0 ||
           source.compare(value_pos, 2, "on") == 0 ||
           source.compare(value_pos, 4, "auto") == 0;
}

bool boolOptionDisabled(const char* json, const char* key) {
    if (!json || !key) return false;
    const std::string source = lowerCopy(json);
    const std::string needle = std::string("\"") + lowerCopy(key) + "\"";
    const size_t key_pos = source.find(needle);
    if (key_pos == std::string::npos) return false;
    const size_t colon = source.find(':', key_pos + needle.size());
    if (colon == std::string::npos) return false;
    size_t value_pos = source.find_first_not_of(" \t\r\n\"", colon + 1);
    if (value_pos == std::string::npos) return false;
    return source.compare(value_pos, 1, "0") == 0 ||
           source.compare(value_pos, 5, "false") == 0 ||
           source.compare(value_pos, 3, "off") == 0 ||
           source.compare(value_pos, 7, "disable") == 0;
}

bool intOption(const char* json, const char* key, int* out) {
    if (!json || !key || !out) return false;
    const std::string source = lowerCopy(json);
    const std::string needle = std::string("\"") + lowerCopy(key) + "\"";
    const size_t key_pos = source.find(needle);
    if (key_pos == std::string::npos) return false;
    const size_t colon = source.find(':', key_pos + needle.size());
    if (colon == std::string::npos) return false;
    size_t value_pos = source.find_first_not_of(" \t\r\n\"", colon + 1);
    if (value_pos == std::string::npos) return false;
    char* end = nullptr;
    const long value = std::strtol(source.c_str() + value_pos, &end, 10);
    if (end == source.c_str() + value_pos) return false;
    *out = static_cast<int>(value);
    return true;
}

bool trafficSemanticStaticReuseDefault(const MFX50RT_Config& config) {
    const std::string expert = lowerCopy(config.algo.expert_options_json);
    return expert.find("\"profile\":\"target90_simple_class\"") != std::string::npos ||
           expert.find("\"profile\":\"target_90_simple_class\"") != std::string::npos ||
           expert.find("\"hybridtsrq_profile\":\"target90_simple_class\"") != std::string::npos ||
           expert.find("\"hybridtsrq_profile\":\"target_90_simple_class\"") != std::string::npos;
}

int legacyCodec(MFX50RT_Codec codec) {
    if (codec == MFX50RT_CODEC_HEVC) return kLegacyCodecHevc;
    return kLegacyCodecH264;
}

int legacyProfile(const MFX50RT_Config& config) {
    if (config.algo.profile == MFX50RT_PROFILE_LOW_LATENCY) return kLegacyProfileThroughput;
    if (config.algo.profile == MFX50RT_PROFILE_MAX_COMPRESSION) return kLegacyProfileCompress90D;
    if (config.algo.profile == MFX50RT_PROFILE_SAFE) return kLegacyProfileQuality90;
    if (config.algo.target_compression_percent >= 90) return kLegacyProfileCompress90D;
    if (config.algo.target_compression_percent >= 85) return kLegacyProfileCompress85;
    return kLegacyProfileQuality90;
}

std::string dirnameOf(const char* path) {
    if (!path || !path[0]) return {};
    std::string s(path);
    const size_t slash = s.find_last_of('/');
    if (slash == std::string::npos) return {};
    return s.substr(0, slash);
}

std::vector<std::string> libraryCandidates(const MFX50RT_Config& config) {
    std::vector<std::string> out;
    if (config.backend.library_path[0]) out.emplace_back(config.backend.library_path);

    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&libraryCandidates), &info) && info.dli_fname) {
        const std::string dir = dirnameOf(info.dli_fname);
        if (!dir.empty()) out.emplace_back(dir + "/libmfx50_realtime.so");
    }

    out.emplace_back("./libmfx50_realtime.so");
    out.emplace_back("libmfx50_realtime.so");
    return out;
}

template <typename Fn>
bool loadSymbol(void* library, const char* name, Fn* out, std::string* error) {
    dlerror();
    void* sym = dlsym(library, name);
    const char* err = dlerror();
    if (err || !sym) {
        if (error) *error = std::string("missing legacy symbol ") + name + ": " + (err ? err : "null");
        return false;
    }
    *out = reinterpret_cast<Fn>(sym);
    return true;
}

} // namespace

struct RealtimeBackendImpl {
    void* library = nullptr;
    LegacyHandle handle = nullptr;
    DefaultConfigFn defaultConfig = nullptr;
    CreateFn create = nullptr;
    PushPacketFn pushPacket = nullptr;
    PollPacketFn pollPacket = nullptr;
    FlushFn flush = nullptr;
    CloseFn close = nullptr;
    LastErrorFn lastError = nullptr;
    SetFrameDecisionCallbackFn setFrameDecisionCallback = nullptr;
    SetEncodeControlEventCallbackFn setEncodeControlEventCallback = nullptr;

    ~RealtimeBackendImpl() {
        if (handle && close) {
            close(handle);
            handle = nullptr;
        }
        if (library) {
            dlclose(library);
            library = nullptr;
        }
    }
};

const char* lastErrorText(const RealtimeBackendImpl& impl) {
    if (!impl.lastError) return "";
    const char* msg = impl.lastError(impl.handle);
    return msg ? msg : "";
}

bool loadLibrary(RealtimeBackendImpl* impl,
                 const MFX50RT_Config& config,
                 std::string* error) {
    std::string errors;
    for (const std::string& path : libraryCandidates(config)) {
        int flags = RTLD_NOW | RTLD_LOCAL;
#ifdef RTLD_DEEPBIND
        flags |= RTLD_DEEPBIND;
#endif
        void* lib = dlopen(path.c_str(), flags);
        if (!lib) {
            const char* err = dlerror();
            errors += path + ": " + (err ? err : "dlopen failed") + "\n";
            continue;
        }
        impl->library = lib;
        if (!loadSymbol(lib, "MFX50RT_DefaultConfig", &impl->defaultConfig, error) ||
            !loadSymbol(lib, "MFX50RT_Create", &impl->create, error) ||
            !loadSymbol(lib, "MFX50RT_PushPacket", &impl->pushPacket, error) ||
            !loadSymbol(lib, "MFX50RT_PollPacket", &impl->pollPacket, error) ||
            !loadSymbol(lib, "MFX50RT_Flush", &impl->flush, error) ||
            !loadSymbol(lib, "MFX50RT_Close", &impl->close, error) ||
            !loadSymbol(lib, "MFX50RT_GetLastError", &impl->lastError, error) ||
            !loadSymbol(lib,
                        "mfx50_realtime_set_frame_decision_callback",
                        &impl->setFrameDecisionCallback,
                        error) ||
            !loadSymbol(lib,
                        "mfx50_realtime_set_encode_control_event_callback",
                        &impl->setEncodeControlEventCallback,
                        error)) {
            dlclose(lib);
            impl->library = nullptr;
            return false;
        }
        return true;
    }

    if (error) *error = "failed to load libmfx50_realtime.so:\n" + errors;
    return false;
}

RealtimeBackend::RealtimeBackend(std::unique_ptr<RealtimeBackendImpl> impl)
    : impl_(std::move(impl)) {}

RealtimeBackend::~RealtimeBackend() = default;

std::unique_ptr<RealtimeBackend> RealtimeBackend::create(const MFX50RT_Config& config,
                                                         std::string* error) {
    auto impl = std::make_unique<RealtimeBackendImpl>();
    if (!loadLibrary(impl.get(), config, error)) return nullptr;

    LegacyConfig legacy{};
    int rc = impl->defaultConfig(&legacy);
    if (rc != kLegacyOk) {
        if (error) *error = "legacy MFX50RT_DefaultConfig failed";
        return nullptr;
    }

    std::string device = config.backend.device_name[0] ? config.backend.device_name : "auto";
    legacy.input_mode = kLegacyInputEncodedPacket;
    legacy.output_mode = kLegacyOutputPoll;
    legacy.input_codec = legacyCodec(config.pipeline.input_codec);
    legacy.output_codec = kLegacyCodecHevc;
    legacy.width = config.pipeline.width;
    legacy.height = config.pipeline.height;
    legacy.fps_num = config.pipeline.fps_num > 0 ? config.pipeline.fps_num : 25;
    legacy.fps_den = config.pipeline.fps_den > 0 ? config.pipeline.fps_den : 1;
    legacy.device_selector = device.c_str();
    legacy.profile = legacyProfile(config);
    legacy.route_count = std::max(1, config.runtime.route_count);
    legacy.async_depth = config.backend.async_depth > 0 ? config.backend.async_depth : 4;
    legacy.max_input_queue_packets = std::max(16, config.runtime.queue_depth_per_route);
    legacy.max_output_queue_packets = std::max(16, config.runtime.queue_depth_per_route);
    legacy.max_queue_surfaces = std::min(16, legacy.route_count);
    int schedulerThreads = 0;
    if (intOption(config.algo.expert_options_json, "legacy_scheduler_threads", &schedulerThreads) &&
        schedulerThreads > 0) {
        legacy.max_queue_surfaces = std::max(1, std::min(schedulerThreads, legacy.route_count));
    }
    legacy.async_mode =
        boolOptionEnabled(config.algo.expert_options_json, "legacy_async_mode") ? 1 : 0;
    int targetUsage = 0;
    if (intOption(config.algo.expert_options_json, "target_usage", &targetUsage) &&
        targetUsage > 0) {
        legacy.target_usage = targetUsage;
    }
    const bool staticReuseDisabled =
        boolOptionEnabled(config.algo.expert_options_json, "disable_static_reuse") ||
        boolOptionDisabled(config.algo.expert_options_json, "enable_static_reuse") ||
        boolOptionDisabled(config.algo.expert_options_json, "sdk_static_reuse");
    const bool staticReuseEnabled =
        boolOptionEnabled(config.algo.expert_options_json, "sdk_static_reuse") ||
        boolOptionEnabled(config.algo.expert_options_json, "enable_static_reuse") ||
        trafficSemanticStaticReuseDefault(config);
    legacy.enable_static_reuse =
        staticReuseEnabled && !staticReuseDisabled ? 1 : 0;
    if (config.pipeline.gop_size > 0) {
        legacy.gop = config.pipeline.gop_size;
    }
    if (config.pipeline.b_frames >= 0) {
        legacy.gop_ref_dist = config.pipeline.b_frames > 0 ? config.pipeline.b_frames + 1 : 1;
        legacy.bref_type = config.pipeline.b_frames > 0 ? 2 : 1;
        if (config.pipeline.b_frames == 0) {
            legacy.num_ref_frame = 0;
        }
    }

    rc = impl->create(&legacy, &impl->handle);
    if (rc != kLegacyOk || !impl->handle) {
        if (error) {
            *error = std::string("legacy MFX50RT_Create failed: ") +
                     (impl->lastError ? lastErrorText(*impl) : "");
        }
        return nullptr;
    }

    return std::unique_ptr<RealtimeBackend>(new RealtimeBackend(std::move(impl)));
}

MFX50RT_Status RealtimeBackend::pushPacket(const MFX50RT_InputPacket& packet,
                                           std::string* error) {
    if (!impl_ || !impl_->handle) return MFX50RT_ERR_INVALID_ARG;

    LegacyPacket legacy{};
    legacy.struct_size = sizeof(legacy);
    legacy.stream_id = static_cast<int>(packet.stream_id);
    legacy.data = packet.data;
    legacy.size = packet.data_size;
    legacy.pts = packet.pts;
    legacy.dts = packet.dts;
    legacy.is_keyframe = (packet.flags & MFX50RT_PACKET_FLAG_KEYFRAME) ? 1 : 0;
    legacy.end_of_stream = (packet.flags & MFX50RT_PACKET_FLAG_EOS) ? 1 : 0;
    legacy.user_opaque = packet.user_opaque;

    const int rc = impl_->pushPacket(impl_->handle, &legacy);
    const MFX50RT_Status mapped = mapStatus(rc);
    if (mapped != MFX50RT_OK && error) {
        *error = lastErrorText(*impl_);
    }
    return mapped;
}

MFX50RT_Status RealtimeBackend::flush(std::string* error) {
    if (!impl_ || !impl_->handle) return MFX50RT_ERR_INVALID_ARG;
    const int rc = impl_->flush(impl_->handle);
    const MFX50RT_Status mapped = mapStatus(rc);
    if (mapped != MFX50RT_OK && error) {
        *error = lastErrorText(*impl_);
    }
    return mapped;
}

MFX50RT_Status RealtimeBackend::pollOne(RealtimeOutputPacket* out,
                                        std::string* error) {
    if (!impl_ || !impl_->handle || !out) return MFX50RT_ERR_INVALID_ARG;
    thread_local std::vector<uint8_t> buffer;
    if (buffer.size() < 8 * 1024 * 1024) {
        buffer.resize(8 * 1024 * 1024);
    }

    for (;;) {
        LegacyEncodedPacket pkt{};
        pkt.struct_size = sizeof(pkt);
        pkt.data = buffer.data();
        pkt.capacity = buffer.size();

        const int rc = impl_->pollPacket(impl_->handle, &pkt);
        if (rc == kLegacyNoOutput || rc == kLegacyAgain) return MFX50RT_ERR_AGAIN;
        if (rc == kLegacyBufferTooSmall) {
            if (pkt.size <= buffer.size()) return MFX50RT_ERR_BUFFER_TOO_SMALL;
            buffer.resize(pkt.size);
            continue;
        }
        if (rc != kLegacyOk) {
            if (error) *error = lastErrorText(*impl_);
            return mapStatus(rc);
        }

        RealtimeOutputPacket converted;
        converted.stream_id = pkt.stream_id < 0 ? 0 : static_cast<uint32_t>(pkt.stream_id);
        converted.data.assign(buffer.data(), buffer.data() + pkt.size);
        converted.pts = pkt.pts;
        converted.dts = pkt.dts;
        converted.flags = pkt.is_keyframe ? MFX50RT_PACKET_FLAG_KEYFRAME : 0;
        converted.frame_type = pkt.frame_type;
        converted.user_opaque = pkt.user_opaque;
        *out = std::move(converted);
        return MFX50RT_OK;
    }
}

MFX50RT_Status RealtimeBackend::pollAll(std::vector<RealtimeOutputPacket>* out,
                                        std::string* error) {
    if (!impl_ || !impl_->handle || !out) return MFX50RT_ERR_INVALID_ARG;
    thread_local std::vector<uint8_t> buffer;
    if (buffer.size() < 8 * 1024 * 1024) {
        buffer.resize(8 * 1024 * 1024);
    }

    for (;;) {
        LegacyEncodedPacket pkt{};
        pkt.struct_size = sizeof(pkt);
        pkt.data = buffer.data();
        pkt.capacity = buffer.size();

        const int rc = impl_->pollPacket(impl_->handle, &pkt);
        if (rc == kLegacyNoOutput || rc == kLegacyAgain) return MFX50RT_OK;
        if (rc == kLegacyBufferTooSmall) {
            if (pkt.size <= buffer.size()) return MFX50RT_ERR_BUFFER_TOO_SMALL;
            buffer.resize(pkt.size);
            continue;
        }
        if (rc != kLegacyOk) {
            if (error) *error = lastErrorText(*impl_);
            return mapStatus(rc);
        }

        RealtimeOutputPacket converted;
        converted.stream_id = pkt.stream_id < 0 ? 0 : static_cast<uint32_t>(pkt.stream_id);
        converted.data.assign(pkt.data, pkt.data + pkt.size);
        converted.pts = pkt.pts;
        converted.dts = pkt.dts;
        converted.flags = pkt.is_keyframe ? MFX50RT_PACKET_FLAG_KEYFRAME : 0;
        converted.frame_type = pkt.frame_type;
        converted.user_opaque = pkt.user_opaque;
        out->push_back(std::move(converted));
    }
}

MFX50RT_Status RealtimeBackend::setFrameDecisionCallback(
    MFX50RT_InternalFrameDecisionCallback cb,
    void* opaque,
    std::string* error) {
    if (!impl_ || !impl_->handle || !impl_->setFrameDecisionCallback) {
        if (error) *error = "legacy realtime frame decision callback hook is unavailable";
        return MFX50RT_ERR_UNSUPPORTED;
    }
    const int rc = impl_->setFrameDecisionCallback(impl_->handle, cb, opaque);
    const MFX50RT_Status mapped = mapStatus(rc);
    if (mapped != MFX50RT_OK && error) {
        *error = lastErrorText(*impl_);
    }
    return mapped;
}

MFX50RT_Status RealtimeBackend::setEncodeControlEventCallback(
    MFX50RT_InternalEncodeControlEventCallback cb,
    void* opaque,
    std::string* error) {
    if (!impl_ || !impl_->handle || !impl_->setEncodeControlEventCallback) {
        if (error) *error = "legacy realtime encode-control event callback hook is unavailable";
        return MFX50RT_ERR_UNSUPPORTED;
    }
    const int rc = impl_->setEncodeControlEventCallback(impl_->handle, cb, opaque);
    const MFX50RT_Status mapped = mapStatus(rc);
    if (mapped != MFX50RT_OK && error) {
        *error = lastErrorText(*impl_);
    }
    return mapped;
}

} // namespace mfx50rt::onevpl
