#include "mfx50rt.h"

#include "src/algo/hybridtsrq/hybridtsrq_controller.h"
#include "src/algo/hybridtsrq/tuning_profile.h"
#include "src/backend/onevpl/onevpl_backend.h"
#include "src/backend/onevpl/onevpl_mbqp_adapter.h"
#include "src/backend/onevpl/onevpl_realtime_adapter.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using mfx50rt::hybridtsrq::HybridTSRQController;
using mfx50rt::hybridtsrq::HybridTSRQDecision;
using mfx50rt::hybridtsrq::tuning_from_policy;

namespace {

constexpr const char* kVersion = "0.6.0-hybridtsrq";

using Clock = std::chrono::steady_clock;

struct PendingPacket {
    uint32_t stream_id = 0;
    std::vector<uint8_t> data;
    int64_t pts = 0;
    int64_t dts = 0;
    uint32_t flags = 0;
    void* user_opaque = nullptr;
    Clock::time_point pushed_at = Clock::now();
};

struct OutputPacketHandle {
    std::vector<uint8_t> bytes;
    MFX50RT_OutputPacket packet{};
};

struct RouteContext {
    uint32_t stream_id = 0;
    HybridTSRQController controller;
    MFX50RT_RouteStats stats{};
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<PendingPacket> input_queue;
    std::vector<double> latencies_ms;
    std::string force_mbqp_pattern = "none";
    HybridTSRQDecision cached_decision{};
    bool cached_decision_valid = false;
    uint64_t decision_callback_frames = 0;
    uint64_t reused_mbqp_frames = 0;
    int mbqp_reuse_interval = 1;
    std::thread worker;
    bool stop = false;
    bool processing = false;
    bool quality_guard_was_on = false;
    Clock::time_point started_at = Clock::now();
};

struct MFX50RT_Context {
    MFX50RT_Config config{};
    MFX50RT_Capabilities caps{};
    MFX50RT_EffectiveConfig effective{};
    std::vector<std::unique_ptr<RouteContext>> routes;
    std::mutex output_mutex;
    std::condition_variable output_cv;
    std::deque<std::unique_ptr<OutputPacketHandle>> output_queue;
    MFX50RT_OutputCallback output_cb = nullptr;
    void* output_cb_opaque = nullptr;
    MFX50RT_EventCallback event_cb = nullptr;
    void* event_cb_opaque = nullptr;
    std::unique_ptr<mfx50rt::onevpl::RealtimeBackend> realtime_backend;
    bool closed = false;
    Clock::time_point started_at = Clock::now();
};

void copy_cstr(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) return;
    std::snprintf(dst, dst_size, "%s", src ? src : "");
}

template <typename T>
void init_public_struct(T& value) {
    std::memset(&value, 0, sizeof(T));
    value.size = sizeof(T);
    value.version = MFX50RT_API_VERSION;
}

bool has_valid_header(uint32_t size, uint32_t version, size_t expected) {
    return size >= expected && version == MFX50RT_API_VERSION;
}

MFX50RT_Status validate_config(const MFX50RT_Config* cfg) {
    if (!cfg || !has_valid_header(cfg->size, cfg->version, sizeof(MFX50RT_Config))) {
        return MFX50RT_ERR_INVALID_ARG;
    }
    if (!has_valid_header(cfg->backend.size, cfg->backend.version, sizeof(MFX50RT_BackendConfig)) ||
        !has_valid_header(cfg->pipeline.size, cfg->pipeline.version, sizeof(MFX50RT_PipelineConfig)) ||
        !has_valid_header(cfg->algo.size, cfg->algo.version, sizeof(MFX50RT_AlgoPolicy)) ||
        !has_valid_header(cfg->runtime.size, cfg->runtime.version, sizeof(MFX50RT_RuntimeConfig))) {
        return MFX50RT_ERR_INVALID_ARG;
    }
    return MFX50RT_OK;
}

void normalize_config(MFX50RT_Config& cfg) {
    cfg.size = sizeof(cfg);
    cfg.version = MFX50RT_API_VERSION;
    cfg.backend.size = sizeof(cfg.backend);
    cfg.backend.version = MFX50RT_API_VERSION;
    cfg.pipeline.size = sizeof(cfg.pipeline);
    cfg.pipeline.version = MFX50RT_API_VERSION;
    cfg.algo.size = sizeof(cfg.algo);
    cfg.algo.version = MFX50RT_API_VERSION;
    cfg.runtime.size = sizeof(cfg.runtime);
    cfg.runtime.version = MFX50RT_API_VERSION;

    if (cfg.backend.async_depth <= 0) cfg.backend.async_depth = 4;
    if (cfg.pipeline.output_codec == MFX50RT_CODEC_AUTO) cfg.pipeline.output_codec = MFX50RT_CODEC_HEVC;
    if (cfg.pipeline.gop_size <= 0) cfg.pipeline.gop_size = 60;
    if (cfg.pipeline.idr_interval <= 0) cfg.pipeline.idr_interval = 120;
    if (cfg.pipeline.fps_den < 0) cfg.pipeline.fps_den = 0;
    if (cfg.pipeline.fps_num < 0) cfg.pipeline.fps_num = 0;
    if (cfg.algo.target_compression_percent <= 0) cfg.algo.target_compression_percent = 90;
    if (cfg.algo.avg_ssim_target <= 0.0f) cfg.algo.avg_ssim_target = 0.90f;
    if (cfg.algo.min_ssim_guard <= 0.0f) cfg.algo.min_ssim_guard = 0.82f;
    if (cfg.algo.heavy_analyze_interval <= 0) cfg.algo.heavy_analyze_interval = 5;
    if (cfg.algo.quality_guard_hold_frames <= 0) cfg.algo.quality_guard_hold_frames = 60;
    if (cfg.runtime.route_count <= 0) cfg.runtime.route_count = 1;
    if (cfg.runtime.worker_threads <= 0) cfg.runtime.worker_threads = cfg.runtime.route_count;
    if (cfg.runtime.queue_depth_per_route <= 0) cfg.runtime.queue_depth_per_route = 8;
}

void set_defaults(MFX50RT_Config* cfg) {
    init_public_struct(*cfg);
    init_public_struct(cfg->backend);
    init_public_struct(cfg->pipeline);
    init_public_struct(cfg->algo);
    init_public_struct(cfg->runtime);

    cfg->backend.type = MFX50RT_BACKEND_AUTO;
    cfg->backend.device_index = 0;
    cfg->backend.prefer_hw_decode = 1;
    cfg->backend.prefer_hw_encode = 1;
    cfg->backend.prefer_video_memory = 1;
    cfg->backend.prefer_zero_copy = 1;
    cfg->backend.async_depth = 4;
    cfg->backend.low_latency = 1;

    cfg->pipeline.input_codec = MFX50RT_CODEC_AUTO;
    cfg->pipeline.output_codec = MFX50RT_CODEC_HEVC;
    cfg->pipeline.gop_size = 60;
    cfg->pipeline.idr_interval = 120;
    cfg->pipeline.b_frames = 0;
    cfg->pipeline.rc_mode = MFX50RT_RC_AUTO;
    cfg->pipeline.low_latency = 1;
    cfg->pipeline.annexb_output = 1;

    cfg->algo.profile = MFX50RT_PROFILE_TARGET_90_SSIM_GUARD;
    cfg->algo.strategy = MFX50RT_STRATEGY_AUTO;
    cfg->algo.target_compression_percent = 90;
    cfg->algo.avg_ssim_target = 0.90f;
    cfg->algo.min_ssim_guard = 0.82f;
    cfg->algo.enable_temporal_qp = 1;
    cfg->algo.enable_adaptive_gop = 1;
    cfg->algo.enable_scene_cut_idr = 1;
    cfg->algo.enable_b_frame_policy = 1;
    cfg->algo.enable_spatial_qp = 1;
    cfg->algo.enable_roi = 1;
    cfg->algo.enable_mbqp = 1;
    cfg->algo.enable_roi_delta_qp = 1;
    cfg->algo.enable_fast_analyzer = 1;
    cfg->algo.enable_heavy_analyzer = 1;
    cfg->algo.heavy_analyze_interval = 5;
    cfg->algo.enable_preprocess = 1;
    cfg->algo.preprocess_mode = MFX50RT_PREPROCESS_AUTO;
    cfg->algo.enable_quality_guard = 1;
    cfg->algo.quality_guard_hold_frames = 60;

    cfg->runtime.route_count = 1;
    cfg->runtime.worker_threads = 1;
    cfg->runtime.queue_depth_per_route = 8;
    cfg->runtime.async_mode = 1;
    cfg->runtime.enable_callback = 0;
    cfg->runtime.log_level = 2;
}

