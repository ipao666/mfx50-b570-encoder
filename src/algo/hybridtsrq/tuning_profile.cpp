#include "tuning_profile.h"

#include <algorithm>
#include <cctype>
#include <regex>

namespace mfx50rt::hybridtsrq {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
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

void override_int(const std::string& json, const char* key, int* value) {
    int parsed = 0;
    if (json_int(json, key, &parsed)) *value = parsed;
}

void override_float(const std::string& json, const char* key, float* value) {
    float parsed = 0.0f;
    if (json_float(json, key, &parsed)) *value = parsed;
}

std::string policy_profile_name(const MFX50RT_AlgoPolicy& policy) {
    std::string name;
    if (json_string(policy.expert_options_json, "target90_profile", &name) ||
        json_string(policy.expert_options_json, "hybridtsrq_profile", &name) ||
        json_string(policy.expert_options_json, "profile", &name)) {
        return lower(name);
    }
    switch (policy.profile) {
        case MFX50RT_PROFILE_SAFE: return "safe";
        case MFX50RT_PROFILE_BALANCED: return "balanced";
        case MFX50RT_PROFILE_MAX_COMPRESSION: return "max_compression";
        case MFX50RT_PROFILE_LOW_LATENCY: return "low_latency";
        case MFX50RT_PROFILE_CUSTOM: return "custom";
        default: return "target_90_ssim_guard";
    }
}

ClassQpTable class_table_from_tuning(const HybridTsrqQpTuning& t,
                                     const char* name) {
    ClassQpTable table;
    table.name = name;
    table.strong_roi_delta = t.class_strong_roi_delta;
    table.strong_roi_min_qp = t.class_strong_roi_min_qp;
    table.strong_roi_max_qp = t.class_strong_roi_max_qp;
    table.weak_roi_delta = t.class_weak_roi_delta;
    table.weak_roi_min_qp = t.class_weak_roi_min_qp;
    table.weak_roi_max_qp = t.class_weak_roi_max_qp;
    table.edge_texture_roi_delta = t.class_edge_texture_roi_delta;
    table.edge_texture_roi_min_qp = t.class_edge_texture_roi_min_qp;
    table.edge_texture_roi_max_qp = t.class_edge_texture_roi_max_qp;
    table.high_texture_background_delta = t.class_high_texture_background_delta;
    table.high_texture_background_min_qp = t.class_high_texture_background_min_qp;
    table.high_texture_background_max_qp = t.class_high_texture_background_max_qp;
    table.hard_scene_background_delta = t.class_hard_scene_background_delta;
    table.hard_scene_background_min_qp = t.class_hard_scene_background_min_qp;
    table.hard_scene_background_max_qp = t.class_hard_scene_background_max_qp;
    table.transition_delta = t.class_transition_delta;
    table.transition_min_qp = t.class_transition_min_qp;
    table.transition_max_qp = t.class_transition_max_qp;
    table.normal_background_delta = t.class_normal_background_delta;
    table.normal_background_min_qp = t.class_normal_background_min_qp;
    table.normal_background_max_qp = t.class_normal_background_max_qp;
    table.flat_background_qp = t.class_flat_background_qp;
    table.smoothing_max_delta = t.max_neighbor_delta;
    return table;
}

void apply_low_risk_static_table(ClassQpTable& table) {
    table.strong_roi_delta = std::max(table.strong_roi_delta, 2);
    table.strong_roi_max_qp = std::max(table.strong_roi_max_qp, 46);
    table.weak_roi_delta = std::max(table.weak_roi_delta, 4);
    table.weak_roi_max_qp = std::max(table.weak_roi_max_qp, 48);
    table.edge_texture_roi_delta = std::max(table.edge_texture_roi_delta, 6);
    table.edge_texture_roi_max_qp = std::max(table.edge_texture_roi_max_qp, 48);
    table.high_texture_background_delta =
        std::max(table.high_texture_background_delta, 10);
    table.high_texture_background_max_qp =
        std::max(table.high_texture_background_max_qp, 51);
    table.hard_scene_background_delta =
        std::max(table.hard_scene_background_delta, 10);
    table.hard_scene_background_max_qp =
        std::max(table.hard_scene_background_max_qp, 51);
    table.transition_delta = std::max(table.transition_delta, 10);
    table.transition_max_qp = std::max(table.transition_max_qp, 50);
    table.normal_background_delta = std::max(table.normal_background_delta, 12);
    table.normal_background_max_qp = std::max(table.normal_background_max_qp, 51);
    table.flat_background_qp = std::max(table.flat_background_qp, 51);
}

void apply_large_roi_table(ClassQpTable& table) {
    table.strong_roi_delta = std::max(table.strong_roi_delta, 2);
    table.strong_roi_max_qp = std::max(table.strong_roi_max_qp, 44);
    table.weak_roi_delta = std::max(table.weak_roi_delta, 4);
    table.weak_roi_max_qp = std::max(table.weak_roi_max_qp, 46);
    table.edge_texture_roi_delta = std::max(table.edge_texture_roi_delta, 4);
    table.edge_texture_roi_max_qp = std::max(table.edge_texture_roi_max_qp, 46);
    table.transition_delta = std::max(table.transition_delta, 8);
    table.transition_max_qp = std::max(table.transition_max_qp, 48);
    table.normal_background_delta = std::max(table.normal_background_delta, 10);
    table.normal_background_max_qp = std::max(table.normal_background_max_qp, 50);
    table.flat_background_qp = std::max(table.flat_background_qp, 51);
}

void apply_high_texture_background_table(ClassQpTable& table) {
    table.high_texture_background_delta =
        std::max(table.high_texture_background_delta, 6);
    table.high_texture_background_max_qp =
        std::max(table.high_texture_background_max_qp, 46);
    table.hard_scene_background_delta =
        std::max(table.hard_scene_background_delta, 6);
    table.hard_scene_background_max_qp =
        std::max(table.hard_scene_background_max_qp, 46);
    table.transition_delta = std::max(table.transition_delta, 8);
    table.transition_max_qp = std::max(table.transition_max_qp, 48);
    table.flat_background_qp = std::max(table.flat_background_qp, 51);
}

void apply_hard_scene_risk_table(ClassQpTable& table) {
    table.edge_texture_roi_delta =
        std::min(table.edge_texture_roi_delta, -1);
    table.edge_texture_roi_max_qp =
        std::min(table.edge_texture_roi_max_qp, 39);
    table.high_texture_background_delta =
        std::max(table.high_texture_background_delta, 2);
    table.high_texture_background_min_qp =
        std::max(table.high_texture_background_min_qp, 40);
    table.high_texture_background_max_qp = 40;
    table.hard_scene_background_delta =
        std::max(table.hard_scene_background_delta, 2);
    table.hard_scene_background_min_qp =
        std::max(table.hard_scene_background_min_qp, 40);
    table.hard_scene_background_max_qp = 40;
    table.transition_delta = std::min(table.transition_delta, 1);
    table.transition_max_qp = std::min(table.transition_max_qp, 40);
    table.normal_background_delta = std::min(table.normal_background_delta, 7);
    table.normal_background_max_qp = std::min(table.normal_background_max_qp, 44);
    table.flat_background_qp = std::min(table.flat_background_qp, 44);
}

void apply_sparse_mid_texture_table(ClassQpTable& table) {
    table.strong_roi_delta = std::min(table.strong_roi_delta, -4);
    table.strong_roi_max_qp = std::min(table.strong_roi_max_qp, 38);
    table.weak_roi_delta = std::min(table.weak_roi_delta, -2);
    table.weak_roi_max_qp = std::min(table.weak_roi_max_qp, 39);
    table.edge_texture_roi_delta = std::min(table.edge_texture_roi_delta, -2);
    table.edge_texture_roi_max_qp = std::min(table.edge_texture_roi_max_qp, 38);
    table.high_texture_background_delta =
        std::min(table.high_texture_background_delta, -2);
    table.high_texture_background_max_qp =
        std::min(table.high_texture_background_max_qp, 38);
    table.hard_scene_background_delta =
        std::min(table.hard_scene_background_delta, -2);
    table.hard_scene_background_max_qp =
        std::min(table.hard_scene_background_max_qp, 38);
    table.transition_delta = std::min(table.transition_delta, 0);
    table.transition_max_qp = std::min(table.transition_max_qp, 40);
    table.normal_background_delta = std::min(table.normal_background_delta, 6);
    table.normal_background_max_qp = std::min(table.normal_background_max_qp, 42);
    table.flat_background_qp = std::min(table.flat_background_qp, 42);
}

void apply_v1(HybridTsrqQpTuning& t) {
    t.profile_name = "target90_v1_moderate";
    t.target90_level = 1;
    t.base_qp_offset = 3;
    t.temporal_i_delta = -2;
    t.temporal_p_delta = 1;
    t.temporal_b_delta = 4;
    t.strong_roi_delta = -5;
    t.weak_roi_delta = -3;
    t.transition_delta = -1;
    t.normal_background_delta = 10;
    t.flat_background_delta = 14;
    t.roi_qp_min = 24;
    t.roi_qp_max = 38;
    t.weak_roi_qp_min = 24;
    t.weak_roi_qp_max = 38;
    t.background_qp_max = 48;
    t.flat_background_qp_max = 50;
    t.night_background_qp_max = 42;
    t.quality_guard_background_qp_max = 44;
    t.max_neighbor_delta = 10;
    t.hard_scene_base_qp_relief = 2;
    t.hard_scene_background_qp_max = 40;
    t.hard_scene_flat_background_qp_max = 40;
    t.hard_scene_normal_background_delta = 8;
    t.hard_scene_flat_background_delta = 10;
}

void apply_v2(HybridTsrqQpTuning& t) {
    t.profile_name = "target90_v2_aggressive";
    t.target90_level = 2;
    t.base_qp_offset = 6;
    t.temporal_i_delta = -1;
    t.temporal_p_delta = 2;
    t.temporal_b_delta = 5;
    t.strong_roi_delta = -4;
    t.weak_roi_delta = -2;
    t.transition_delta = 0;
    t.normal_background_delta = 14;
    t.flat_background_delta = 18;
    t.roi_qp_min = 26;
    t.roi_qp_max = 40;
    t.weak_roi_qp_min = 26;
    t.weak_roi_qp_max = 40;
    t.background_qp_max = 50;
    t.flat_background_qp_max = 51;
    t.night_background_qp_max = 44;
    t.quality_guard_background_qp_max = 46;
    t.max_neighbor_delta = 12;
    t.hard_scene_base_qp_relief = 4;
    t.hard_scene_background_qp_max = 40;
    t.hard_scene_flat_background_qp_max = 40;
    t.hard_scene_normal_background_delta = 8;
    t.hard_scene_flat_background_delta = 10;
}

void apply_v3(HybridTsrqQpTuning& t) {
    t.profile_name = "target90_v3_extreme_guarded";
    t.target90_level = 3;
    t.base_qp_offset = 8;
    t.temporal_i_delta = 0;
    t.temporal_p_delta = 3;
    t.temporal_b_delta = 6;
    t.strong_roi_delta = -3;
    t.weak_roi_delta = -1;
    t.transition_delta = 0;
    t.normal_background_delta = 16;
    t.flat_background_delta = 20;
    t.roi_qp_min = 28;
    t.roi_qp_max = 42;
    t.weak_roi_qp_min = 28;
    t.weak_roi_qp_max = 42;
    t.background_qp_max = 51;
    t.flat_background_qp_max = 51;
    t.night_background_qp_max = 46;
    t.quality_guard_background_qp_max = 46;
    t.max_neighbor_delta = 12;
    t.hard_scene_base_qp_relief = 6;
    t.hard_scene_background_qp_max = 40;
    t.hard_scene_flat_background_qp_max = 40;
    t.hard_scene_normal_background_delta = 8;
    t.hard_scene_flat_background_delta = 10;
}

void apply_selective_guard(HybridTsrqQpTuning& t) {
    apply_v2(t);
    t.profile_name = "target90_v2_selective_guard";
    t.hard_scene_background_qp_max = 0;
    t.hard_scene_flat_background_qp_max = 0;
    t.hard_scene_normal_background_delta = 0;
    t.hard_scene_flat_background_delta = 0;
    t.selective_hard_scene_guard = 1;
    t.hard_scene_important_qp_max = 42;
    t.hard_scene_edge_transition_qp_max = 46;
    t.hard_scene_selective_background_qp_max = 50;
    t.hard_scene_selective_flat_qp_max = 51;
    t.hard_scene_apply_to_flat_background = 0;
}

void apply_roi_relaxed(HybridTsrqQpTuning& t) {
    apply_selective_guard(t);
    t.profile_name = "target90_v2_roi_relaxed";
    t.normal_background_delta = 18;
    t.flat_background_delta = 22;
    t.strong_roi_delta = -3;
    t.weak_roi_delta = -1;
    t.transition_delta = 1;
    t.large_roi_relax_enabled = 1;
    t.large_roi_ratio_threshold = 0.35f;
    t.large_roi_background_max_ratio = 0.15f;
    t.large_roi_quality_margin_min_ssim = 0.84f;
    t.large_roi_strong_roi_delta = 2;
    t.large_roi_weak_roi_delta = 2;
    t.large_roi_transition_delta = 3;
    t.large_roi_qp_max = 44;
    t.large_roi_weak_qp_max = 48;
}

void apply_final_candidate(HybridTsrqQpTuning& t) {
    apply_selective_guard(t);
    t.profile_name = "target90_final_candidate";
    t.target90_level = 4;
    t.base_qp_offset = 7;
    t.temporal_i_delta = -1;
    t.temporal_p_delta = 2;
    t.temporal_b_delta = 5;
    t.strong_roi_delta = -3;
    t.weak_roi_delta = -1;
    t.transition_delta = 1;
    t.normal_background_delta = 16;
    t.flat_background_delta = 22;
    t.roi_qp_min = 26;
    t.roi_qp_max = 42;
    t.weak_roi_qp_min = 26;
    t.weak_roi_qp_max = 42;
    t.background_qp_max = 51;
    t.flat_background_qp_max = 51;
    t.night_background_qp_max = 46;
    t.quality_guard_background_qp_max = 46;
    t.max_neighbor_delta = 12;
    t.hard_scene_base_qp_relief = 0;
    t.dynamic_hard_scene_guard = 0;
    t.hard_scene_protect_background = 0;
    t.hard_scene_important_qp_max = 42;
    t.hard_scene_edge_transition_qp_max = 46;
    t.hard_scene_selective_background_qp_max = 51;
    t.hard_scene_selective_flat_qp_max = 51;
    t.hard_scene_apply_to_flat_background = 0;
    t.large_roi_relax_enabled = 1;
    t.large_roi_ratio_threshold = 0.35f;
    t.large_roi_background_max_ratio = 2.0f;
    t.large_roi_quality_margin_min_ssim = 0.0f;
    t.large_roi_strong_roi_delta = -2;
    t.large_roi_weak_roi_delta = 0;
    t.large_roi_transition_delta = 2;
    t.large_roi_qp_max = 42;
    t.large_roi_weak_qp_max = 42;
}

void disable_hard_scene_guard(HybridTsrqQpTuning& t) {
    t.hard_scene_base_qp_relief = 0;
    t.hard_scene_background_qp_max = 0;
    t.hard_scene_flat_background_qp_max = 0;
    t.hard_scene_normal_background_delta = 0;
    t.hard_scene_flat_background_delta = 0;
    t.selective_hard_scene_guard = 0;
    t.dynamic_hard_scene_guard = 0;
    t.hard_scene_protect_background = 0;
    t.hard_scene_guard_active = 0;
}

void apply_legacy_v2_temporal(HybridTsrqQpTuning& t) {
    apply_v2(t);
    t.profile_name = "legacy_v2_temporal";
    t.spatial_passthrough = 1;
    t.enable_qp_smoothing = 0;
    disable_hard_scene_guard(t);
}

void apply_legacy_212_ctu(HybridTsrqQpTuning& t) {
    t.profile_name = "legacy_212_ctu";
    t.target90_level = 1;
    t.base_qp_offset = 0;
    t.temporal_i_delta = -2;
    t.temporal_p_delta = 1;
    t.temporal_b_delta = 4;
    t.absolute_region_qp = 1;
    t.absolute_strong_roi_qp = 26;
    t.absolute_weak_roi_qp = 32;
    t.absolute_transition_qp = 32;
    t.absolute_background_qp = 42;
    t.absolute_flat_background_qp = 42;
    t.background_qp_max = 42;
    t.flat_background_qp_max = 42;
    t.global_qp_max = 51;
    t.max_neighbor_delta = 8;
    disable_hard_scene_guard(t);
}

void apply_simple_roi_background(HybridTsrqQpTuning& t) {
    t.profile_name = "simple_roi_background";
    t.target90_level = 1;
    t.base_qp_offset = 0;
    t.temporal_i_delta = -1;
    t.temporal_p_delta = 2;
    t.temporal_b_delta = 5;
    t.absolute_region_qp = 1;
    t.absolute_strong_roi_qp = 36;
    t.absolute_weak_roi_qp = 38;
    t.absolute_transition_qp = 44;
    t.absolute_background_qp = 48;
    t.absolute_flat_background_qp = 51;
    t.background_qp_max = 51;
    t.flat_background_qp_max = 51;
    t.global_qp_max = 51;
    t.enable_qp_smoothing = 0;
    t.large_roi_relax_enabled = 0;
    disable_hard_scene_guard(t);
}

void apply_simple_class(HybridTsrqQpTuning& t, const char* name) {
    t.profile_name = name;
    t.target90_level = 5;
    t.base_qp_offset = 4;
    t.temporal_i_delta = -1;
    t.temporal_p_delta = 2;
    t.temporal_b_delta = 5;
    t.simple_class_policy = 1;
    t.enable_qp_smoothing = 0;
    t.disable_proxy_quality_guard = 1;
    t.background_qp_max = 51;
    t.flat_background_qp_max = 51;
    t.global_qp_max = 51;
    t.large_roi_relax_enabled = 0;
    disable_hard_scene_guard(t);
}

} // namespace

