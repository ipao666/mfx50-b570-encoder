#include "mfx50_algo_config.h"

#include <algorithm>
#include <cstring>

namespace mfx50rt {

namespace {

int boolInt(int value) {
    return value ? 1 : 0;
}

int clampInt(int value, int lo, int hi) {
    return std::max(lo, std::min(hi, value));
}

} // namespace

void defaultAlgoConfig(MFX50RT_AlgoConfig* cfg) {
    if (!cfg) return;
    std::memset(cfg, 0, sizeof(*cfg));
    cfg->struct_size = sizeof(*cfg);
    cfg->abi_version = MFX50RT_API_VERSION;
    cfg->smooth_scale_factor = 30;
    cfg->pre_denoise_strength = 30;
    cfg->mbqp_block_size = 16;
    cfg->target_output_ratio_permille = 100;
    cfg->fallback_profile = MFX50_PROFILE_COMPRESS_85_PROBE;
    cfg->aggressive_profile = MFX50_PROFILE_COMPRESS_90_PROBE_B;
}

MFX50RT_AlgoConfig normalizedAlgoConfig(const MFX50RT_AlgoConfig* cfg) {
    MFX50RT_AlgoConfig normalized;
    defaultAlgoConfig(&normalized);
    if (!cfg) return normalized;

    const size_t copySize = cfg->struct_size > 0
        ? std::min<size_t>(cfg->struct_size, sizeof(normalized))
        : sizeof(normalized);
    std::memcpy(&normalized, cfg, copySize);

    normalized.struct_size = sizeof(normalized);
    if (normalized.abi_version == 0) normalized.abi_version = MFX50RT_API_VERSION;

    normalized.enable_preprocess = boolInt(normalized.enable_preprocess);
    normalized.enable_smooth_scale = boolInt(normalized.enable_smooth_scale);
    normalized.enable_pre_denoise = boolInt(normalized.enable_pre_denoise);
    normalized.enable_scene_analyzer = boolInt(normalized.enable_scene_analyzer);
    normalized.enable_adaptive_profile = boolInt(normalized.enable_adaptive_profile);
    normalized.enable_adaptive_qp = boolInt(normalized.enable_adaptive_qp);
    normalized.enable_mbqp = boolInt(normalized.enable_mbqp);

    normalized.smooth_scale_factor = normalized.smooth_scale_factor > 0
        ? clampInt(normalized.smooth_scale_factor, 1, 100)
        : 30;
    normalized.pre_denoise_strength = normalized.pre_denoise_strength > 0
        ? clampInt(normalized.pre_denoise_strength, 1, 100)
        : 30;
    normalized.mbqp_strength = clampInt(normalized.mbqp_strength, 0, 100);
    normalized.mbqp_block_size = normalized.mbqp_block_size > 0
        ? normalized.mbqp_block_size
        : 16;
    normalized.target_output_ratio_permille = normalized.target_output_ratio_permille > 0
        ? clampInt(normalized.target_output_ratio_permille, 1, 1000)
        : 100;
    if (normalized.fallback_profile == 0) {
        normalized.fallback_profile = MFX50_PROFILE_COMPRESS_85_PROBE;
    }
    if (normalized.aggressive_profile == 0) {
        normalized.aggressive_profile = MFX50_PROFILE_COMPRESS_90_PROBE_B;
    }

    return normalized;
}

MFX50RT_AlgoCaps buildAlgoCaps() {
    MFX50RT_AlgoCaps caps = {};
    caps.struct_size = sizeof(caps);
    caps.supports_preprocess = 1;
    caps.supports_scene_analyzer = 1;
    caps.supports_adaptive_qp = 0;
    caps.supports_mbqp = 0;
    caps.supports_hevc_mbqp = 0;
    caps.supports_runtime_qp_ctrl = 0;
    return caps;
}

int activeAlgoFlags(const MFX50RT_AlgoConfig& cfg) {
    int flags = 0;
    if (cfg.enable_preprocess &&
        (cfg.enable_smooth_scale || cfg.enable_pre_denoise)) {
        flags |= MFX50RT_ALGO_FLAG_PREPROCESS;
        if (cfg.enable_smooth_scale) flags |= MFX50RT_ALGO_FLAG_SMOOTH_SCALE;
        if (cfg.enable_pre_denoise) flags |= MFX50RT_ALGO_FLAG_PRE_DENOISE;
    }
    if (cfg.enable_scene_analyzer) flags |= MFX50RT_ALGO_FLAG_SCENE_ANALYZER;
    if (cfg.enable_adaptive_profile) flags |= MFX50RT_ALGO_FLAG_ADAPTIVE_PROFILE;
    if (cfg.enable_adaptive_qp) flags |= MFX50RT_ALGO_FLAG_ADAPTIVE_QP;
    if (cfg.enable_mbqp) flags |= MFX50RT_ALGO_FLAG_MBQP;
    return flags;
}

} // namespace mfx50rt