std::string read_file(const char* path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string section_for(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find('{', pos);
    if (pos == std::string::npos) return {};
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = pos; i < json.size(); ++i) {
        char c = json[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) continue;
        if (c == '{') depth++;
        if (c == '}') {
            depth--;
            if (depth == 0) return json.substr(pos, i - pos + 1);
        }
    }
    return {};
}

bool json_string(const std::string& s, const char* key, std::string* out) {
    std::regex re(std::string("\"") + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch m;
    if (!std::regex_search(s, m, re)) return false;
    *out = m[1].str();
    return true;
}

bool json_int(const std::string& s, const char* key, int* out) {
    std::regex re(std::string("\"") + key + "\"\\s*:\\s*(-?[0-9]+)");
    std::smatch m;
    if (!std::regex_search(s, m, re)) return false;
    *out = std::stoi(m[1].str());
    return true;
}

bool json_float(const std::string& s, const char* key, float* out) {
    std::regex re(std::string("\"") + key + "\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)");
    std::smatch m;
    if (!std::regex_search(s, m, re)) return false;
    *out = std::stof(m[1].str());
    return true;
}

bool json_boolish(const std::string& s, const char* key, int* out) {
    std::regex re(std::string("\"") + key + "\"\\s*:\\s*(true|false|\"auto\"|\"on\"|\"off\")",
                  std::regex::icase);
    std::smatch m;
    if (!std::regex_search(s, m, re)) return false;
    const std::string v = lower(m[1].str());
    *out = (v == "true" || v == "\"auto\"" || v == "\"on\"") ? 1 : 0;
    return true;
}

MFX50RT_BackendType backend_from_string(const std::string& v) {
    const std::string s = lower(v);
    if (s == "onevpl") return MFX50RT_BACKEND_ONEVPL;
    if (s == "ffmpeg") return MFX50RT_BACKEND_FFMPEG;
    if (s == "vaapi") return MFX50RT_BACKEND_VAAPI;
    if (s == "nvenc") return MFX50RT_BACKEND_NVENC;
    if (s == "cpu") return MFX50RT_BACKEND_CPU;
    if (s == "null") return MFX50RT_BACKEND_NULL;
    return MFX50RT_BACKEND_AUTO;
}

MFX50RT_Codec codec_from_string(const std::string& v) {
    const std::string s = lower(v);
    if (s == "h264") return MFX50RT_CODEC_H264;
    if (s == "hevc" || s == "h265") return MFX50RT_CODEC_HEVC;
    if (s == "av1") return MFX50RT_CODEC_AV1;
    return MFX50RT_CODEC_AUTO;
}

MFX50RT_RateControl rc_from_string(const std::string& v) {
    const std::string s = lower(v);
    if (s == "cqp") return MFX50RT_RC_CQP;
    if (s == "qvbr") return MFX50RT_RC_QVBR;
    if (s == "icq") return MFX50RT_RC_ICQ;
    if (s == "vbr") return MFX50RT_RC_VBR;
    if (s == "cbr") return MFX50RT_RC_CBR;
    return MFX50RT_RC_AUTO;
}

MFX50RT_AlgoProfile profile_from_string(const std::string& v) {
    const std::string s = lower(v);
    if (s == "safe") return MFX50RT_PROFILE_SAFE;
    if (s == "balanced") return MFX50RT_PROFILE_BALANCED;
    if (s == "max_compression") return MFX50RT_PROFILE_MAX_COMPRESSION;
    if (s == "low_latency") return MFX50RT_PROFILE_LOW_LATENCY;
    if (s == "custom") return MFX50RT_PROFILE_CUSTOM;
    return MFX50RT_PROFILE_TARGET_90_SSIM_GUARD;
}

MFX50RT_ControlStrategy strategy_from_string(const std::string& v) {
    const std::string s = lower(v);
    if (s == "mbqp_cqp") return MFX50RT_STRATEGY_MBQP_CQP;
    if (s == "roi_delta_qp") return MFX50RT_STRATEGY_ROI_DELTA_QP;
    if (s == "global") return MFX50RT_STRATEGY_GLOBAL;
    return MFX50RT_STRATEGY_AUTO;
}

MFX50RT_PreprocessMode preprocess_from_string(const std::string& v) {
    const std::string s = lower(v);
    if (s == "off") return MFX50RT_PREPROCESS_OFF;
    if (s == "light") return MFX50RT_PREPROCESS_LIGHT;
    if (s == "strong") return MFX50RT_PREPROCESS_STRONG;
    return MFX50RT_PREPROCESS_AUTO;
}

MFX50RT_Status parse_json_config(const std::string& json, MFX50RT_Config* cfg) {
    if (json.empty() || !cfg) return MFX50RT_ERR_INVALID_ARG;
    set_defaults(cfg);

    const std::string backend = section_for(json, "backend");
    const std::string pipeline = section_for(json, "pipeline");
    const std::string algorithm = section_for(json, "algorithm");
    const std::string debug = section_for(json, "debug");
    const std::string runtime = section_for(json, "runtime");
    std::string s;
    int i = 0;
    float f = 0.0f;

    if (json_string(backend, "type", &s)) cfg->backend.type = backend_from_string(s);
    if (json_int(backend, "device_index", &i)) cfg->backend.device_index = i;
    if (json_boolish(backend, "prefer_hw_decode", &i)) cfg->backend.prefer_hw_decode = i;
    if (json_boolish(backend, "prefer_hw_encode", &i)) cfg->backend.prefer_hw_encode = i;
    if (json_boolish(backend, "prefer_video_memory", &i)) cfg->backend.prefer_video_memory = i;
    if (json_boolish(backend, "prefer_zero_copy", &i)) cfg->backend.prefer_zero_copy = i;
    if (json_int(backend, "async_depth", &i)) cfg->backend.async_depth = i;
    if (json_boolish(backend, "low_latency", &i)) cfg->backend.low_latency = i;
    if (!backend.empty()) copy_cstr(cfg->backend.backend_options_json,
                                    sizeof(cfg->backend.backend_options_json),
                                    backend.c_str());

    if (json_string(pipeline, "input_codec", &s)) cfg->pipeline.input_codec = codec_from_string(s);
    if (json_string(pipeline, "output_codec", &s)) cfg->pipeline.output_codec = codec_from_string(s);
    if (json_int(pipeline, "width", &i)) cfg->pipeline.width = i;
    if (json_int(pipeline, "height", &i)) cfg->pipeline.height = i;
    if (json_int(pipeline, "gop_size", &i)) cfg->pipeline.gop_size = i;
    if (json_int(pipeline, "idr_interval", &i)) cfg->pipeline.idr_interval = i;
    if (json_int(pipeline, "b_frames", &i)) cfg->pipeline.b_frames = i;
    if (json_string(pipeline, "rate_control", &s)) cfg->pipeline.rc_mode = rc_from_string(s);
    if (json_int(pipeline, "target_bitrate_kbps", &i)) cfg->pipeline.target_bitrate_kbps = i;
    if (json_int(pipeline, "max_bitrate_kbps", &i)) cfg->pipeline.max_bitrate_kbps = i;
    if (json_boolish(pipeline, "annexb_output", &i)) cfg->pipeline.annexb_output = i;

    if (json_string(algorithm, "profile", &s)) cfg->algo.profile = profile_from_string(s);
    if (json_string(algorithm, "strategy", &s)) cfg->algo.strategy = strategy_from_string(s);
    if (json_int(algorithm, "target_compression_percent", &i)) cfg->algo.target_compression_percent = i;
    if (json_float(algorithm, "avg_ssim_target", &f)) cfg->algo.avg_ssim_target = f;
    if (json_float(algorithm, "min_ssim_guard", &f)) cfg->algo.min_ssim_guard = f;
    if (json_boolish(algorithm, "temporal_qp", &i)) cfg->algo.enable_temporal_qp = i;
    if (json_boolish(algorithm, "adaptive_gop", &i)) cfg->algo.enable_adaptive_gop = i;
    if (json_boolish(algorithm, "scene_cut_idr", &i)) cfg->algo.enable_scene_cut_idr = i;
    if (json_boolish(algorithm, "spatial_qp", &i)) cfg->algo.enable_spatial_qp = i;
    if (json_boolish(algorithm, "roi", &i)) cfg->algo.enable_roi = i;
    if (json_boolish(algorithm, "mbqp", &i)) cfg->algo.enable_mbqp = i;
    if (json_boolish(algorithm, "roi_delta_qp", &i)) cfg->algo.enable_roi_delta_qp = i;
    if (json_boolish(algorithm, "fast_analyzer", &i)) cfg->algo.enable_fast_analyzer = i;
    if (json_boolish(algorithm, "heavy_analyzer", &i)) cfg->algo.enable_heavy_analyzer = i;
    if (json_int(algorithm, "heavy_analyze_interval", &i)) cfg->algo.heavy_analyze_interval = i;
    if (json_string(algorithm, "preprocess", &s)) {
        cfg->algo.preprocess_mode = preprocess_from_string(s);
        cfg->algo.enable_preprocess = cfg->algo.preprocess_mode != MFX50RT_PREPROCESS_OFF;
    }
    if (json_boolish(algorithm, "quality_guard", &i)) cfg->algo.enable_quality_guard = i;
    if (json_int(algorithm, "quality_guard_hold_frames", &i)) cfg->algo.quality_guard_hold_frames = i;
    if (!algorithm.empty() || !debug.empty()) {
        const std::string expert = algorithm + debug;
        copy_cstr(cfg->algo.expert_options_json,
                  sizeof(cfg->algo.expert_options_json),
                  expert.c_str());
    }

    if (json_int(runtime, "route_count", &i)) cfg->runtime.route_count = i;
    if (json_int(runtime, "worker_threads", &i)) cfg->runtime.worker_threads = i;
    if (json_int(runtime, "queue_depth_per_route", &i)) cfg->runtime.queue_depth_per_route = i;
    if (json_boolish(runtime, "async_mode", &i)) cfg->runtime.async_mode = i;

    normalize_config(*cfg);
    return MFX50RT_OK;
}

const char* strategy_name(MFX50RT_ControlStrategy s) {
    switch (s) {
        case MFX50RT_STRATEGY_MBQP_CQP: return "MBQP_CQP";
        case MFX50RT_STRATEGY_ROI_DELTA_QP: return "ROI_DELTA_QP";
        case MFX50RT_STRATEGY_GLOBAL: return "GLOBAL";
        default: return "AUTO";
    }
}

const char* actual_control_name(MFX50RT_InternalControlStrategy s) {
    switch (s) {
        case MFX50RT_INTERNAL_CONTROL_MBQP: return "MBQP";
        case MFX50RT_INTERNAL_CONTROL_ROI: return "ROI";
        default: return "GLOBAL";
    }
}

MFX50RT_InternalControlStrategy internal_strategy_from_public(MFX50RT_ControlStrategy s) {
    if (s == MFX50RT_STRATEGY_MBQP_CQP) return MFX50RT_INTERNAL_CONTROL_MBQP;
    if (s == MFX50RT_STRATEGY_ROI_DELTA_QP) return MFX50RT_INTERNAL_CONTROL_ROI;
    return MFX50RT_INTERNAL_CONTROL_GLOBAL;
}

std::string force_mbqp_pattern_from_config(const MFX50RT_Config& cfg) {
    std::string pattern;
    if (json_string(cfg.algo.expert_options_json, "force_mbqp_pattern", &pattern)) {
        pattern = lower(pattern);
        if (!pattern.empty()) return pattern;
    }
    return "none";
}

MFX50RT_EffectiveConfig make_effective(const MFX50RT_Config& cfg,
                                       const MFX50RT_Capabilities& caps) {
    MFX50RT_EffectiveConfig out{};
    init_public_struct(out);
    out.requested_backend = cfg.backend.type;
    out.effective_backend = cfg.backend.type == MFX50RT_BACKEND_AUTO
        ? (caps.supports_hw_encode ? MFX50RT_BACKEND_ONEVPL : MFX50RT_BACKEND_NULL)
        : cfg.backend.type;
    out.requested_strategy = cfg.algo.strategy;
    out.requested_rc_mode = cfg.pipeline.rc_mode;
    out.effective_rc_mode = cfg.pipeline.rc_mode == MFX50RT_RC_AUTO ? MFX50RT_RC_CQP : cfg.pipeline.rc_mode;
    out.temporal_qp_enabled = cfg.algo.enable_temporal_qp;
    out.preprocess_enabled = cfg.algo.enable_preprocess;
    out.quality_guard_enabled = cfg.algo.enable_quality_guard;

    const bool wants_mbqp = cfg.algo.enable_mbqp != 0;
    const bool wants_roi = cfg.algo.enable_roi_delta_qp != 0 || cfg.algo.enable_roi != 0;

    auto select_auto = [&]() {
        if (wants_mbqp && caps.supports_mbqp) return MFX50RT_STRATEGY_MBQP_CQP;
        if (wants_roi && caps.supports_roi_delta_qp) return MFX50RT_STRATEGY_ROI_DELTA_QP;
        return MFX50RT_STRATEGY_GLOBAL;
    };

    if (cfg.algo.strategy == MFX50RT_STRATEGY_AUTO) {
        out.effective_strategy = select_auto();
        if (out.effective_strategy == MFX50RT_STRATEGY_GLOBAL) {
            copy_cstr(out.fallback_reason,
                      sizeof(out.fallback_reason),
                      "AUTO selected GLOBAL because verified MBQP/ROI capabilities are unavailable");
        }
    } else if (cfg.algo.strategy == MFX50RT_STRATEGY_MBQP_CQP) {
        if (caps.supports_mbqp && wants_mbqp) {
            out.effective_strategy = MFX50RT_STRATEGY_MBQP_CQP;
        } else if (caps.supports_roi_delta_qp && wants_roi) {
            out.effective_strategy = MFX50RT_STRATEGY_ROI_DELTA_QP;
            copy_cstr(out.fallback_reason,
                      sizeof(out.fallback_reason),
                      "requested MBQP_CQP but verified backend capability does not include MBQP; using ROI_DELTA_QP");
        } else {
            out.effective_strategy = MFX50RT_STRATEGY_GLOBAL;
            copy_cstr(out.fallback_reason,
                      sizeof(out.fallback_reason),
                      "requested MBQP_CQP but verified backend capability does not include MBQP or ROI DeltaQP; using GLOBAL");
        }
    } else if (cfg.algo.strategy == MFX50RT_STRATEGY_ROI_DELTA_QP) {
        if (caps.supports_roi_delta_qp && wants_roi) {
            out.effective_strategy = MFX50RT_STRATEGY_ROI_DELTA_QP;
        } else {
            out.effective_strategy = MFX50RT_STRATEGY_GLOBAL;
            copy_cstr(out.fallback_reason,
                      sizeof(out.fallback_reason),
                      "requested ROI_DELTA_QP but verified backend capability does not include ROI DeltaQP; using GLOBAL");
        }
    } else {
        out.effective_strategy = MFX50RT_STRATEGY_GLOBAL;
    }

    out.spatial_qp_enabled = cfg.algo.enable_spatial_qp;
    out.mbqp_enabled = out.effective_strategy == MFX50RT_STRATEGY_MBQP_CQP;
    out.roi_delta_qp_enabled = out.effective_strategy == MFX50RT_STRATEGY_ROI_DELTA_QP;
    const auto tuning = tuning_from_policy(cfg.algo);
    std::snprintf(out.effective_options_json,
                  sizeof(out.effective_options_json),
                  "{\"algorithm\":\"HybridTSRQ\",\"strategy\":\"%s\","
                  "\"hybridtsrq_profile\":\"%s\","
                  "\"capability_policy\":\"hardware-probed\","
                  "\"onevpl_backend\":\"mfx50_realtime_adapter\"}",
                  strategy_name(out.effective_strategy),
                  tuning.profile_name.c_str());
    return out;
}

MFX50RT_Context* as_ctx(MFX50RT_Handle h) {
    return reinterpret_cast<MFX50RT_Context*>(h);
}

RouteContext* get_route(MFX50RT_Context* ctx, uint32_t stream_id) {
    if (!ctx || stream_id >= ctx->routes.size()) return nullptr;
    return ctx->routes[stream_id].get();
}

int frame_type_from_decision(const HybridTSRQDecision& decision) {
    if (decision.frame_id == 0 || decision.temporal.force_idr) return 1;
    if (decision.temporal.frame_type_delta == decision.temporal.qpb - decision.temporal.base_scene_qp) return 3;
    return 2;
}

void emit_event(MFX50RT_Context* ctx,
                MFX50RT_EventType type,
                uint32_t stream_id,
                int32_t code,
                const char* message,
                const char* detail_json = "{}") {
    if (!ctx || !ctx->event_cb) return;
    MFX50RT_Event ev{};
    init_public_struct(ev);
    ev.type = type;
    ev.stream_id = stream_id;
    ev.code = code;
    ev.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    copy_cstr(ev.message, sizeof(ev.message), message);
    copy_cstr(ev.detail_json, sizeof(ev.detail_json), detail_json);
    ctx->event_cb(&ev, ctx->event_cb_opaque);
}

void enqueue_output(MFX50RT_Context* ctx, std::unique_ptr<OutputPacketHandle> handle) {
    handle->packet.data = handle->bytes.empty() ? nullptr : handle->bytes.data();
    handle->packet.packet_handle = handle.get();
    if (ctx->output_cb) ctx->output_cb(&handle->packet, ctx->output_cb_opaque);
    {
        std::lock_guard<std::mutex> lock(ctx->output_mutex);
        ctx->output_queue.push_back(std::move(handle));
    }
    ctx->output_cv.notify_one();
}

double elapsed_seconds(Clock::time_point start) {
    const auto dt = Clock::now() - start;
    const double sec = std::chrono::duration<double>(dt).count();
    return std::max(0.001, sec);
}

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t idx = std::min(values.size() - 1,
                                static_cast<size_t>((values.size() - 1) * p));
    return values[idx];
}