void rebuild_class_qp_tables(HybridTsrqQpTuning& tuning) {
    tuning.default_class_table = class_table_from_tuning(tuning, "default");

    tuning.low_risk_static_table = tuning.default_class_table;
    tuning.low_risk_static_table.name = "low_risk_static";
    apply_low_risk_static_table(tuning.low_risk_static_table);

    tuning.sparse_mid_texture_table = tuning.default_class_table;
    tuning.sparse_mid_texture_table.name = "sparse_mid_texture";
    apply_sparse_mid_texture_table(tuning.sparse_mid_texture_table);

    tuning.large_roi_table = tuning.default_class_table;
    tuning.large_roi_table.name = "large_roi";
    apply_large_roi_table(tuning.large_roi_table);

    tuning.high_texture_background_table = tuning.default_class_table;
    tuning.high_texture_background_table.name = "high_texture_background";
    apply_high_texture_background_table(tuning.high_texture_background_table);

    tuning.hard_scene_risk_table = tuning.default_class_table;
    tuning.hard_scene_risk_table.name = "hard_scene_risk";
    apply_hard_scene_risk_table(tuning.hard_scene_risk_table);

    tuning.quality_recovery_table = tuning.default_class_table;
    tuning.quality_recovery_table.name = "quality_recovery";
    tuning.active_class_qp_table = tuning.default_class_table;
}

