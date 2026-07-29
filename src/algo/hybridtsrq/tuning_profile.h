#pragma once

#include "hybridtsrq_types.h"
#include "mfx50rt.h"

#include <string>

namespace mfx50rt::hybridtsrq {

struct ClassQpTable {
    std::string name = "default";

    int strong_roi_delta = -2;
    int strong_roi_min_qp = 30;
    int strong_roi_max_qp = 40;

    int weak_roi_delta = 0;
    int weak_roi_min_qp = 32;
    int weak_roi_max_qp = 42;

    int edge_texture_roi_delta = 0;
    int edge_texture_roi_min_qp = 36;
    int edge_texture_roi_max_qp = 42;

    int high_texture_background_delta = 4;
    int high_texture_background_min_qp = 40;
    int high_texture_background_max_qp = 44;

    int hard_scene_background_delta = 4;
    int hard_scene_background_min_qp = 40;
    int hard_scene_background_max_qp = 44;

    int transition_delta = 4;
    int transition_min_qp = 40;
    int transition_max_qp = 46;

    int normal_background_delta = 8;
    int normal_background_min_qp = 44;
    int normal_background_max_qp = 48;

    int flat_background_qp = 48;
    int frame_anchor_offset = 0;
    int smoothing_max_delta = 8;
    int transition_expand_blocks = 0;
};

struct HybridTsrqQpTuning {
    std::string profile_name = "target_90_ssim_guard";
    int target90_level = 0;

    int base_qp_offset = 0;
    int temporal_i_delta = -2;
    int temporal_p_delta = 1;
    int temporal_b_delta = 4;

    int strong_roi_delta = -6;
    int weak_roi_delta = -4;
    int transition_delta = -2;
    int normal_background_delta = 4;
    int flat_background_delta = 8;
    int enable_roi_protection = 1;
    int enable_transition_protection = 1;
    int enable_qp_smoothing = 1;
    int spatial_passthrough = 0;
    int absolute_region_qp = 0;
    int absolute_strong_roi_qp = 36;
    int absolute_weak_roi_qp = 38;
    int absolute_transition_qp = 44;
    int absolute_background_qp = 48;
    int absolute_flat_background_qp = 51;
    int simple_class_policy = 0;
    int disable_edge_texture_roi = 0;
    int disable_proxy_quality_guard = 0;

    int class_strong_roi_delta = -2;
    int class_strong_roi_min_qp = 30;
    int class_strong_roi_max_qp = 40;
    int class_weak_roi_delta = 0;
    int class_weak_roi_min_qp = 32;
    int class_weak_roi_max_qp = 42;
    int class_edge_texture_roi_delta = 0;
    int class_edge_texture_roi_min_qp = 36;
    int class_edge_texture_roi_max_qp = 42;
    int class_high_texture_background_delta = 4;
    int class_high_texture_background_min_qp = 40;
    int class_high_texture_background_max_qp = 44;
    int class_hard_scene_background_delta = 4;
    int class_hard_scene_background_min_qp = 40;
    int class_hard_scene_background_max_qp = 44;
    int class_transition_delta = 4;
    int class_transition_min_qp = 40;
    int class_transition_max_qp = 46;
    int class_normal_background_delta = 8;
    int class_normal_background_min_qp = 44;
    int class_normal_background_max_qp = 48;
    int class_flat_background_qp = 48;

    int roi_qp_min = 22;
    int roi_qp_max = 36;
    int weak_roi_qp_min = 26;
    int weak_roi_qp_max = 39;
    int background_qp_max = 48;
    int flat_background_qp_max = 48;
    int global_qp_max = 51;
    int night_background_qp_max = 40;
    int quality_guard_background_qp_max = 42;

    int max_neighbor_delta = 8;

    int hard_scene_base_qp_relief = 0;
    int hard_scene_background_qp_max = 0;
    int hard_scene_flat_background_qp_max = 0;
    int hard_scene_normal_background_delta = 0;
    int hard_scene_flat_background_delta = 0;

    int selective_hard_scene_guard = 0;
    int dynamic_hard_scene_guard = 1;
    int hard_scene_protect_background = 1;
    int hard_scene_guard_active = 0;
    int hard_scene_important_qp_max = 42;
    int hard_scene_edge_transition_qp_max = 46;
    int hard_scene_selective_background_qp_max = 50;
    int hard_scene_selective_flat_qp_max = 51;
    int hard_scene_apply_to_flat_background = 0;

    int large_roi_relax_enabled = 0;
    float large_roi_ratio_threshold = 0.35f;
    float large_roi_background_max_ratio = 0.15f;
    float large_roi_quality_margin_min_ssim = 0.84f;
    int large_roi_strong_roi_delta = -2;
    int large_roi_weak_roi_delta = 0;
    int large_roi_transition_delta = 2;
    int large_roi_qp_max = 42;
    int large_roi_weak_qp_max = 42;

    ClassQpTable default_class_table;
    ClassQpTable low_risk_static_table;
    ClassQpTable sparse_mid_texture_table;
    ClassQpTable large_roi_table;
    ClassQpTable high_texture_background_table;
    ClassQpTable hard_scene_risk_table;
    ClassQpTable quality_recovery_table;
    ClassQpTable active_class_qp_table;
};

HybridTsrqQpTuning tuning_from_policy(const MFX50RT_AlgoPolicy& policy);
bool is_hard_scene_for_qp_guard(const FastFrameFeatures& features);
void rebuild_class_qp_tables(HybridTsrqQpTuning& tuning);
const ClassQpTable& resolve_class_qp_table(const HybridTsrqQpTuning& tuning,
                                           SceneMode mode);
const char* scene_mode_name(SceneMode mode);

} // namespace mfx50rt::hybridtsrq