void refresh_route_stats(RouteContext& route) {
    const double sec = elapsed_seconds(route.started_at);
    route.stats.fps_in = route.stats.input_packets / sec;
    route.stats.fps_out = route.stats.output_packets / sec;
    route.stats.compression_ratio_avg = route.stats.input_bytes > 0
        ? 1.0 - static_cast<double>(route.stats.output_bytes) / route.stats.input_bytes
        : 0.0;
    if (!route.latencies_ms.empty()) {
        double sum = 0.0;
        for (double v : route.latencies_ms) sum += v;
        route.stats.latency_ms_avg = sum / route.latencies_ms.size();
        route.stats.latency_ms_p50 = percentile(route.latencies_ms, 0.50);
        route.stats.latency_ms_p95 = percentile(route.latencies_ms, 0.95);
        route.stats.latency_ms_p99 = percentile(route.latencies_ms, 0.99);
    }
}

void copy_decision_qp_stats(MFX50RT_RouteStats& stats,
                            const HybridTSRQDecision& decision) {
    stats.qp_avg = decision.spatial.spatial_avg_qp > 0
        ? decision.spatial.spatial_avg_qp
        : decision.temporal.frame_anchor_qp;
    stats.qp_min = decision.spatial.spatial_min_qp > 0
        ? decision.spatial.spatial_min_qp
        : decision.temporal.frame_anchor_qp;
    stats.qp_max = decision.spatial.spatial_max_qp > 0
        ? decision.spatial.spatial_max_qp
        : decision.temporal.frame_anchor_qp;
    stats.qp_p10 = decision.spatial.qp_stats.qp_p10;
    stats.qp_p50 = decision.spatial.qp_stats.qp_p50;
    stats.qp_p90 = decision.spatial.qp_stats.qp_p90;
    stats.qp_p95 = decision.spatial.qp_stats.qp_p95;
    stats.low_importance_block_ratio =
        decision.spatial.region_stats.low_importance_block_ratio;
    stats.high_qp_block_ratio =
        decision.spatial.qp_stats.high_qp_block_ratio;
    stats.roi_block_ratio =
        decision.spatial.region_stats.roi_block_ratio;
    stats.background_block_ratio =
        decision.spatial.region_stats.background_block_ratio;
    stats.flat_background_block_ratio =
        decision.spatial.region_stats.flat_background_block_ratio;
    stats.foreground_block_ratio =
        decision.spatial.region_stats.foreground_block_ratio;
    stats.edge_block_ratio =
        decision.spatial.region_stats.edge_block_ratio;
    stats.texture_block_ratio =
        decision.spatial.region_stats.texture_block_ratio;
    stats.transition_block_ratio =
        decision.spatial.region_stats.transition_block_ratio;
    stats.true_roi_block_ratio =
        decision.spatial.region_stats.true_roi_block_ratio;
    stats.edge_texture_roi_block_ratio =
        decision.spatial.region_stats.edge_texture_roi_block_ratio;
    stats.high_texture_background_block_ratio =
        decision.spatial.region_stats.high_texture_background_block_ratio;
    stats.hard_scene_background_block_ratio =
        decision.spatial.region_stats.hard_scene_background_block_ratio;
    stats.normal_background_block_ratio =
        decision.spatial.region_stats.normal_background_block_ratio;
    stats.smoothing_changed_qp_avg = decision.spatial.smoothing_changed_qp_avg;
}

