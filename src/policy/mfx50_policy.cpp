#include "mfx50_policy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <vector>

struct PolicyOptions {
    int vehicle_delta_qp = -4;
    int plate_delta_qp = -12;
    int sign_text_delta_qp = -3;
    int background_delta_qp = 4;
    int denoise_strength = 0;
    int gop_size = 300;
    int b_frame_dist = 4;
    int roi_area_budget_percent = 20;
    std::map<std::string, std::string> raw;
};

struct MFX50_PolicyContext {
    MFX50_PolicyConfig config{};
    PolicyOptions options;
    MFX50_LogCallback log_callback = nullptr;
    void* log_user_data = nullptr;
};

struct MFX50_PolicyStream {
    MFX50_PolicyContext* context = nullptr;
    MFX50_StreamConfig config{};
    MFX50_FrameFeatures features{};
    bool has_features = false;
    MFX50_Metadata metadata{};
    bool has_metadata = false;
    MFX50_PolicyStats stats{};
    uint64_t next_decision_id = 1;
    uint64_t roi_count_sum = 0;
    double roi_area_percent_sum = 0.0;
};

namespace {

constexpr const char* kBuildString = "mfx50_policy_sdk_1.0.0";

bool valid_context(const MFX50_PolicyContext* context) {
    return context != nullptr;
}

bool valid_stream(const MFX50_PolicyStream* stream) {
    return stream != nullptr && stream->context != nullptr;
}

void log_msg(MFX50_PolicyContext* ctx, int level, const char* message) {
    if (ctx && ctx->log_callback) {
        ctx->log_callback(level, message, ctx->log_user_data);
    }
}

MFX50_EncoderCaps default_caps() {
    MFX50_EncoderCaps caps{};
    caps.struct_size = sizeof(caps);
    caps.supports_b_frames = 1;
    caps.supports_roi = 1;
    caps.supports_mbqp = 0;
    caps.supports_dynamic_gop = 1;
    caps.supports_idr_request = 1;
    caps.supports_frame_reuse = 0;
    caps.supports_negative_delta_qp = 1;
    caps.supports_positive_delta_qp = 1;
    caps.supports_per_frame_qp = 1;
    caps.supports_roi_delta_qp = 1;
    caps.supports_roi_absolute_qp = 0;
    caps.max_roi_count = MFX50_MAX_ROIS;
    caps.roi_alignment = 16;
    caps.mbqp_block_size = 16;
    caps.min_qp = 1;
    caps.max_qp = 51;
    caps.min_delta_qp = -16;
    caps.max_delta_qp = 16;
    caps.max_b_frame_dist = 4;
    caps.max_gop_size = 300;
    return caps;
}

void normalize_caps(MFX50_EncoderCaps& caps) {
    if (caps.struct_size == 0) {
        caps = default_caps();
        return;
    }
    if (caps.max_roi_count <= 0 || caps.max_roi_count > MFX50_MAX_ROIS) {
        caps.max_roi_count = MFX50_MAX_ROIS;
    }
    if (caps.roi_alignment <= 0) {
        caps.roi_alignment = 16;
    }
    if (caps.min_qp <= 0) {
        caps.min_qp = 1;
    }
    if (caps.max_qp <= 0) {
        caps.max_qp = 51;
    }
    if (caps.min_delta_qp == 0 && caps.max_delta_qp == 0) {
        caps.min_delta_qp = -16;
        caps.max_delta_qp = 16;
    }
    if (caps.max_gop_size <= 0) {
        caps.max_gop_size = 300;
    }
    if (caps.max_b_frame_dist <= 0) {
        caps.max_b_frame_dist = caps.supports_b_frames ? 4 : 1;
    }
}

int clamp_int(int value, int lo, int hi) {
    return std::max(lo, std::min(hi, value));
}

float clamp_float(float value, float lo, float hi) {
    return std::max(lo, std::min(hi, value));
}

int align_down(int value, int alignment) {
    if (alignment <= 1) {
        return value;
    }
    return value - (value % alignment);
}

int align_up(int value, int alignment) {
    if (alignment <= 1) {
        return value;
    }
    return ((value + alignment - 1) / alignment) * alignment;
}

bool parse_int(const char* value, int& out) {
    if (!value) {
        return false;
    }
    char* end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (!end || *end != '\0') {
        return false;
    }
    out = static_cast<int>(parsed);
    return true;
}

void set_option_value(PolicyOptions& options, const char* key, const char* value) {
    if (!key || !value) {
        return;
    }
    options.raw[key] = value;
    int parsed = 0;
    if (!parse_int(value, parsed)) {
        return;
    }
    const std::string k(key);
    if (k == "vehicle.delta_qp") {
        options.vehicle_delta_qp = parsed;
    } else if (k == "plate.delta_qp") {
        options.plate_delta_qp = parsed;
    } else if (k == "sign_text.delta_qp") {
        options.sign_text_delta_qp = parsed;
    } else if (k == "background.delta_qp") {
        options.background_delta_qp = parsed;
    } else if (k == "denoise.background_strength") {
        options.denoise_strength = parsed;
    } else if (k == "gop.size") {
        options.gop_size = parsed;
    } else if (k == "bframe.dist") {
        options.b_frame_dist = parsed;
    } else if (k == "roi.area_budget_percent") {
        options.roi_area_budget_percent = parsed;
    }
}

const char* profile_name(int profile_id) {
    switch (profile_id) {
        case MFX50_PROFILE_DAY_GUARD_Q34:
            return "day_guard_q34";
        case MFX50_PROFILE_RISK_Q35:
            return "risk_q35";
        case MFX50_PROFILE_QUALITY_Q32:
            return "quality_q32";
        case MFX50_PROFILE_BASE_Q36B48:
        default:
            return "base_q36b48";
    }
}

void choose_profile(const MFX50_PolicyConfig& config,
                    const MFX50_FrameFeatures& features,
                    MFX50_EncodeDecision& decision) {
    int profile = MFX50_PROFILE_BASE_Q36B48;
    int qpi = 36;
    int qpp = 38;
    int qpb = 48;

    if (config.mode == MFX50_MODE_QUALITY) {
        profile = MFX50_PROFILE_QUALITY_Q32;
        qpi = 32;
        qpp = 34;
        qpb = 40;
    } else {
        const float mean_y = features.mean_y;
        const float edge = features.edge_density;
        if (95.0f <= mean_y && mean_y <= 108.0f && 0.15f <= edge && edge <= 0.225f) {
            profile = MFX50_PROFILE_DAY_GUARD_Q34;
            qpi = 34;
            qpp = 36;
            qpb = 42;
        } else if (95.0f <= mean_y && mean_y <= 112.0f && edge > 0.25f) {
            profile = MFX50_PROFILE_RISK_Q35;
            qpi = 35;
            qpp = 37;
            qpb = 43;
        }
    }

    decision.profile_id = profile;
    decision.qpi = qpi;
    decision.qpp = qpp;
    decision.qpb = qpb;
    decision.applied_flags |= MFX50_DECISION_FLAG_QP;
}

int metadata_delta_qp(const PolicyOptions& options, const MFX50_MetadataObject& object) {
    if (object.suggested_delta_qp != 0) {
        return object.suggested_delta_qp;
    }
    switch (object.type) {
        case MFX50_OBJECT_PLATE:
            return options.plate_delta_qp;
        case MFX50_OBJECT_VEHICLE:
            return options.vehicle_delta_qp;
        case MFX50_OBJECT_SIGN:
        case MFX50_OBJECT_TEXT:
            return options.sign_text_delta_qp;
        case MFX50_OBJECT_BACKGROUND:
        case MFX50_OBJECT_ROAD:
            return options.background_delta_qp;
        default:
            return options.vehicle_delta_qp;
    }
}

int default_priority(const MFX50_MetadataObject& object) {
    if (object.priority != 0) {
        return object.priority;
    }
    switch (object.type) {
        case MFX50_OBJECT_PLATE:
            return 100;
        case MFX50_OBJECT_VEHICLE:
            return 80;
        case MFX50_OBJECT_SIGN:
        case MFX50_OBJECT_TEXT:
            return 60;
        case MFX50_OBJECT_BACKGROUND:
        case MFX50_OBJECT_ROAD:
            return 10;
        default:
            return 20;
    }
}

MFX50_Roi roi_from_metadata(const MFX50_MetadataObject& object,
                            const MFX50_StreamConfig& stream_cfg,
                            const MFX50_EncoderCaps& caps,
                            const PolicyOptions& options) {
    const int align = std::max(1, caps.roi_alignment);
    int x0 = align_down(std::max(0, object.x), align);
    int y0 = align_down(std::max(0, object.y), align);
    int x1 = align_up(std::max(object.x + object.w, x0 + align), align);
    int y1 = align_up(std::max(object.y + object.h, y0 + align), align);
    if (stream_cfg.width > 0) {
        x1 = std::min(x1, stream_cfg.width);
    }
    if (stream_cfg.height > 0) {
        y1 = std::min(y1, stream_cfg.height);
    }
    MFX50_Roi roi{};
    roi.x = x0;
    roi.y = y0;
    roi.w = std::max(0, x1 - x0);
    roi.h = std::max(0, y1 - y0);
    roi.delta_qp = clamp_int(metadata_delta_qp(options, object), caps.min_delta_qp, caps.max_delta_qp);
    roi.type = object.type;
    roi.priority = default_priority(object);
    roi.source = MFX50_ROI_SOURCE_METADATA;
    roi.confidence = clamp_float(object.confidence, 0.0f, 1.0f);
    roi.track_id = object.track_id;
    return roi;
}

void fill_reason(MFX50_EncodeDecision& decision, const char* reason) {
    std::strncpy(decision.reason, reason, sizeof(decision.reason) - 1);
    decision.reason[sizeof(decision.reason) - 1] = '\0';
}

} // namespace