const ClassQpTable& resolve_class_qp_table(const HybridTsrqQpTuning& tuning,
                                           SceneMode mode) {
    switch (mode) {
        case SceneMode::QualityRecovery:
            return tuning.quality_recovery_table;
        case SceneMode::HardSceneRisk:
            return tuning.hard_scene_risk_table;
        case SceneMode::LargeRoi:
            return tuning.large_roi_table;
        case SceneMode::HighTextureBackground:
            return tuning.high_texture_background_table;
        case SceneMode::LowRiskStatic:
            return tuning.low_risk_static_table;
        case SceneMode::SparseMidTexture:
            return tuning.sparse_mid_texture_table;
        case SceneMode::Default:
        default:
            return tuning.default_class_table;
    }
}

const char* scene_mode_name(SceneMode mode) {
    switch (mode) {
        case SceneMode::QualityRecovery: return "quality_recovery";
        case SceneMode::HardSceneRisk: return "hard_scene_risk";
        case SceneMode::LargeRoi: return "large_roi";
        case SceneMode::HighTextureBackground: return "high_texture_background";
        case SceneMode::LowRiskStatic: return "low_risk_static";
        case SceneMode::SparseMidTexture: return "sparse_mid_texture";
        case SceneMode::Default:
        default: return "default";
    }
}