void record_output_stats(RouteContext& route,
                         const MFX50RT_OutputPacket& packet,
                         const HybridTSRQDecision* decision,
                         Clock::time_point pushed_at) {
    std::lock_guard<std::mutex> lock(route.mutex);
    route.stats.output_packets++;
    route.stats.output_bytes += packet.data_size;
    route.stats.encoded_frames++;
    route.stats.decoded_frames++;
    if (packet.flags & MFX50RT_PACKET_FLAG_KEYFRAME) route.stats.force_idr_count++;
    if (decision) {
        if (decision->strategy == MFX50RT_STRATEGY_MBQP_CQP) route.stats.mbqp_frames++;
        else if (decision->strategy == MFX50RT_STRATEGY_ROI_DELTA_QP) route.stats.roi_frames++;
        else route.stats.global_frames++;
        copy_decision_qp_stats(route.stats, *decision);
        route.stats.avg_ssim = route.controller.qualityState().avg_ssim;
        route.stats.min_ssim = route.controller.qualityState().min_ssim;
        route.stats.p5_ssim = route.controller.qualityState().p5_ssim;
    }
    route.latencies_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - pushed_at).count());
    if (route.latencies_ms.size() > 512) route.latencies_ms.erase(route.latencies_ms.begin());
    refresh_route_stats(route);
}