extern "C" {

MFX50_Status mfx50_policy_get_version(MFX50_Version* out_version) {
    if (!out_version || out_version->struct_size < sizeof(MFX50_Version)) {
        return MFX50_ERR_INVALID_PARAM;
    }
    out_version->api_version = MFX50_POLICY_API_VERSION;
    out_version->major = 1;
    out_version->minor = 0;
    out_version->patch = 0;
    out_version->build = kBuildString;
    return MFX50_OK;
}

MFX50_Status mfx50_policy_create(const MFX50_PolicyConfig* config,
                                 MFX50_PolicyContext** out_context) {
    if (!config || !out_context || config->struct_size < sizeof(MFX50_PolicyConfig)) {
        return MFX50_ERR_INVALID_PARAM;
    }
    if (config->api_version != MFX50_POLICY_API_VERSION) {
        return MFX50_ERR_VERSION_MISMATCH;
    }
    std::unique_ptr<MFX50_PolicyContext> ctx(new (std::nothrow) MFX50_PolicyContext());
    if (!ctx) {
        return MFX50_ERR_NO_MEMORY;
    }
    ctx->config = *config;
    normalize_caps(ctx->config.encoder_caps);
    set_option_value(ctx->options, "vehicle.delta_qp", "-4");
    set_option_value(ctx->options, "plate.delta_qp", "-12");
    set_option_value(ctx->options, "sign_text.delta_qp", "-3");
    set_option_value(ctx->options, "background.delta_qp", "4");
    set_option_value(ctx->options, "denoise.background_strength", "0");
    set_option_value(ctx->options, "gop.size", "300");
    set_option_value(ctx->options, "bframe.dist", "4");
    set_option_value(ctx->options, "roi.area_budget_percent", "20");
    *out_context = ctx.release();
    return MFX50_OK;
}

void mfx50_policy_destroy(MFX50_PolicyContext* context) {
    delete context;
}

MFX50_Status mfx50_policy_create_stream(MFX50_PolicyContext* context,
                                        const MFX50_StreamConfig* config,
                                        MFX50_PolicyStream** out_stream) {
    if (!valid_context(context) || !config || !out_stream ||
        config->struct_size < sizeof(MFX50_StreamConfig)) {
        return MFX50_ERR_INVALID_PARAM;
    }
    if (config->width <= 0 || config->height <= 0 || config->fps <= 0.0f) {
        return MFX50_ERR_INVALID_PARAM;
    }
    std::unique_ptr<MFX50_PolicyStream> stream(new (std::nothrow) MFX50_PolicyStream());
    if (!stream) {
        return MFX50_ERR_NO_MEMORY;
    }
    stream->context = context;
    stream->config = *config;
    stream->stats.struct_size = sizeof(MFX50_PolicyStats);
    *out_stream = stream.release();
    return MFX50_OK;
}

void mfx50_policy_destroy_stream(MFX50_PolicyStream* stream) {
    delete stream;
}

MFX50_Status mfx50_policy_reset_stream(MFX50_PolicyStream* stream, MFX50_ResetMode mode) {
    if (!valid_stream(stream)) {
        return MFX50_ERR_INVALID_PARAM;
    }
    stream->has_features = false;
    stream->has_metadata = false;
    std::memset(&stream->metadata, 0, sizeof(stream->metadata));
    stream->roi_count_sum = 0;
    stream->roi_area_percent_sum = 0.0;
    stream->stats.avg_roi_count = 0.0f;
    stream->stats.avg_roi_area_percent = 0.0f;
    if (mode == MFX50_RESET_HARD) {
        stream->stats = {};
        stream->stats.struct_size = sizeof(MFX50_PolicyStats);
        stream->next_decision_id = 1;
    } else if (mode == MFX50_RESET_SCENE_CUT) {
        stream->features.scene_cut_score = 1.0f;
        stream->has_features = true;
    }
    return MFX50_OK;
}

MFX50_Status mfx50_policy_submit_features(MFX50_PolicyStream* stream,
                                          const MFX50_FrameFeatures* features) {
    if (!valid_stream(stream) || !features || features->struct_size < sizeof(MFX50_FrameFeatures)) {
        return MFX50_ERR_INVALID_PARAM;
    }
    stream->features = *features;
    stream->has_features = true;
    stream->stats.frames_seen += 1;
    return MFX50_OK;
}

MFX50_Status mfx50_policy_submit_metadata(MFX50_PolicyStream* stream,
                                          const MFX50_Metadata* metadata) {
    if (!valid_stream(stream) || !metadata || metadata->struct_size < sizeof(MFX50_Metadata)) {
        return MFX50_ERR_INVALID_PARAM;
    }
    stream->metadata = *metadata;
    stream->metadata.object_count = clamp_int(stream->metadata.object_count, 0, MFX50_MAX_METADATA_OBJECTS);
    stream->has_metadata = true;
    stream->stats.metadata_object_count += static_cast<uint64_t>(stream->metadata.object_count);
    return MFX50_OK;
}

MFX50_Status mfx50_policy_submit_frame(MFX50_PolicyStream* stream,
                                       const MFX50_AnalyzeFrame* frame) {
    if (!valid_stream(stream) || !frame || frame->struct_size < sizeof(MFX50_AnalyzeFrame) ||
        !frame->y_plane || frame->width <= 0 || frame->height <= 0 || frame->y_stride <= 0) {
        return MFX50_ERR_INVALID_PARAM;
    }
    if (!frame->is_lowres && frame->width * frame->height > 640 * 360) {
        log_msg(stream->context, 1, "submit_frame received a full-resolution frame; prefer lowres features for realtime use");
    }

    double sum = 0.0;
    double edge_sum = 0.0;
    const int step_y = std::max(1, frame->height / 90);
    const int step_x = std::max(1, frame->width / 160);
    int samples = 0;
    int edge_samples = 0;
    for (int y = 0; y < frame->height; y += step_y) {
        const uint8_t* row = frame->y_plane + y * frame->y_stride;
        const uint8_t* prev = y > 0 ? frame->y_plane + (y - step_y) * frame->y_stride : nullptr;
        for (int x = 0; x < frame->width; x += step_x) {
            const int v = row[x];
            sum += v;
            samples += 1;
            if (x >= step_x) {
                edge_sum += std::abs(v - static_cast<int>(row[x - step_x]));
                edge_samples += 1;
            }
            if (prev) {
                edge_sum += std::abs(v - static_cast<int>(prev[x]));
                edge_samples += 1;
            }
        }
    }

    MFX50_FrameFeatures features{};
    features.struct_size = sizeof(features);
    features.frame_index = frame->frame_index;
    features.pts = frame->pts;
    features.mean_y = samples > 0 ? static_cast<float>(sum / samples) : 0.0f;
    features.edge_density = edge_samples > 0 ? static_cast<float>((edge_sum / edge_samples) / 255.0) : 0.0f;
    return mfx50_policy_submit_features(stream, &features);
}

MFX50_Status mfx50_policy_get_decision(MFX50_PolicyStream* stream,
                                       MFX50_EncodeDecision* decision) {
    if (!valid_stream(stream) || !decision || decision->struct_size < sizeof(MFX50_EncodeDecision)) {
        return MFX50_ERR_INVALID_PARAM;
    }
    const MFX50_EncoderCaps& caps = stream->context->config.encoder_caps;
    const PolicyOptions& options = stream->context->options;
    MFX50_FrameFeatures features = stream->features;
    if (!stream->has_features) {
        features.struct_size = sizeof(features);
        features.mean_y = 60.0f;
        features.edge_density = 0.10f;
    }

    std::memset(decision, 0, sizeof(MFX50_EncodeDecision));
    decision->struct_size = sizeof(MFX50_EncodeDecision);
    decision->decision_id = stream->next_decision_id++;
    choose_profile(stream->context->config, features, *decision);

    decision->gop_size = clamp_int(options.gop_size, 1, caps.max_gop_size);
    decision->b_frame_dist = caps.supports_b_frames
        ? clamp_int(options.b_frame_dist, 1, caps.max_b_frame_dist)
        : 1;
    if (caps.supports_b_frames) {
        decision->applied_flags |= MFX50_DECISION_FLAG_B_FRAMES;
    } else {
        decision->disabled_flags |= MFX50_DECISION_FLAG_B_FRAMES;
    }

    if (features.scene_cut_score >= 0.75f) {
        if (caps.supports_idr_request) {
            decision->request_idr = 1;
            decision->applied_flags |= MFX50_DECISION_FLAG_IDR;
        } else {
            decision->disabled_flags |= MFX50_DECISION_FLAG_IDR;
        }
    }

    if (caps.supports_dynamic_gop) {
        decision->applied_flags |= MFX50_DECISION_FLAG_DYNAMIC_GOP;
    } else {
        decision->disabled_flags |= MFX50_DECISION_FLAG_DYNAMIC_GOP;
    }

    if (caps.supports_positive_delta_qp) {
        decision->background_delta_qp = clamp_int(options.background_delta_qp, caps.min_delta_qp, caps.max_delta_qp);
        decision->applied_flags |= MFX50_DECISION_FLAG_BACKGROUND_QP;
    } else {
        decision->disabled_flags |= MFX50_DECISION_FLAG_BACKGROUND_QP;
    }
    decision->denoise_strength = std::max(0, options.denoise_strength);
    if (decision->denoise_strength > 0) {
        decision->applied_flags |= MFX50_DECISION_FLAG_DENOISE;
    }

    std::vector<MFX50_Roi> rois;
    if (stream->has_metadata && caps.supports_roi && caps.supports_roi_delta_qp) {
        for (int i = 0; i < stream->metadata.object_count; ++i) {
            const MFX50_MetadataObject& object = stream->metadata.objects[i];
            if (object.w <= 0 || object.h <= 0 || object.confidence < 0.1f) {
                continue;
            }
            MFX50_Roi roi = roi_from_metadata(object, stream->config, caps, options);
            if (roi.w > 0 && roi.h > 0) {
                rois.push_back(roi);
            }
        }
        std::sort(rois.begin(), rois.end(), [](const MFX50_Roi& a, const MFX50_Roi& b) {
            if (a.priority != b.priority) {
                return a.priority > b.priority;
            }
            return a.confidence > b.confidence;
        });
        if (static_cast<int>(rois.size()) > caps.max_roi_count) {
            rois.resize(static_cast<size_t>(caps.max_roi_count));
        }
        decision->roi_count = static_cast<int32_t>(std::min<size_t>(rois.size(), MFX50_MAX_ROIS));
        for (int i = 0; i < decision->roi_count; ++i) {
            decision->rois[i] = rois[static_cast<size_t>(i)];
        }
        if (decision->roi_count > 0) {
            decision->applied_flags |= MFX50_DECISION_FLAG_ROI;
        }
    } else if (stream->has_metadata) {
        decision->disabled_flags |= MFX50_DECISION_FLAG_ROI;
    }

    if (decision->disabled_flags != 0) {
        stream->stats.disabled_flags_count += 1;
        fill_reason(*decision, "some decisions were disabled by encoder capabilities");
    } else {
        char reason[128];
        std::snprintf(reason, sizeof(reason), "profile=%s", profile_name(decision->profile_id));
        fill_reason(*decision, reason);
    }

    stream->stats.decisions_made += 1;
    stream->stats.current_profile_id = decision->profile_id;
    if (decision->request_idr) {
        stream->stats.idr_request_count += 1;
    }
    const double frame_area = static_cast<double>(std::max(1, stream->config.width)) *
                              static_cast<double>(std::max(1, stream->config.height));
    double roi_area = 0.0;
    for (int i = 0; i < decision->roi_count; ++i) {
        roi_area += static_cast<double>(decision->rois[i].w) * static_cast<double>(decision->rois[i].h);
    }
    stream->roi_count_sum += static_cast<uint64_t>(decision->roi_count);
    stream->roi_area_percent_sum += frame_area > 0.0 ? (roi_area / frame_area) * 100.0 : 0.0;
    const double decisions = static_cast<double>(std::max<uint64_t>(1, stream->stats.decisions_made));
    stream->stats.avg_roi_count = static_cast<float>(stream->roi_count_sum / decisions);
    stream->stats.avg_roi_area_percent = static_cast<float>(stream->roi_area_percent_sum / decisions);
    return MFX50_OK;
}

MFX50_Status mfx50_policy_set_option(MFX50_PolicyContext* context,
                                     const char* key,
                                     const char* value) {
    if (!valid_context(context) || !key || !value) {
        return MFX50_ERR_INVALID_PARAM;
    }
    set_option_value(context->options, key, value);
    return MFX50_OK;
}

MFX50_Status mfx50_policy_get_option(MFX50_PolicyContext* context,
                                     const char* key,
                                     char* value,
                                     size_t value_capacity) {
    if (!valid_context(context) || !key || !value || value_capacity == 0) {
        return MFX50_ERR_INVALID_PARAM;
    }
    auto it = context->options.raw.find(key);
    if (it == context->options.raw.end()) {
        return MFX50_ERR_NOT_READY;
    }
    if (it->second.size() + 1 > value_capacity) {
        return MFX50_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(value, it->second.c_str(), it->second.size() + 1);
    return MFX50_OK;
}

MFX50_Status mfx50_policy_get_stats(MFX50_PolicyStream* stream,
                                    MFX50_PolicyStats* stats) {
    if (!valid_stream(stream) || !stats || stats->struct_size < sizeof(MFX50_PolicyStats)) {
        return MFX50_ERR_INVALID_PARAM;
    }
    *stats = stream->stats;
    stats->struct_size = sizeof(MFX50_PolicyStats);
    return MFX50_OK;
}

MFX50_Status mfx50_policy_set_log_callback(MFX50_PolicyContext* context,
                                           MFX50_LogCallback callback,
                                           void* user_data) {
    if (!valid_context(context)) {
        return MFX50_ERR_INVALID_PARAM;
    }
    context->log_callback = callback;
    context->log_user_data = user_data;
    return MFX50_OK;
}

} // extern "C"
