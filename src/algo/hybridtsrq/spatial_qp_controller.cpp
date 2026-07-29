#include "spatial_qp_controller.h"

#include "qp_map.h"

#include <algorithm>
#include <numeric>

namespace mfx50rt::hybridtsrq {

namespace {

void summarize_qp(SpatialQpDecision& out) {
    if (out.mbqp_map.qp.empty()) return;
    auto [min_it, max_it] = std::minmax_element(out.mbqp_map.qp.begin(), out.mbqp_map.qp.end());
    const uint64_t sum = std::accumulate(out.mbqp_map.qp.begin(), out.mbqp_map.qp.end(), uint64_t{0});
    out.spatial_min_qp = *min_it;
    out.spatial_max_qp = *max_it;
    out.spatial_avg_qp = static_cast<int>(sum / out.mbqp_map.qp.size());
}

bool is_low_risk_static_scene(const FastFrameFeatures& fast) {
    return fast.edge_density <= 0.35f &&
           fast.noise_score <= 0.22f &&
           fast.motion_score <= 0.03f &&
           fast.scene_cut_score <= 0.05f;
}

bool is_large_roi_compression_candidate(const RegionBlockStats& regions) {
    if (regions.roi_block_ratio >= 0.90f &&
        regions.true_roi_block_ratio >= 0.50f &&
        regions.background_block_ratio <= 0.05f) {
        return true;
    }
    return regions.roi_block_ratio >= 0.45f &&
           regions.true_roi_block_ratio >= 0.20f &&
           regions.true_roi_block_ratio <= 0.30f &&
           regions.background_block_ratio <= 0.35f;
}

bool is_severe_texture_risk(const FastFrameFeatures& fast,
                            const RegionBlockStats& regions) {
    const bool low_camera_motion =
        fast.motion_score <= 0.03f &&
        fast.scene_cut_score <= 0.05f;
    const bool high_detail_pressure =
        fast.edge_density >= 0.72f &&
        fast.noise_score >= 0.43f;
    const bool texture_background_dominant =
        regions.background_block_ratio >= 0.55f &&
        (regions.high_texture_background_block_ratio >= 0.35f ||
         regions.hard_scene_background_block_ratio >= 0.25f);
    const bool traffic_context =
        regions.true_roi_block_ratio >= 0.02f ||
        regions.foreground_block_ratio >= 0.025f ||
        regions.transition_block_ratio >= 0.05f;
    const bool limited_semantic_target =
        regions.true_roi_block_ratio <= 0.18f &&
        regions.foreground_block_ratio <= 0.20f;
    return low_camera_motion &&
           high_detail_pressure &&
           texture_background_dominant &&
           traffic_context &&
           limited_semantic_target;
}

bool is_sparse_mid_texture_risk(const FastFrameFeatures& fast,
                                const RegionBlockStats& regions) {
    return fast.edge_density >= 0.55f &&
           fast.edge_density <= 0.68f &&
           fast.noise_score >= 0.35f &&
           fast.noise_score <= 0.42f &&
           fast.motion_score <= 0.03f &&
           regions.background_block_ratio >= 0.68f &&
           regions.foreground_block_ratio >= 0.10f &&
           regions.true_roi_block_ratio >= 0.10f &&
           regions.true_roi_block_ratio <= 0.14f;
}

bool is_moderate_texture_compression_candidate(const FastFrameFeatures& fast,
                                               const RegionBlockStats& regions) {
    return fast.edge_density <= 0.72f &&
           fast.noise_score <= 0.43f &&
           fast.motion_score <= 0.03f &&
           regions.background_block_ratio >= 0.55f &&
           regions.foreground_block_ratio < 0.12f &&
           regions.high_texture_background_block_ratio >= 0.30f;
}

SceneMode select_scene_mode(const FastFrameFeatures& fast,
                            const RegionBlockStats& regions,
                            const QualityState& quality,
                            const HybridTsrqQpTuning& tuning) {
    if (!tuning.simple_class_policy) return SceneMode::Default;
    const bool quality_active = quality.active &&
        !(quality.proxy_mode && tuning.disable_proxy_quality_guard);
    if (quality_active) return SceneMode::QualityRecovery;
    if (is_severe_texture_risk(fast, regions)) return SceneMode::HardSceneRisk;
    if (is_large_roi_compression_candidate(regions)) return SceneMode::LargeRoi;
    if (is_moderate_texture_compression_candidate(fast, regions)) {
        return SceneMode::HighTextureBackground;
    }
    if (is_low_risk_static_scene(fast)) return SceneMode::LowRiskStatic;
    if (is_sparse_mid_texture_risk(fast, regions)) return SceneMode::SparseMidTexture;
    return SceneMode::Default;
}

bool use_global_detail_recovery(SceneMode mode,
                                const RegionBlockStats& regions,
                                const HybridTsrqQpTuning& tuning) {
    if (!tuning.simple_class_policy) return false;
    if (mode == SceneMode::HardSceneRisk) return true;
    return mode == SceneMode::SparseMidTexture &&
           regions.foreground_block_ratio >= 0.10f &&
           regions.true_roi_block_ratio >= 0.08f;
}

bool can_hold_global_detail_recovery(const FastFrameFeatures& fast,
                                     const RegionBlockStats& regions,
                                     const HybridTsrqQpTuning& tuning) {
    if (!tuning.simple_class_policy) return false;
    return fast.motion_score <= 0.03f &&
           fast.scene_cut_score <= 0.05f &&
           fast.edge_density >= 0.70f &&
           fast.noise_score >= 0.42f &&
           regions.background_block_ratio >= 0.55f &&
           regions.true_roi_block_ratio <= 0.18f &&
           (regions.high_texture_background_block_ratio >= 0.30f ||
            regions.hard_scene_background_block_ratio >= 0.20f);
}

void resolve_simple_class_table(SceneMode mode, HybridTsrqQpTuning& tuning) {
    if (!tuning.simple_class_policy) return;
    tuning.active_class_qp_table = resolve_class_qp_table(tuning, mode);
}

} // namespace

void SpatialQpController::configure(const MFX50RT_AlgoPolicy& policy,
                                    const MFX50RT_Capabilities& caps,
                                    const MFX50RT_EffectiveConfig& effective) {
    strategy_ = effective.effective_strategy;
    spatial_enabled_ = policy.enable_spatial_qp != 0;
    mbqp_enabled_ = policy.enable_mbqp != 0 && caps.supports_mbqp != 0;
    roi_enabled_ = policy.enable_roi != 0 && caps.supports_roi_delta_qp != 0;
    max_roi_regions_ = caps.max_roi_regions > 0 ? caps.max_roi_regions : 0;
    tuning_ = tuning_from_policy(policy);
}

SpatialQpDecision SpatialQpController::decide(const FastFrameFeatures& fast,
                                              const RoiAnalysisResult& roi,
                                              const TemporalQpDecision& temporal,
                                              const QualityState& quality,
                                              int width,
                                              int height) const {
    SpatialQpDecision out;
    if (!spatial_enabled_ || width <= 0 || height <= 0) {
        out.spatial_min_qp = temporal.frame_anchor_qp;
        out.spatial_max_qp = temporal.frame_anchor_qp;
        out.spatial_avg_qp = temporal.frame_anchor_qp;
        return out;
    }

    if (strategy_ == MFX50RT_STRATEGY_MBQP_CQP && mbqp_enabled_) {
        CtuMap ctu = build_ctu_importance_map(roi, fast, width, height, 64);
        RegionBlockStats region_stats = summarize_region_blocks(ctu, roi);
        auto publish_roi_boxes = [&]() {
            out.roi_boxes = roi.objects;
            std::sort(out.roi_boxes.begin(), out.roi_boxes.end(),
                      [](const RoiBox& a, const RoiBox& b) {
                          return a.confidence > b.confidence;
                      });
            const size_t cap = max_roi_regions_ > 0
                ? static_cast<size_t>(max_roi_regions_)
                : static_cast<size_t>(32);
            if (out.roi_boxes.size() > cap) {
                out.roi_boxes.resize(cap);
            }
            out.has_roi = !out.roi_boxes.empty();
        };
        HybridTsrqQpTuning frame_tuning = tuning_;
        frame_tuning.active_class_qp_table = frame_tuning.default_class_table;
        out.hard_scene_like = is_hard_scene_for_qp_guard(fast);
        out.scene_mode = select_scene_mode(fast, region_stats, quality, frame_tuning);
        resolve_simple_class_table(out.scene_mode, frame_tuning);
        out.class_qp_table_name = frame_tuning.active_class_qp_table.name;
        out.class_qp_table_overwrite_count = frame_tuning.simple_class_policy ? 1 : 0;
        if (frame_tuning.target90_level > 0 &&
            frame_tuning.dynamic_hard_scene_guard &&
            is_hard_scene_for_qp_guard(fast)) {
            if (frame_tuning.selective_hard_scene_guard) {
                frame_tuning.hard_scene_guard_active = 1;
                if (fast.noise_score >= 0.48f ||
                    fast.edge_density >= 0.78f ||
                    (fast.noise_score >= 0.36f && fast.edge_density >= 0.58f)) {
                    const bool near_all_roi = region_stats.roi_block_ratio > 0.50f &&
                                              region_stats.background_block_ratio < 0.15f;
                    frame_tuning.hard_scene_important_qp_max =
                        near_all_roi ? 44 : 42;
                    frame_tuning.hard_scene_edge_transition_qp_max = 42;
                    if (frame_tuning.hard_scene_protect_background) {
                        frame_tuning.hard_scene_selective_background_qp_max = 44;
                        frame_tuning.hard_scene_selective_flat_qp_max = 44;
                        frame_tuning.hard_scene_apply_to_flat_background = 1;
                    }
                } else if ((fast.noise_score >= 0.35f || fast.edge_density >= 0.50f) &&
                           region_stats.roi_block_ratio > 0.20f) {
                    frame_tuning.hard_scene_important_qp_max =
                        region_stats.roi_block_ratio > 0.50f ? 44 : 42;
                    frame_tuning.hard_scene_edge_transition_qp_max = 44;
                    if (frame_tuning.hard_scene_protect_background) {
                        frame_tuning.hard_scene_selective_background_qp_max = 46;
                        frame_tuning.hard_scene_selective_flat_qp_max = 48;
                    }
                    frame_tuning.hard_scene_apply_to_flat_background = 0;
                } else if (fast.noise_score >= 0.35f || fast.edge_density >= 0.50f) {
                    frame_tuning.hard_scene_important_qp_max =
                        std::min(frame_tuning.hard_scene_important_qp_max, 42);
                    frame_tuning.hard_scene_edge_transition_qp_max =
                        std::min(frame_tuning.hard_scene_edge_transition_qp_max, 44);
                    if (frame_tuning.hard_scene_protect_background) {
                        frame_tuning.hard_scene_selective_background_qp_max =
                            std::min(frame_tuning.hard_scene_selective_background_qp_max, 46);
                        frame_tuning.hard_scene_selective_flat_qp_max =
                            std::min(frame_tuning.hard_scene_selective_flat_qp_max, 48);
                    }
                    frame_tuning.hard_scene_apply_to_flat_background = 0;
                }
            } else if (frame_tuning.hard_scene_background_qp_max > 0) {
                frame_tuning.background_qp_max = std::min(frame_tuning.background_qp_max,
                                                          frame_tuning.hard_scene_background_qp_max);
            }
            if (!frame_tuning.selective_hard_scene_guard &&
                frame_tuning.hard_scene_flat_background_qp_max > 0) {
                frame_tuning.flat_background_qp_max =
                    std::min(frame_tuning.flat_background_qp_max,
                             frame_tuning.hard_scene_flat_background_qp_max);
            }
            if (!frame_tuning.selective_hard_scene_guard &&
                frame_tuning.hard_scene_normal_background_delta > 0) {
                frame_tuning.normal_background_delta =
                    std::min(frame_tuning.normal_background_delta,
                             frame_tuning.hard_scene_normal_background_delta);
            }
            if (!frame_tuning.selective_hard_scene_guard &&
                frame_tuning.hard_scene_flat_background_delta > 0) {
                frame_tuning.flat_background_delta =
                    std::min(frame_tuning.flat_background_delta,
                             frame_tuning.hard_scene_flat_background_delta);
            }
        }
        out.hard_guard_active =
            frame_tuning.hard_scene_guard_active != 0 ||
            out.scene_mode == SceneMode::HardSceneRisk;
        bool global_detail_recovery =
            use_global_detail_recovery(out.scene_mode, region_stats, frame_tuning);
        if (global_detail_recovery) {
            global_detail_recovery_hold_frames_ = 24;
        } else if (global_detail_recovery_hold_frames_ > 0 &&
                   can_hold_global_detail_recovery(fast, region_stats, frame_tuning)) {
            global_detail_recovery = true;
            --global_detail_recovery_hold_frames_;
        } else if (global_detail_recovery_hold_frames_ > 0) {
            --global_detail_recovery_hold_frames_;
        }
        if (global_detail_recovery) {
            out.region_stats = region_stats;
            publish_roi_boxes();
            out.spatial_min_qp = temporal.frame_anchor_qp;
            out.spatial_max_qp = temporal.frame_anchor_qp;
            out.spatial_avg_qp = temporal.frame_anchor_qp;
            return out;
        }
        out.mbqp_map = ctu_qp_to_16x16_mbqp(ctu,
                                            width,
                                            height,
                                            temporal.frame_anchor_qp,
                                            quality,
                                            frame_tuning);
        out.has_mbqp = !out.mbqp_map.qp.empty();
        out.smoothing_changed_qp_avg = out.mbqp_map.smoothing_changed_qp_avg;
        out.qp_stats = summarize_qp_distribution(out.mbqp_map);
        out.region_stats = region_stats;
        publish_roi_boxes();
        summarize_qp(out);
        return out;
    }

    if (strategy_ == MFX50RT_STRATEGY_ROI_DELTA_QP && roi_enabled_) {
        out.roi_boxes = roi.objects;
        std::sort(out.roi_boxes.begin(), out.roi_boxes.end(),
                  [](const RoiBox& a, const RoiBox& b) {
                      return a.confidence > b.confidence;
                  });
        if (max_roi_regions_ > 0 &&
            out.roi_boxes.size() > static_cast<size_t>(max_roi_regions_)) {
            out.roi_boxes.resize(static_cast<size_t>(max_roi_regions_));
        }
        out.has_roi = !out.roi_boxes.empty();
    }

    out.spatial_min_qp = temporal.frame_anchor_qp;
    out.spatial_max_qp = temporal.frame_anchor_qp;
    out.spatial_avg_qp = temporal.frame_anchor_qp;
    return out;
}

} // namespace mfx50rt::hybridtsrq