int hybrid_frame_decision_callback(void* opaque,
                                   const MFX50RT_InternalSurfaceView* surface,
                                   MFX50RT_InternalEncodeDecision* out) {
    auto* ctx = static_cast<MFX50RT_Context*>(opaque);
    if (!ctx || !surface || !out ||
        surface->version != MFX50RT_INTERNAL_API_VERSION ||
        out->version != MFX50RT_INTERNAL_API_VERSION) {
        return -1;
    }
    RouteContext* route = get_route(ctx, surface->stream_id);
    if (!route) return -1;

    uint8_t* qp_buffer = out->mbqp_qp_buffer;
    const uint32_t qp_capacity = out->mbqp_qp_capacity;
    const int preset_cols = out->mbqp_block_cols;
    const int preset_rows = out->mbqp_block_rows;
    const int preset_pitch = out->mbqp_pitch;
    const int preset_block_size = out->mbqp_block_size;
    std::memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);
    out->version = MFX50RT_INTERNAL_API_VERSION;
    out->strategy = MFX50RT_INTERNAL_CONTROL_GLOBAL;
    out->mbqp_qp_buffer = qp_buffer;
    out->mbqp_qp_capacity = qp_capacity;
    out->mbqp_block_cols = preset_cols;
    out->mbqp_block_rows = preset_rows;
    out->mbqp_pitch = preset_pitch;
    out->mbqp_block_size = preset_block_size > 0 ? preset_block_size : 16;

    HybridTSRQDecision decision;
    bool reused_decision = false;
    {
        std::lock_guard<std::mutex> lock(route->mutex);
        const uint64_t frame_index = route->decision_callback_frames++;
        if (route->mbqp_reuse_interval > 1 &&
            route->cached_decision_valid &&
            (frame_index % static_cast<uint64_t>(route->mbqp_reuse_interval)) != 0 &&
            route->cached_decision.spatial.has_mbqp) {
            decision = route->cached_decision;
            decision.temporal.force_idr = false;
            reused_decision = true;
            route->reused_mbqp_frames++;
        }
    }
    if (!reused_decision) {
        decision = route->controller.decideFromYPlane(surface->y_ptr,
                                                     surface->width,
                                                     surface->height,
                                                     surface->y_pitch,
                                                     surface->pts);
        if (decision.spatial.has_mbqp) {
            std::lock_guard<std::mutex> lock(route->mutex);
            route->cached_decision = decision;
            route->cached_decision_valid = true;
        }
    }
    out->strategy = internal_strategy_from_public(decision.strategy);
    out->force_idr = decision.temporal.force_idr ? 1 : 0;
    out->qpi = decision.temporal.qpi;
    out->qpp = decision.temporal.qpp;
    out->qpb = decision.temporal.qpb;
    out->base_scene_qp = decision.temporal.base_scene_qp;
    out->frame_anchor_qp = decision.temporal.frame_anchor_qp;
    out->foreground_ratio = route->controller.lastRoi().foreground_ratio;
    out->edge_density = route->controller.lastFastFeatures().edge_density;
    out->motion_score = route->controller.lastFastFeatures().motion_score;
    out->noise_score = route->controller.lastFastFeatures().noise_score;
    out->scene_cut_score = route->controller.lastFastFeatures().scene_cut_score;
    out->quality_guard_state = decision.quality_guard_on ? 1 : 0;
    out->static_reuse_candidate = decision.static_reuse_candidate ? 1 : 0;
    out->static_reuse_consecutive_frames = decision.static_reuse_consecutive_frames;
    out->static_reuse_risk_score = decision.static_reuse_risk_score;

    bool filled = false;
    std::string error;
    if (route->force_mbqp_pattern != "none") {
        filled = mfx50rt::onevpl::fillInternalForceMbqpPattern(route->force_mbqp_pattern.c_str(),
                                                               surface->width,
                                                               surface->height,
                                                               decision.temporal.frame_anchor_qp,
                                                               out,
                                                               &error);
    } else if (decision.strategy == MFX50RT_STRATEGY_MBQP_CQP &&
               decision.spatial.has_mbqp) {
        filled = mfx50rt::onevpl::fillInternalMbqpDecisionFromMap(decision.spatial.mbqp_map,
                                                                  decision.temporal.frame_anchor_qp,
                                                                  out,
                                                                  &error);
    }

    {
        std::lock_guard<std::mutex> lock(route->mutex);
        if (filled) {
            route->stats.mbqp_frames++;
            copy_decision_qp_stats(route->stats, decision);
        } else {
            route->stats.global_frames++;
            copy_cstr(route->stats.last_warning,
                      sizeof(route->stats.last_warning),
                      error.empty() ? "HybridTSRQ did not produce MBQP" : error.c_str());
        }
        route->stats.avg_ssim = route->controller.qualityState().avg_ssim;
        route->stats.min_ssim = route->controller.qualityState().min_ssim;
        route->stats.p5_ssim = route->controller.qualityState().p5_ssim;
    }

    if (!filled) {
        out->strategy = MFX50RT_INTERNAL_CONTROL_GLOBAL;
        out->has_mbqp = 0;
        copy_cstr(out->reason,
                  sizeof(out->reason),
                  error.empty() ? "HybridTSRQ did not produce MBQP, fallback to GLOBAL" : error.c_str());
    }
    return 0;
}

void hybrid_encode_control_event_callback(void* opaque,
                                          const MFX50RT_InternalEncodeControlEvent* event) {
    auto* ctx = static_cast<MFX50RT_Context*>(opaque);
    if (!ctx || !event || event->version != MFX50RT_INTERNAL_API_VERSION) return;
    RouteContext* route = get_route(ctx, event->stream_id);
    if (!route) return;

    const char* actual = actual_control_name(event->actual_strategy);
    {
        std::lock_guard<std::mutex> lock(route->mutex);
        route->stats.mbqp_init_enabled = event->mbqp_init_enabled;
        copy_cstr(route->stats.actual_encode_control,
                  sizeof(route->stats.actual_encode_control),
                  actual);
        if (event->mbqp_attached) {
            route->stats.mbqp_applied_frames++;
            route->stats.qp_avg = event->qp_avg;
            route->stats.qp_min = event->qp_min;
            route->stats.qp_max = event->qp_max;
        } else {
            route->stats.mbqp_skipped_frames++;
            route->stats.mbqp_fallback_count++;
            route->stats.fallback_count++;
            copy_cstr(route->stats.last_warning,
                      sizeof(route->stats.last_warning),
                      event->reason);
        }
        refresh_route_stats(*route);
    }
    route->controller.recordEncodeControlResult(static_cast<int64_t>(event->frame_id),
                                                event->mbqp_attached,
                                                actual,
                                                event->reason);
}

MFX50RT_Status drain_realtime_outputs(MFX50RT_Context* ctx,
                                      const HybridTSRQDecision* decision,
                                      Clock::time_point pushed_at,
                                      std::string* error) {
    if (!ctx || !ctx->realtime_backend) return MFX50RT_ERR_INVALID_ARG;
    std::vector<mfx50rt::onevpl::RealtimeOutputPacket> packets;
    MFX50RT_Status st = ctx->realtime_backend->pollAll(&packets, error);
    if (st != MFX50RT_OK) return st;
    for (const auto& pkt : packets) {
        auto out = std::make_unique<OutputPacketHandle>();
        init_public_struct(out->packet);
        out->bytes = pkt.data;
        out->packet.stream_id = pkt.stream_id;
        out->packet.data_size = static_cast<uint32_t>(out->bytes.size());
        out->packet.pts = pkt.pts;
        out->packet.dts = pkt.dts;
        out->packet.flags = pkt.flags;
        out->packet.codec = MFX50RT_CODEC_HEVC;
        out->packet.frame_type = pkt.frame_type;
        if (decision) {
            out->packet.qp_avg = decision->spatial.spatial_avg_qp > 0
                ? decision->spatial.spatial_avg_qp
                : decision->temporal.frame_anchor_qp;
            out->packet.qp_min = decision->spatial.spatial_min_qp > 0
                ? decision->spatial.spatial_min_qp
                : decision->temporal.frame_anchor_qp;
            out->packet.qp_max = decision->spatial.spatial_max_qp > 0
                ? decision->spatial.spatial_max_qp
                : decision->temporal.frame_anchor_qp;
        }
        out->packet.user_opaque = pkt.user_opaque;
        if (RouteContext* out_route = get_route(ctx, pkt.stream_id)) {
            if (!decision) {
                std::lock_guard<std::mutex> lock(out_route->mutex);
                out->packet.qp_avg = out_route->stats.qp_avg;
                out->packet.qp_min = out_route->stats.qp_min;
                out->packet.qp_max = out_route->stats.qp_max;
            }
            record_output_stats(*out_route, out->packet, decision, pushed_at);
        }
        enqueue_output(ctx, std::move(out));
    }
    return MFX50RT_OK;
}