HybridTsrqQpTuning tuning_from_policy(const MFX50RT_AlgoPolicy& policy) {
    HybridTsrqQpTuning tuning;
    const std::string profile = policy_profile_name(policy);
    if (profile == "target90_v1_moderate" || profile == "target_90_v1_moderate") {
        apply_v1(tuning);
    } else if (profile == "target90_v2_aggressive" || profile == "target_90_v2_aggressive") {
        apply_v2(tuning);
    } else if (profile == "target90_v2_selective_guard" ||
               profile == "target_90_v2_selective_guard") {
        apply_selective_guard(tuning);
    } else if (profile == "target90_v2_roi_relaxed" ||
               profile == "target_90_v2_roi_relaxed") {
        apply_roi_relaxed(tuning);
    } else if (profile == "target90_final_candidate" ||
               profile == "target_90_final_candidate") {
        apply_final_candidate(tuning);
    } else if (profile == "legacy_v2_temporal" ||
               profile == "legacy_v2_temporal_like") {
        apply_legacy_v2_temporal(tuning);
    } else if (profile == "legacy_212_ctu" ||
               profile == "legacy_212_ctu_like") {
        apply_legacy_212_ctu(tuning);
    } else if (profile == "simple_roi_background") {
        apply_simple_roi_background(tuning);
    } else if (profile == "target90_simple_class" ||
               profile == "target_90_simple_class") {
        apply_simple_class(tuning, "target90_simple_class");
    } else if (profile == "target90_simple_class_b0" ||
               profile == "target_90_simple_class_b0") {
        apply_simple_class(tuning, "target90_simple_class_b0");
    } else if (profile == "target90_simple_class_b1" ||
               profile == "target_90_simple_class_b1") {
        apply_simple_class(tuning, "target90_simple_class_b1");
    } else if (profile == "target90_simple_class_b2" ||
               profile == "target_90_simple_class_b2") {
        apply_simple_class(tuning, "target90_simple_class_b2");
    } else if (profile == "target90_v3_extreme_guarded" ||
               profile == "target_90_v3_extreme_guarded") {
        apply_v3(tuning);
    } else if (profile == "max_compression") {
        apply_v2(tuning);
        tuning.profile_name = "max_compression";
    } else {
        tuning.profile_name = profile;
    }

    const std::string expert = policy.expert_options_json;
    override_int(expert, "base_qp_offset", &tuning.base_qp_offset);
    override_int(expert, "temporal_i_delta", &tuning.temporal_i_delta);
    override_int(expert, "temporal_p_delta", &tuning.temporal_p_delta);
    override_int(expert, "temporal_b_delta", &tuning.temporal_b_delta);
    override_int(expert, "strong_roi_delta", &tuning.strong_roi_delta);
    override_int(expert, "weak_roi_delta", &tuning.weak_roi_delta);
    override_int(expert, "transition_delta", &tuning.transition_delta);
    override_int(expert, "normal_background_delta", &tuning.normal_background_delta);
    override_int(expert, "flat_background_delta", &tuning.flat_background_delta);
    override_int(expert, "enable_roi_protection", &tuning.enable_roi_protection);
    override_int(expert, "enable_transition_protection", &tuning.enable_transition_protection);
    override_int(expert, "enable_qp_smoothing", &tuning.enable_qp_smoothing);
    override_int(expert, "spatial_passthrough", &tuning.spatial_passthrough);
    override_int(expert, "absolute_region_qp", &tuning.absolute_region_qp);
    override_int(expert, "absolute_strong_roi_qp", &tuning.absolute_strong_roi_qp);
    override_int(expert, "absolute_weak_roi_qp", &tuning.absolute_weak_roi_qp);
    override_int(expert, "absolute_transition_qp", &tuning.absolute_transition_qp);
    override_int(expert, "absolute_background_qp", &tuning.absolute_background_qp);
    override_int(expert, "absolute_flat_background_qp", &tuning.absolute_flat_background_qp);
    override_int(expert, "simple_class_policy", &tuning.simple_class_policy);
    override_int(expert, "disable_edge_texture_roi", &tuning.disable_edge_texture_roi);
    override_int(expert, "disable_proxy_quality_guard", &tuning.disable_proxy_quality_guard);
    override_int(expert, "class_strong_roi_delta", &tuning.class_strong_roi_delta);
    override_int(expert, "class_strong_roi_min_qp", &tuning.class_strong_roi_min_qp);
    override_int(expert, "class_strong_roi_max_qp", &tuning.class_strong_roi_max_qp);
    override_int(expert, "class_weak_roi_delta", &tuning.class_weak_roi_delta);
    override_int(expert, "class_weak_roi_min_qp", &tuning.class_weak_roi_min_qp);
    override_int(expert, "class_weak_roi_max_qp", &tuning.class_weak_roi_max_qp);
    override_int(expert, "class_edge_texture_roi_delta", &tuning.class_edge_texture_roi_delta);
    override_int(expert, "class_edge_texture_roi_min_qp", &tuning.class_edge_texture_roi_min_qp);
    override_int(expert, "class_edge_texture_roi_max_qp", &tuning.class_edge_texture_roi_max_qp);
    override_int(expert, "class_high_texture_background_delta", &tuning.class_high_texture_background_delta);
    override_int(expert, "class_high_texture_background_min_qp", &tuning.class_high_texture_background_min_qp);
    override_int(expert, "class_high_texture_background_max_qp", &tuning.class_high_texture_background_max_qp);
    override_int(expert, "class_hard_scene_background_delta", &tuning.class_hard_scene_background_delta);
    override_int(expert, "class_hard_scene_background_min_qp", &tuning.class_hard_scene_background_min_qp);
    override_int(expert, "class_hard_scene_background_max_qp", &tuning.class_hard_scene_background_max_qp);
    override_int(expert, "class_transition_delta", &tuning.class_transition_delta);
    override_int(expert, "class_transition_min_qp", &tuning.class_transition_min_qp);
    override_int(expert, "class_transition_max_qp", &tuning.class_transition_max_qp);
    override_int(expert, "class_normal_background_delta", &tuning.class_normal_background_delta);
    override_int(expert, "class_normal_background_min_qp", &tuning.class_normal_background_min_qp);
    override_int(expert, "class_normal_background_max_qp", &tuning.class_normal_background_max_qp);
    override_int(expert, "class_flat_background_qp", &tuning.class_flat_background_qp);
    override_int(expert, "roi_qp_min", &tuning.roi_qp_min);
    override_int(expert, "roi_qp_max", &tuning.roi_qp_max);
    override_int(expert, "max_qp", &tuning.global_qp_max);
    override_int(expert, "global_qp_max", &tuning.global_qp_max);
    override_int(expert, "background_qp_max", &tuning.background_qp_max);
    override_int(expert, "flat_background_qp_max", &tuning.flat_background_qp_max);
    override_int(expert, "night_background_qp_max", &tuning.night_background_qp_max);
    override_int(expert, "quality_guard_background_qp_max", &tuning.quality_guard_background_qp_max);
    override_int(expert, "max_neighbor_delta", &tuning.max_neighbor_delta);
    override_int(expert, "hard_scene_base_qp_relief", &tuning.hard_scene_base_qp_relief);
    override_int(expert, "hard_scene_background_qp_max", &tuning.hard_scene_background_qp_max);
    override_int(expert, "hard_scene_flat_background_qp_max", &tuning.hard_scene_flat_background_qp_max);
    override_int(expert, "hard_scene_normal_background_delta", &tuning.hard_scene_normal_background_delta);
    override_int(expert, "hard_scene_flat_background_delta", &tuning.hard_scene_flat_background_delta);
    override_int(expert, "selective_hard_scene_guard", &tuning.selective_hard_scene_guard);
    override_int(expert, "dynamic_hard_scene_guard", &tuning.dynamic_hard_scene_guard);
    override_int(expert, "hard_scene_protect_background", &tuning.hard_scene_protect_background);
    override_int(expert, "important_max_qp", &tuning.hard_scene_important_qp_max);
    override_int(expert, "edge_transition_max_qp", &tuning.hard_scene_edge_transition_qp_max);
    override_int(expert, "hard_scene_selective_background_qp_max", &tuning.hard_scene_selective_background_qp_max);
    override_int(expert, "hard_scene_selective_flat_qp_max", &tuning.hard_scene_selective_flat_qp_max);
    override_int(expert, "apply_to_flat_background", &tuning.hard_scene_apply_to_flat_background);
    override_int(expert, "large_roi_relax_enabled", &tuning.large_roi_relax_enabled);
    override_float(expert, "large_roi_ratio_threshold", &tuning.large_roi_ratio_threshold);
    override_float(expert, "large_roi_background_max_ratio", &tuning.large_roi_background_max_ratio);
    override_float(expert, "large_roi_quality_margin_min_ssim", &tuning.large_roi_quality_margin_min_ssim);
    override_int(expert, "large_roi_strong_delta", &tuning.large_roi_strong_roi_delta);
    override_int(expert, "large_roi_weak_delta", &tuning.large_roi_weak_roi_delta);
    override_int(expert, "large_roi_transition_delta", &tuning.large_roi_transition_delta);
    override_int(expert, "large_roi_qp_max", &tuning.large_roi_qp_max);
    override_int(expert, "large_roi_weak_qp_max", &tuning.large_roi_weak_qp_max);
    int enable_hard_scene_guard = 1;
    if (json_int(expert, "enable_hard_scene_guard", &enable_hard_scene_guard) &&
        enable_hard_scene_guard == 0) {
        disable_hard_scene_guard(tuning);
    }
    int disable_hard_scene_guard_flag = 0;
    if (json_int(expert, "disable_hard_scene_guard", &disable_hard_scene_guard_flag) &&
        disable_hard_scene_guard_flag != 0) {
        disable_hard_scene_guard(tuning);
    }
    int disable_transition_protection = 0;
    if (json_int(expert, "disable_transition_protection", &disable_transition_protection) &&
        disable_transition_protection != 0) {
        tuning.enable_transition_protection = 0;
    }
    int disable_qp_smoothing = 0;
    if (json_int(expert, "disable_qp_smoothing", &disable_qp_smoothing) &&
        disable_qp_smoothing != 0) {
        tuning.enable_qp_smoothing = 0;
    }
    rebuild_class_qp_tables(tuning);
    return tuning;
}

bool is_hard_scene_for_qp_guard(const FastFrameFeatures& features) {
    return features.hard_score >= 0.25f ||
           features.noise_score >= 0.35f ||
           features.motion_score >= 0.30f ||
           features.edge_density >= 0.30f ||
           features.scene_cut_score >= 0.45f;
}

} // namespace mfx50rt::hybridtsrq