MFX50RT_Status process_packet(MFX50RT_Context* ctx, RouteContext* route, const PendingPacket& in) {
    if (!ctx || !route) return MFX50RT_ERR_INVALID_ARG;
    if (ctx->realtime_backend) {
        MFX50RT_InputPacket pkt{};
        init_public_struct(pkt);
        pkt.stream_id = in.stream_id;
        pkt.data = in.data.empty() ? nullptr : in.data.data();
        pkt.data_size = static_cast<uint32_t>(in.data.size());
        pkt.pts = in.pts;
        pkt.dts = in.dts;
        pkt.flags = in.flags;
        pkt.user_opaque = in.user_opaque;

        std::string error;
        MFX50RT_Status st = ctx->realtime_backend->pushPacket(pkt, &error);
        if (st != MFX50RT_OK) {
            std::lock_guard<std::mutex> lock(route->mutex);
            copy_cstr(route->stats.last_warning,
                      sizeof(route->stats.last_warning),
                      error.empty() ? "oneVPL realtime backend push failed" : error.c_str());
            emit_event(ctx,
                       MFX50RT_EVENT_ERROR,
                       route->stream_id,
                       st,
                       route->stats.last_warning);
            return st;
        }
        if (ctx->output_cb || ctx->config.runtime.enable_callback) {
            st = drain_realtime_outputs(ctx, nullptr, in.pushed_at, &error);
            if (st != MFX50RT_OK) {
                std::lock_guard<std::mutex> lock(route->mutex);
                copy_cstr(route->stats.last_warning,
                          sizeof(route->stats.last_warning),
                          error.empty() ? "oneVPL realtime backend poll failed" : error.c_str());
                emit_event(ctx,
                           MFX50RT_EVENT_ERROR,
                           route->stream_id,
                           st,
                           route->stats.last_warning);
                return st;
            }
        }
        if (in.flags & MFX50RT_PACKET_FLAG_EOS) {
            emit_event(ctx, MFX50RT_EVENT_ROUTE_STOPPED, route->stream_id, 0, "route received EOS");
        }
        return MFX50RT_OK;
    }

    HybridTSRQDecision decision = route->controller.decideFromYPlane(
        nullptr,
        ctx->config.pipeline.width,
        ctx->config.pipeline.height,
        ctx->config.pipeline.width,
        in.pts);

    const bool qg_on = route->controller.qualityState().active;
    if (qg_on != route->quality_guard_was_on) {
        emit_event(ctx,
                   qg_on ? MFX50RT_EVENT_QUALITY_GUARD_ON : MFX50RT_EVENT_QUALITY_GUARD_OFF,
                   route->stream_id,
                   0,
                   qg_on ? "quality guard on" : "quality guard off");
        route->quality_guard_was_on = qg_on;
    }
    if (decision.temporal.force_idr) {
        emit_event(ctx, MFX50RT_EVENT_FORCE_IDR, route->stream_id, 0, "HybridTSRQ requested force IDR");
    }

    if (in.flags & MFX50RT_PACKET_FLAG_EOS) {
        emit_event(ctx, MFX50RT_EVENT_ROUTE_STOPPED, route->stream_id, 0, "route received EOS");
        return MFX50RT_OK;
    }

    const int target = std::max(0, std::min(99, ctx->config.algo.target_compression_percent));
    const size_t keep = in.data.empty()
        ? 0
        : std::max<size_t>(1, in.data.size() * static_cast<size_t>(100 - target) / 100);
    auto out = std::make_unique<OutputPacketHandle>();
    init_public_struct(out->packet);
    out->bytes.resize(std::min(keep, in.data.size()));
    if (!out->bytes.empty()) {
        for (size_t i = 0; i < out->bytes.size(); ++i) {
            out->bytes[i] = in.data[(i * 9973) % in.data.size()];
        }
    }
    out->packet.stream_id = in.stream_id;
    out->packet.data_size = static_cast<uint32_t>(out->bytes.size());
    out->packet.pts = in.pts;
    out->packet.dts = in.dts;
    out->packet.flags = decision.temporal.force_idr ? MFX50RT_PACKET_FLAG_KEYFRAME : 0;
    out->packet.codec = ctx->config.pipeline.output_codec;
    out->packet.frame_type = frame_type_from_decision(decision);
    out->packet.qp_avg = decision.spatial.spatial_avg_qp > 0
        ? decision.spatial.spatial_avg_qp
        : decision.temporal.frame_anchor_qp;
    out->packet.qp_min = decision.spatial.spatial_min_qp > 0
        ? decision.spatial.spatial_min_qp
        : decision.temporal.frame_anchor_qp;
    out->packet.qp_max = decision.spatial.spatial_max_qp > 0
        ? decision.spatial.spatial_max_qp
        : decision.temporal.frame_anchor_qp;
    out->packet.user_opaque = in.user_opaque;

    {
        std::lock_guard<std::mutex> lock(route->mutex);
        route->stats.output_packets++;
        route->stats.output_bytes += out->packet.data_size;
        route->stats.encoded_frames++;
        route->stats.decoded_frames++;
        if (decision.temporal.force_idr) route->stats.force_idr_count++;
        if (decision.strategy == MFX50RT_STRATEGY_MBQP_CQP) route->stats.mbqp_frames++;
        else if (decision.strategy == MFX50RT_STRATEGY_ROI_DELTA_QP) route->stats.roi_frames++;
        else route->stats.global_frames++;
        copy_decision_qp_stats(route->stats, decision);
        route->stats.avg_ssim = route->controller.qualityState().avg_ssim;
        route->stats.min_ssim = route->controller.qualityState().min_ssim;
        route->stats.p5_ssim = route->controller.qualityState().p5_ssim;
        route->stats.fast_analyze_ms_avg = 0.0;
        route->stats.heavy_analyze_ms_avg = 0.0;
        route->latencies_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - in.pushed_at).count());
        if (route->latencies_ms.size() > 512) route->latencies_ms.erase(route->latencies_ms.begin());
        refresh_route_stats(*route);
    }

    enqueue_output(ctx, std::move(out));
    return MFX50RT_OK;
}

void worker_loop(MFX50RT_Context* ctx, RouteContext* route) {
    while (true) {
        PendingPacket pkt;
        {
            std::unique_lock<std::mutex> lock(route->mutex);
            route->cv.wait(lock, [&]() {
                return route->stop || !route->input_queue.empty();
            });
            if (route->stop && route->input_queue.empty()) break;
            pkt = std::move(route->input_queue.front());
            route->input_queue.pop_front();
            route->processing = true;
            route->stats.queue_depth_input = static_cast<uint32_t>(route->input_queue.size());
        }
        MFX50RT_Status st = process_packet(ctx, route, pkt);
        {
            std::lock_guard<std::mutex> lock(route->mutex);
            route->processing = false;
            if (st != MFX50RT_OK) {
                route->stats.fallback_count++;
            }
        }
        route->cv.notify_all();
    }
}

void wait_route_idle(RouteContext& route) {
    std::unique_lock<std::mutex> lock(route.mutex);
    route.cv.wait(lock, [&]() {
        return route.input_queue.empty() && !route.processing;
    });
}

} // namespace

extern "C" {

const char* MFX50RT_GetVersion(void) {
    return kVersion;
}

const char* MFX50RT_StatusString(MFX50RT_Status status) {
    switch (status) {
        case MFX50RT_OK: return "OK";
        case MFX50RT_ERR_UNKNOWN: return "UNKNOWN";
        case MFX50RT_ERR_INVALID_ARG: return "INVALID_ARG";
        case MFX50RT_ERR_NOT_READY: return "NOT_READY";
        case MFX50RT_ERR_AGAIN: return "AGAIN";
        case MFX50RT_ERR_BUFFER_TOO_SMALL: return "BUFFER_TOO_SMALL";
        case MFX50RT_ERR_UNSUPPORTED: return "UNSUPPORTED";
        case MFX50RT_ERR_DEVICE: return "DEVICE";
        case MFX50RT_ERR_EOS: return "EOS";
        default: return "UNRECOGNIZED";
    }
}

MFX50RT_Status MFX50RT_DefaultConfig(MFX50RT_Config* config) {
    if (!config) return MFX50RT_ERR_INVALID_ARG;
    set_defaults(config);
    return MFX50RT_OK;
}

MFX50RT_Status MFX50RT_QueryCapabilities(const MFX50RT_BackendConfig* backend,
                                         MFX50RT_Capabilities* caps) {
    if (!backend || !caps ||
        !has_valid_header(backend->size, backend->version, sizeof(MFX50RT_BackendConfig)) ||
        !has_valid_header(caps->size, caps->version, sizeof(MFX50RT_Capabilities))) {
        return MFX50RT_ERR_INVALID_ARG;
    }
    if (backend->type == MFX50RT_BACKEND_ONEVPL || backend->type == MFX50RT_BACKEND_AUTO) {
        return mfx50rt::onevpl::queryRealCapabilities(*backend, caps);
    }
    MFX50RT_Capabilities out{};
    init_public_struct(out);
    out.supports_cqp = 1;
    out.supports_ipb_qp = 1;
    out.supports_force_idr = 1;
    out.max_width = 8192;
    out.max_height = 4320;
    out.max_async_depth = backend->async_depth > 0 ? backend->async_depth : 4;
    if (backend->type == MFX50RT_BACKEND_NULL) {
        copy_cstr(out.backend_name, sizeof(out.backend_name), "null");
        copy_cstr(out.driver_desc, sizeof(out.driver_desc), "in-process validation backend; no real encode");
    } else {
        copy_cstr(out.backend_name, sizeof(out.backend_name), "conservative-generic");
        copy_cstr(out.driver_desc, sizeof(out.driver_desc), "generic backend capability set; MBQP/ROI disabled until probed");
    }
    *caps = out;
    return MFX50RT_OK;
}

MFX50RT_Status MFX50RT_Create(const MFX50RT_Config* config,
                              MFX50RT_Handle* handle) {
    if (!handle) return MFX50RT_ERR_INVALID_ARG;
    *handle = nullptr;
    MFX50RT_Status st = validate_config(config);
    if (st != MFX50RT_OK) return st;

    auto ctx = std::make_unique<MFX50RT_Context>();
    ctx->config = *config;
    normalize_config(ctx->config);
    init_public_struct(ctx->caps);
    st = MFX50RT_QueryCapabilities(&ctx->config.backend, &ctx->caps);
    if (st != MFX50RT_OK) return st;
    ctx->effective = make_effective(ctx->config, ctx->caps);

    if (ctx->effective.effective_backend == MFX50RT_BACKEND_ONEVPL) {
        std::string error;
        ctx->realtime_backend = mfx50rt::onevpl::RealtimeBackend::create(ctx->config, &error);
        if (!ctx->realtime_backend) {
            if (ctx->config.backend.type == MFX50RT_BACKEND_AUTO) {
                ctx->caps.supports_hw_decode = 0;
                ctx->caps.supports_hw_encode = 0;
                ctx->effective = make_effective(ctx->config, ctx->caps);
                copy_cstr(ctx->effective.fallback_reason,
                          sizeof(ctx->effective.fallback_reason),
                          error.empty() ? "oneVPL backend unavailable; using NULL fallback" : error.c_str());
                ctx->effective.effective_backend = MFX50RT_BACKEND_NULL;
            } else {
                return MFX50RT_ERR_DEVICE;
            }
        }
    }

    const int route_count = std::max(1, ctx->config.runtime.route_count);
    ctx->routes.reserve(static_cast<size_t>(route_count));
    for (int i = 0; i < route_count; ++i) {
        auto route = std::make_unique<RouteContext>();
        route->stream_id = static_cast<uint32_t>(i);
        init_public_struct(route->stats);
        route->stats.stream_id = static_cast<uint32_t>(i);
        copy_cstr(route->stats.effective_strategy,
                  sizeof(route->stats.effective_strategy),
                  strategy_name(ctx->effective.effective_strategy));
        copy_cstr(route->stats.actual_encode_control,
                  sizeof(route->stats.actual_encode_control),
                  ctx->effective.effective_backend == MFX50RT_BACKEND_ONEVPL ? "PENDING" : "GLOBAL");
        if (ctx->effective.fallback_reason[0]) {
            copy_cstr(route->stats.last_warning,
                      sizeof(route->stats.last_warning),
                      ctx->effective.fallback_reason);
            route->stats.fallback_count = 1;
        }
        route->force_mbqp_pattern = force_mbqp_pattern_from_config(ctx->config);
        int mbqp_reuse_interval = 1;
        if (json_int(ctx->config.algo.expert_options_json,
                     "mbqp_reuse_interval",
                     &mbqp_reuse_interval)) {
            route->mbqp_reuse_interval = std::max(1, std::min(30, mbqp_reuse_interval));
        }
        route->controller.configure(route->stream_id,
                                    ctx->config,
                                    ctx->caps,
                                    ctx->effective);
        ctx->routes.push_back(std::move(route));
    }
    if (ctx->realtime_backend &&
        ctx->effective.effective_strategy == MFX50RT_STRATEGY_MBQP_CQP) {
        std::string error;
        st = ctx->realtime_backend->setFrameDecisionCallback(hybrid_frame_decision_callback,
                                                             ctx.get(),
                                                             &error);
        if (st == MFX50RT_OK) {
            st = ctx->realtime_backend->setEncodeControlEventCallback(
                hybrid_encode_control_event_callback,
                ctx.get(),
                &error);
        }
        if (st != MFX50RT_OK) {
            for (auto& route : ctx->routes) {
                copy_cstr(route->stats.actual_encode_control,
                          sizeof(route->stats.actual_encode_control),
                          "GLOBAL");
                copy_cstr(route->stats.last_warning,
                          sizeof(route->stats.last_warning),
                          error.empty() ? "oneVPL MBQP callback hook registration failed" : error.c_str());
                route->stats.fallback_count++;
            }
            ctx->effective.effective_strategy = MFX50RT_STRATEGY_GLOBAL;
            ctx->effective.mbqp_enabled = 0;
            copy_cstr(ctx->effective.fallback_reason,
                      sizeof(ctx->effective.fallback_reason),
                      error.empty() ? "oneVPL MBQP callback hook registration failed" : error.c_str());
        }
    }
    if (ctx->config.runtime.async_mode) {
        for (auto& route : ctx->routes) {
            route->worker = std::thread(worker_loop, ctx.get(), route.get());
        }
    }
    *handle = ctx.release();
    return MFX50RT_OK;
}

MFX50RT_Status MFX50RT_CreateFromJson(const char* json_path_or_text,
                                      int32_t is_path,
                                      MFX50RT_Handle* handle) {
    if (!json_path_or_text || !handle) return MFX50RT_ERR_INVALID_ARG;
    const std::string json = is_path ? read_file(json_path_or_text) : std::string(json_path_or_text);
    if (json.empty()) return MFX50RT_ERR_INVALID_ARG;
    MFX50RT_Config cfg{};
    MFX50RT_Status st = parse_json_config(json, &cfg);
    if (st != MFX50RT_OK) return st;
    return MFX50RT_Create(&cfg, handle);
}

MFX50RT_Status MFX50RT_GetEffectiveConfig(MFX50RT_Handle handle,
                                          MFX50RT_EffectiveConfig* out_config) {
    MFX50RT_Context* ctx = as_ctx(handle);
    if (!ctx || !out_config ||
        !has_valid_header(out_config->size, out_config->version, sizeof(MFX50RT_EffectiveConfig))) {
        return MFX50RT_ERR_INVALID_ARG;
    }
    *out_config = ctx->effective;
    return MFX50RT_OK;
}

MFX50RT_Status MFX50RT_PushPacket(MFX50RT_Handle handle,
                                  const MFX50RT_InputPacket* packet) {
    MFX50RT_Context* ctx = as_ctx(handle);
    if (!ctx || ctx->closed || !packet ||
        !has_valid_header(packet->size, packet->version, sizeof(MFX50RT_InputPacket))) {
        return MFX50RT_ERR_INVALID_ARG;
    }
    RouteContext* route = get_route(ctx, packet->stream_id);
    if (!route) return MFX50RT_ERR_INVALID_ARG;
    if (!(packet->flags & MFX50RT_PACKET_FLAG_EOS) && (!packet->data || packet->data_size == 0)) {
        return MFX50RT_ERR_INVALID_ARG;
    }

    PendingPacket pending;
    pending.stream_id = packet->stream_id;
    pending.pts = packet->pts;
    pending.dts = packet->dts;
    pending.flags = packet->flags;
    pending.user_opaque = packet->user_opaque;
    pending.pushed_at = Clock::now();
    if (packet->data && packet->data_size > 0) {
        pending.data.assign(packet->data, packet->data + packet->data_size);
    }

    {
        std::lock_guard<std::mutex> lock(route->mutex);
        if (ctx->config.runtime.async_mode &&
            route->input_queue.size() >= static_cast<size_t>(ctx->config.runtime.queue_depth_per_route)) {
            route->stats.queue_depth_input = static_cast<uint32_t>(route->input_queue.size());
            route->stats.dropped_frames++;
            copy_cstr(route->stats.last_warning,
                      sizeof(route->stats.last_warning),
                      "input queue backpressure");
            emit_event(ctx, MFX50RT_EVENT_OVERLOAD, packet->stream_id, MFX50RT_ERR_AGAIN, "input queue backpressure");
            return MFX50RT_ERR_AGAIN;
        }
        route->stats.input_packets++;
        route->stats.input_bytes += packet->data_size;
        refresh_route_stats(*route);
        if (ctx->config.runtime.async_mode) {
            route->input_queue.push_back(std::move(pending));
            route->stats.queue_depth_input = static_cast<uint32_t>(route->input_queue.size());
            route->cv.notify_one();
            return MFX50RT_OK;
        }
    }
    return process_packet(ctx, route, pending);
}

MFX50RT_Status MFX50RT_PollPacket(MFX50RT_Handle handle,
                                  MFX50RT_OutputPacket* packet,
                                  int32_t timeout_ms) {
    MFX50RT_Context* ctx = as_ctx(handle);
    if (!ctx || !packet) return MFX50RT_ERR_INVALID_ARG;
    if (packet->size != 0 &&
        !has_valid_header(packet->size, packet->version, sizeof(MFX50RT_OutputPacket))) {
        return MFX50RT_ERR_INVALID_ARG;
    }
    if (ctx->realtime_backend) {
        std::string error;
        MFX50RT_Status st = drain_realtime_outputs(ctx, nullptr, Clock::now(), &error);
        if (st != MFX50RT_OK) return st;
    }
    std::unique_ptr<OutputPacketHandle> out;
    {
        std::unique_lock<std::mutex> lock(ctx->output_mutex);
        if (!ctx->realtime_backend && ctx->output_queue.empty() && timeout_ms > 0) {
            ctx->output_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]() {
                return !ctx->output_queue.empty() || ctx->closed;
            });
        }
        if (ctx->output_queue.empty()) return MFX50RT_ERR_AGAIN;
        out = std::move(ctx->output_queue.front());
        ctx->output_queue.pop_front();
    }
    out->packet.data = out->bytes.empty() ? nullptr : out->bytes.data();
    out->packet.packet_handle = out.get();
    *packet = out->packet;
    out.release();
    return MFX50RT_OK;
}

MFX50RT_Status MFX50RT_ReleasePacket(MFX50RT_Handle handle,
                                     MFX50RT_OutputPacket* packet) {
    (void)handle;
    if (!packet || !packet->packet_handle) return MFX50RT_ERR_INVALID_ARG;
    auto* owned = reinterpret_cast<OutputPacketHandle*>(packet->packet_handle);
    delete owned;
    init_public_struct(*packet);
    return MFX50RT_OK;
}

MFX50RT_Status MFX50RT_Flush(MFX50RT_Handle handle,
                             uint32_t stream_id) {
    MFX50RT_Context* ctx = as_ctx(handle);
    if (!ctx || ctx->closed) return MFX50RT_ERR_INVALID_ARG;
    if (stream_id != MFX50RT_STREAM_ALL && !get_route(ctx, stream_id)) return MFX50RT_ERR_INVALID_ARG;
    if (ctx->realtime_backend) {
        if (stream_id == MFX50RT_STREAM_ALL) {
            for (auto& route : ctx->routes) wait_route_idle(*route);
            std::string error;
            MFX50RT_Status st = ctx->realtime_backend->flush(&error);
            if (st != MFX50RT_OK) return st;
            st = drain_realtime_outputs(ctx, nullptr, Clock::now(), &error);
            if (st != MFX50RT_OK) return st;
        } else {
            wait_route_idle(*get_route(ctx, stream_id));
            MFX50RT_InputPacket eos{};
            init_public_struct(eos);
            eos.stream_id = stream_id;
            eos.flags = MFX50RT_PACKET_FLAG_EOS;
            std::string error;
            MFX50RT_Status st = ctx->realtime_backend->pushPacket(eos, &error);
            if (st != MFX50RT_OK) return st;
            st = drain_realtime_outputs(ctx, nullptr, Clock::now(), &error);
            if (st != MFX50RT_OK) return st;
        }
    }
    if (stream_id == MFX50RT_STREAM_ALL) {
        for (auto& route : ctx->routes) {
            emit_event(ctx, MFX50RT_EVENT_ROUTE_STOPPED, route->stream_id, 0, "flush all");
        }
    } else {
        emit_event(ctx, MFX50RT_EVENT_ROUTE_STOPPED, stream_id, 0, "flush route");
    }
    return MFX50RT_OK;
}

MFX50RT_Status MFX50RT_Close(MFX50RT_Handle handle) {
    MFX50RT_Context* ctx = as_ctx(handle);
    if (!ctx) return MFX50RT_ERR_INVALID_ARG;
    ctx->closed = true;
    for (auto& route : ctx->routes) {
        {
            std::lock_guard<std::mutex> lock(route->mutex);
            route->stop = true;
        }
        route->cv.notify_all();
    }
    for (auto& route : ctx->routes) {
        if (route->worker.joinable()) route->worker.join();
    }
    {
        std::lock_guard<std::mutex> lock(ctx->output_mutex);
        ctx->output_queue.clear();
    }
    ctx->output_cv.notify_all();
    delete ctx;
    return MFX50RT_OK;
}

MFX50RT_Status MFX50RT_UpdateQualityMetric(MFX50RT_Handle handle,
                                           const MFX50RT_QualityMetric* metric) {
    MFX50RT_Context* ctx = as_ctx(handle);
    if (!ctx || !metric ||
        !has_valid_header(metric->size, metric->version, sizeof(MFX50RT_QualityMetric))) {
        return MFX50RT_ERR_INVALID_ARG;
    }
    RouteContext* route = get_route(ctx, metric->stream_id);
    if (!route) return MFX50RT_ERR_INVALID_ARG;
    route->controller.updateQualityMetric(*metric);
    {
        std::lock_guard<std::mutex> lock(route->mutex);
        route->stats.avg_ssim = route->controller.qualityState().avg_ssim;
        route->stats.min_ssim = route->controller.qualityState().min_ssim;
        route->stats.p5_ssim = route->controller.qualityState().p5_ssim;
        refresh_route_stats(*route);
    }
    if (route->controller.qualityState().active && !route->quality_guard_was_on) {
        emit_event(ctx, MFX50RT_EVENT_QUALITY_GUARD_ON, metric->stream_id, 0, "quality guard on after metric update");
        route->quality_guard_was_on = true;
    }
    return MFX50RT_OK;
}

MFX50RT_Status MFX50RT_GetRouteStats(MFX50RT_Handle handle,
                                     uint32_t stream_id,
                                     MFX50RT_RouteStats* stats) {
    MFX50RT_Context* ctx = as_ctx(handle);
    RouteContext* route = get_route(ctx, stream_id);
    if (!route || !stats ||
        !has_valid_header(stats->size, stats->version, sizeof(MFX50RT_RouteStats))) {
        return MFX50RT_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(route->mutex);
    refresh_route_stats(*route);
    if (ctx->effective.effective_strategy == MFX50RT_STRATEGY_MBQP_CQP &&
        route->stats.encoded_frames > 0 &&
        route->stats.mbqp_applied_frames == 0) {
        copy_cstr(route->stats.last_warning,
                  sizeof(route->stats.last_warning),
                  "effective_strategy=MBQP_CQP but no frame has reported actual MBQP attach");
    }
    *stats = route->stats;
    {
        std::lock_guard<std::mutex> qlock(ctx->output_mutex);
        stats->queue_depth_output = static_cast<uint32_t>(ctx->output_queue.size());
    }
    return MFX50RT_OK;
}

MFX50RT_Status MFX50RT_GetGlobalStats(MFX50RT_Handle handle,
                                      MFX50RT_GlobalStats* stats) {
    MFX50RT_Context* ctx = as_ctx(handle);
    if (!ctx || !stats ||
        !has_valid_header(stats->size, stats->version, sizeof(MFX50RT_GlobalStats))) {
        return MFX50RT_ERR_INVALID_ARG;
    }
    MFX50RT_GlobalStats out{};
    init_public_struct(out);
    out.route_count = static_cast<uint32_t>(ctx->routes.size());
    out.active_routes = out.route_count;
    for (auto& route : ctx->routes) {
        std::lock_guard<std::mutex> lock(route->mutex);
        refresh_route_stats(*route);
        out.total_fps_in += route->stats.fps_in;
        out.total_fps_out += route->stats.fps_out;
        out.total_input_bytes += route->stats.input_bytes;
        out.total_output_bytes += route->stats.output_bytes;
        out.total_fallback_count += route->stats.fallback_count;
    }
    out.compression_ratio_avg = out.total_input_bytes > 0
        ? 1.0 - static_cast<double>(out.total_output_bytes) / out.total_input_bytes
        : 0.0;
    *stats = out;
    return MFX50RT_OK;
}

MFX50RT_Status MFX50RT_GetDecisionTrace(MFX50RT_Handle handle,
                                        uint32_t stream_id,
                                        MFX50RT_DecisionTrace* traces,
                                        uint32_t* inout_count) {
    MFX50RT_Context* ctx = as_ctx(handle);
    RouteContext* route = get_route(ctx, stream_id);
    if (!route) return MFX50RT_ERR_INVALID_ARG;
    return route->controller.copyTrace(traces, inout_count);
}

MFX50RT_Status MFX50RT_SetOutputCallback(MFX50RT_Handle handle,
                                         MFX50RT_OutputCallback cb,
                                         void* user_opaque) {
    MFX50RT_Context* ctx = as_ctx(handle);
    if (!ctx) return MFX50RT_ERR_INVALID_ARG;
    ctx->output_cb = cb;
    ctx->output_cb_opaque = user_opaque;
    return MFX50RT_OK;
}

MFX50RT_Status MFX50RT_SetEventCallback(MFX50RT_Handle handle,
                                        MFX50RT_EventCallback cb,
                                        void* user_opaque) {
    MFX50RT_Context* ctx = as_ctx(handle);
    if (!ctx) return MFX50RT_ERR_INVALID_ARG;
    ctx->event_cb = cb;
    ctx->event_cb_opaque = user_opaque;
    if (cb && ctx->effective.fallback_reason[0]) {
        emit_event(ctx,
                   MFX50RT_EVENT_FALLBACK,
                   MFX50RT_STREAM_ALL,
                   MFX50RT_ERR_UNSUPPORTED,
                   ctx->effective.fallback_reason);
    }
    return MFX50RT_OK;
}

} // extern "C"
