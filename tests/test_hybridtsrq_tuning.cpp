#include "mfx50rt.h"
#include "src/algo/hybridtsrq/hybridtsrq_controller.h"
#include "src/algo/hybridtsrq/quality_guard.h"
#include "src/algo/hybridtsrq/spatial_qp_controller.h"
#include "src/algo/hybridtsrq/temporal_qp_controller.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

using mfx50rt::hybridtsrq::HybridTSRQController;
using mfx50rt::hybridtsrq::HybridTSRQDecision;
using mfx50rt::hybridtsrq::QualityGuard;
using mfx50rt::hybridtsrq::QualityState;
using mfx50rt::hybridtsrq::RoiAnalysisResult;
using mfx50rt::hybridtsrq::SceneMode;
using mfx50rt::hybridtsrq::SpatialQpController;
using mfx50rt::hybridtsrq::TemporalQpDecision;
using mfx50rt::hybridtsrq::TemporalQpController;

namespace {

template <typename T>
void init_api(T& value) {
    std::memset(&value, 0, sizeof(T));
    value.size = sizeof(T);
    value.version = MFX50RT_API_VERSION;
}

HybridTSRQDecision decide_for_profile(const char* profile_name, const char* extra_options = "") {
    MFX50RT_Config cfg{};
    assert(MFX50RT_DefaultConfig(&cfg) == MFX50RT_OK);
    cfg.pipeline.width = 128;
    cfg.pipeline.height = 128;
    cfg.algo.strategy = MFX50RT_STRATEGY_MBQP_CQP;
    std::snprintf(cfg.algo.expert_options_json,
                  sizeof(cfg.algo.expert_options_json),
                  "{\"profile\":\"%s\"%s%s}",
                  profile_name,
                  extra_options && extra_options[0] ? "," : "",
                  extra_options && extra_options[0] ? extra_options : "");

    MFX50RT_Capabilities caps{};
    init_api(caps);
    caps.supports_mbqp = 1;
    caps.supports_roi_delta_qp = 1;
    caps.max_roi_regions = 256;

    MFX50RT_EffectiveConfig effective{};
    init_api(effective);
    effective.effective_strategy = MFX50RT_STRATEGY_MBQP_CQP;
    effective.mbqp_enabled = 1;
    effective.spatial_qp_enabled = 1;
    effective.temporal_qp_enabled = 1;

    HybridTSRQController controller;
    controller.configure(0, cfg, caps, effective);
    return controller.decideFromYPlane(nullptr, 128, 128, 128, 0);
}

SpatialQpController spatial_for_profile(const char* profile_name, const char* extra_options = "") {
    MFX50RT_Config cfg{};
    assert(MFX50RT_DefaultConfig(&cfg) == MFX50RT_OK);
    cfg.algo.strategy = MFX50RT_STRATEGY_MBQP_CQP;
    std::snprintf(cfg.algo.expert_options_json,
                  sizeof(cfg.algo.expert_options_json),
                  "{\"profile\":\"%s\"%s%s}",
                  profile_name,
                  extra_options && extra_options[0] ? "," : "",
                  extra_options && extra_options[0] ? extra_options : "");

    MFX50RT_Capabilities caps{};
    init_api(caps);
    caps.supports_mbqp = 1;
    caps.max_roi_regions = 256;

    MFX50RT_EffectiveConfig effective{};
    init_api(effective);
    effective.effective_strategy = MFX50RT_STRATEGY_MBQP_CQP;
    effective.mbqp_enabled = 1;
    effective.spatial_qp_enabled = 1;

    SpatialQpController spatial;
    spatial.configure(cfg.algo, caps, effective);
    return spatial;
}

RoiAnalysisResult roi_map(int cols, int rows, uint8_t importance) {
    RoiAnalysisResult roi;
    roi.valid = true;
    roi.ctu_cols = cols;
    roi.ctu_rows = rows;
    roi.ctu_importance.assign(static_cast<size_t>(cols * rows), importance);
    return roi;
}

RoiAnalysisResult classified_roi_map(int cols,
                                     int rows,
                                     uint8_t importance,
                                     uint8_t foreground,
                                     uint8_t edge,
                                     uint8_t texture,
                                     uint8_t motion = 0) {
    RoiAnalysisResult roi = roi_map(cols, rows, importance);
    const size_t blocks = static_cast<size_t>(cols * rows);
    roi.ctu_foreground.assign(blocks, foreground);
    roi.ctu_edge.assign(blocks, edge);
    roi.ctu_texture.assign(blocks, texture);
    roi.ctu_motion.assign(blocks, motion);
    return roi;
}

} // namespace

int main() {
    HybridTSRQDecision base = decide_for_profile("target_90_ssim_guard");
    HybridTSRQDecision aggressive = decide_for_profile("target90_v2_aggressive");
    HybridTSRQDecision selective = decide_for_profile("target90_v2_selective_guard");
    HybridTSRQDecision roi_relaxed = decide_for_profile("target90_v2_roi_relaxed");
    HybridTSRQDecision final_candidate = decide_for_profile("target90_final_candidate");
    HybridTSRQDecision legacy_temporal = decide_for_profile("legacy_v2_temporal");
    HybridTSRQDecision legacy_212 = decide_for_profile("legacy_212_ctu");
    HybridTSRQDecision simple = decide_for_profile("simple_roi_background");
    HybridTSRQDecision simple_class = decide_for_profile("target90_simple_class");

    assert(base.spatial.has_mbqp);
    assert(aggressive.spatial.has_mbqp);
    assert(selective.spatial.has_mbqp);
    assert(roi_relaxed.spatial.has_mbqp);
    assert(final_candidate.spatial.has_mbqp);
    assert(legacy_temporal.spatial.has_mbqp);
    assert(legacy_212.spatial.has_mbqp);
    assert(simple.spatial.has_mbqp);
    assert(simple_class.spatial.has_mbqp);
    assert(aggressive.temporal.base_scene_qp >= base.temporal.base_scene_qp + 5);
    assert(aggressive.spatial.spatial_avg_qp > base.spatial.spatial_avg_qp);
    assert(aggressive.spatial.qp_stats.qp_p50 >= aggressive.spatial.spatial_avg_qp);
    assert(aggressive.spatial.qp_stats.qp_p90 >= aggressive.spatial.qp_stats.qp_p50);
    assert(aggressive.spatial.qp_stats.high_qp_block_ratio > 0.90f);
    assert(aggressive.spatial.region_stats.low_importance_block_ratio > 0.90f);
    assert(aggressive.spatial.region_stats.background_block_ratio > 0.90f);
    assert(aggressive.spatial.region_stats.flat_background_block_ratio > 0.90f);
    assert(final_candidate.temporal.frame_anchor_qp >= aggressive.temporal.frame_anchor_qp);
    assert(final_candidate.spatial.qp_stats.qp_p50 >= aggressive.spatial.qp_stats.qp_p50);
    assert(legacy_temporal.spatial.spatial_min_qp == legacy_temporal.temporal.frame_anchor_qp);
    assert(legacy_temporal.spatial.spatial_max_qp == legacy_temporal.temporal.frame_anchor_qp);
    assert(legacy_212.spatial.qp_stats.qp_p50 == 42);
    assert(simple.spatial.qp_stats.qp_p50 >= 48);
    assert(simple_class.spatial.qp_stats.qp_p50 >= 44);
    assert(simple_class.spatial.qp_stats.qp_p90 <= 48);

    QualityGuard guard;
    guard.configure(0.90f, 0.82f, 10);
    mfx50rt::hybridtsrq::FastFrameFeatures proxy_features;
    proxy_features.noise_score = 0.78f;
    proxy_features.motion_score = 0.20f;
    proxy_features.edge_density = 0.20f;
    guard.advanceFrame(proxy_features, 45);
    assert(!guard.state().active);

    proxy_features.noise_score = 0.90f;
    guard.advanceFrame(proxy_features, 47);
    assert(guard.state().active);
    assert(guard.state().background_max_qp >= 44);

    MFX50RT_QualityMetric metric{};
    init_api(metric);
    metric.stream_id = 0;
    metric.ssim = 0.89f;
    metric.compression_ratio = 0.88f;
    guard.update(metric);
    guard.advanceFrame(proxy_features, 47);
    assert(guard.state().active);
    assert(guard.state().background_max_qp >= 46);

    MFX50RT_Config cfg{};
    assert(MFX50RT_DefaultConfig(&cfg) == MFX50RT_OK);
    std::snprintf(cfg.algo.expert_options_json,
                  sizeof(cfg.algo.expert_options_json),
                  "{\"profile\":\"target90_v2_aggressive\"}");
    TemporalQpController temporal;
    temporal.configure(cfg.algo, cfg.pipeline);
    mfx50rt::hybridtsrq::FastFrameFeatures normal;
    normal.edge_density = 0.18f;
    normal.noise_score = 0.10f;
    normal.motion_score = 0.08f;
    normal.hard_score = 0.12f;
    mfx50rt::hybridtsrq::FastFrameFeatures hard = normal;
    hard.hard_score = 0.60f;
    hard.noise_score = 0.55f;
    auto normal_temporal = temporal.decide(normal, guard.state(), 0);
    TemporalQpController hard_temporal;
    hard_temporal.configure(cfg.algo, cfg.pipeline);
    auto guarded_temporal = hard_temporal.decide(hard, guard.state(), 0);
    assert(guarded_temporal.base_scene_qp < normal_temporal.base_scene_qp);

    QualityState proxy_quality;
    proxy_quality.active = true;
    proxy_quality.proxy_mode = true;
    proxy_quality.temporal_delta = 0;
    MFX50RT_Config proxy_cfg{};
    assert(MFX50RT_DefaultConfig(&proxy_cfg) == MFX50RT_OK);
    std::snprintf(proxy_cfg.algo.expert_options_json,
                  sizeof(proxy_cfg.algo.expert_options_json),
                  "{\"profile\":\"target90_simple_class\",\"disable_proxy_quality_guard\":0}");
    TemporalQpController proxy_temporal_enabled;
    proxy_temporal_enabled.configure(proxy_cfg.algo, proxy_cfg.pipeline);
    auto proxy_enabled_temporal = proxy_temporal_enabled.decide(normal, proxy_quality, 1);
    std::snprintf(proxy_cfg.algo.expert_options_json,
                  sizeof(proxy_cfg.algo.expert_options_json),
                  "{\"profile\":\"target90_simple_class\",\"disable_proxy_quality_guard\":1}");
    TemporalQpController proxy_temporal_disabled;
    proxy_temporal_disabled.configure(proxy_cfg.algo, proxy_cfg.pipeline);
    auto proxy_disabled_temporal = proxy_temporal_disabled.decide(normal, proxy_quality, 1);
    assert(proxy_disabled_temporal.frame_anchor_qp > proxy_enabled_temporal.frame_anchor_qp);

    TemporalQpDecision temporal_decision;
    temporal_decision.frame_anchor_qp = 38;
    QualityState quality;
    mfx50rt::hybridtsrq::FastFrameFeatures high_texture;
    high_texture.noise_score = 0.52f;
    high_texture.edge_density = 0.86f;
    high_texture.motion_score = 0.01f;
    SpatialQpController spatial = spatial_for_profile("target90_v2_roi_relaxed");

    auto background_decision = spatial.decide(high_texture,
                                              roi_map(4, 1, 64),
                                              temporal_decision,
                                              quality,
                                              256,
                                              64);
    assert(background_decision.qp_stats.qp_p90 >= 42);
    assert(background_decision.qp_stats.qp_p90 <= 44);

    auto near_all_roi_decision = spatial.decide(high_texture,
                                                roi_map(4, 1, 240),
                                                temporal_decision,
                                                quality,
                                                256,
                                                64);
    assert(near_all_roi_decision.qp_stats.qp_p50 <= 38);

    SpatialQpController final_spatial = spatial_for_profile("target90_final_candidate");
    SpatialQpController no_roi =
        spatial_for_profile("target90_final_candidate", "\"enable_roi_protection\":0");
    SpatialQpController no_transition =
        spatial_for_profile("target90_final_candidate", "\"enable_transition_protection\":0");
    auto protected_roi = final_spatial.decide(high_texture,
                                              roi_map(4, 1, 240),
                                              temporal_decision,
                                              quality,
                                              256,
                                              64);
    auto unprotected_roi = no_roi.decide(high_texture,
                                         roi_map(4, 1, 240),
                                         temporal_decision,
                                         quality,
                                         256,
                                         64);
    assert(unprotected_roi.qp_stats.qp_p50 > protected_roi.qp_stats.qp_p50);

    auto protected_transition = final_spatial.decide(high_texture,
                                                     roi_map(4, 1, 128),
                                                     temporal_decision,
                                                     quality,
                                                     256,
                                                     64);
    auto unprotected_transition = no_transition.decide(high_texture,
                                                       roi_map(4, 1, 128),
                                                       temporal_decision,
                                                       quality,
                                                       256,
                                                       64);
    assert(unprotected_transition.qp_stats.qp_p50 > protected_transition.qp_stats.qp_p50);

    SpatialQpController guarded_selective = spatial_for_profile("target90_v2_selective_guard");
    SpatialQpController no_hard_guard =
        spatial_for_profile("target90_v2_selective_guard", "\"enable_hard_scene_guard\":0");
    auto guarded_background = guarded_selective.decide(high_texture,
                                                       roi_map(4, 1, 64),
                                                       temporal_decision,
                                                       quality,
                                                       256,
                                                       64);
    auto unguarded_background = no_hard_guard.decide(high_texture,
                                                     roi_map(4, 1, 64),
                                                     temporal_decision,
                                                     quality,
                                                     256,
                                                     64);
    assert(unguarded_background.qp_stats.qp_p90 > guarded_background.qp_stats.qp_p90);

    SpatialQpController simple_class_spatial = spatial_for_profile("target90_simple_class");
    auto high_texture_background = simple_class_spatial.decide(
        high_texture,
        classified_roi_map(4, 1, 64, 0, 255, 255),
        temporal_decision,
        quality,
        256,
        64);
    assert(high_texture_background.qp_stats.qp_p50 >= 40);
    assert(high_texture_background.qp_stats.qp_p90 <= 44);
    assert(high_texture_background.scene_mode == SceneMode::Default);
    assert(high_texture_background.class_qp_table_name == "default");
    assert(high_texture_background.class_qp_table_overwrite_count == 1);
    assert(high_texture_background.region_stats.high_texture_background_block_ratio > 0.90f);
    assert(high_texture_background.region_stats.true_roi_block_ratio == 0.0f);

    RoiAnalysisResult static_texture_with_global_traffic =
        classified_roi_map(4, 1, 160, 0, 255, 255);
    static_texture_with_global_traffic.foreground_ratio = 0.20f;
    auto static_texture_decision = simple_class_spatial.decide(
        high_texture,
        static_texture_with_global_traffic,
        temporal_decision,
        quality,
        256,
        64);
    assert(static_texture_decision.region_stats.edge_texture_roi_block_ratio == 0.0f);
    assert(static_texture_decision.region_stats.high_texture_background_block_ratio > 0.90f);
    assert(static_texture_decision.qp_stats.qp_p50 >= high_texture_background.qp_stats.qp_p50);

    RoiAnalysisResult moving_texture_target =
        classified_roi_map(4, 1, 160, 0, 255, 255, 255);
    auto moving_texture_decision = simple_class_spatial.decide(
        high_texture,
        moving_texture_target,
        temporal_decision,
        quality,
        256,
        64);
    assert(moving_texture_decision.region_stats.edge_texture_roi_block_ratio > 0.90f);
    assert(moving_texture_decision.qp_stats.qp_p50 <= 42);

    auto true_roi = simple_class_spatial.decide(high_texture,
                                                classified_roi_map(4, 1, 240, 255, 255, 255),
                                                temporal_decision,
                                                quality,
                                                256,
                                                64);
    assert(true_roi.qp_stats.qp_p50 <= 40);
    assert(true_roi.region_stats.true_roi_block_ratio > 0.90f);

    auto hard_background = simple_class_spatial.decide(
        high_texture,
        classified_roi_map(4, 1, 64, 0, 0, 0, 255),
        temporal_decision,
        quality,
        256,
        64);
    assert(hard_background.qp_stats.qp_p50 >= 40);
    assert(hard_background.qp_stats.qp_p90 <= 44);
    assert(hard_background.region_stats.hard_scene_background_block_ratio > 0.90f);

    mfx50rt::hybridtsrq::FastFrameFeatures low_risk_static;
    low_risk_static.edge_density = 0.20f;
    low_risk_static.noise_score = 0.10f;
    low_risk_static.motion_score = 0.01f;
    auto boosted_static_background = simple_class_spatial.decide(
        low_risk_static,
        classified_roi_map(4, 1, 64, 0, 0, 0),
        temporal_decision,
        quality,
        256,
        64);
    assert(boosted_static_background.scene_mode == SceneMode::LowRiskStatic);
    assert(boosted_static_background.class_qp_table_name == "low_risk_static");
    assert(boosted_static_background.qp_stats.qp_p50 >= 48);

    auto relaxed_large_roi = simple_class_spatial.decide(
        high_texture,
        classified_roi_map(4, 1, 240, 255, 255, 255),
        temporal_decision,
        quality,
        256,
        64);
    assert(relaxed_large_roi.qp_stats.qp_p50 >= 40);

    mfx50rt::hybridtsrq::FastFrameFeatures narrow_texture_risk = high_texture;
    narrow_texture_risk.edge_density = 0.85f;
    narrow_texture_risk.noise_score = 0.51f;
    narrow_texture_risk.scene_cut_score = 0.005f;
    RoiAnalysisResult narrow_texture_background =
        classified_roi_map(8, 1, 64, 0, 255, 255);
    narrow_texture_background.ctu_importance[0] = 128;
    auto guarded_texture_risk = simple_class_spatial.decide(
        narrow_texture_risk,
        narrow_texture_background,
        temporal_decision,
        quality,
        512,
        64);
    assert(!guarded_texture_risk.has_mbqp);
    assert(guarded_texture_risk.scene_mode == SceneMode::HardSceneRisk);
    assert(guarded_texture_risk.region_stats.edge_texture_roi_block_ratio == 0.0f);
    assert(guarded_texture_risk.region_stats.high_texture_background_block_ratio > 0.80f);

    mfx50rt::hybridtsrq::FastFrameFeatures broad_static_detail_risk = high_texture;
    broad_static_detail_risk.edge_density = 0.76f;
    broad_static_detail_risk.noise_score = 0.45f;
    broad_static_detail_risk.motion_score = 0.006f;
    broad_static_detail_risk.scene_cut_score = 0.008f;
    RoiAnalysisResult broad_texture_background =
        classified_roi_map(8, 1, 64, 0, 255, 255);
    broad_texture_background.ctu_foreground[0] = 255;
    broad_texture_background.ctu_importance[0] = 240;
    auto broad_texture_risk = simple_class_spatial.decide(
        broad_static_detail_risk,
        broad_texture_background,
        temporal_decision,
        quality,
        512,
        64);
    assert(broad_texture_risk.scene_mode == SceneMode::HardSceneRisk);
    assert(!broad_texture_risk.has_mbqp);
    assert(broad_texture_risk.region_stats.true_roi_block_ratio <= 0.20f);

    RoiAnalysisResult pure_texture_after_risk =
        classified_roi_map(8, 1, 64, 0, 255, 255);
    auto held_texture_recovery = simple_class_spatial.decide(
        broad_static_detail_risk,
        pure_texture_after_risk,
        temporal_decision,
        quality,
        512,
        64);
    assert(!held_texture_recovery.has_mbqp);

    SpatialQpController fresh_simple_class_spatial =
        spatial_for_profile("target90_simple_class");
    auto fresh_pure_texture = fresh_simple_class_spatial.decide(
        broad_static_detail_risk,
        pure_texture_after_risk,
        temporal_decision,
        quality,
        512,
        64);
    assert(fresh_pure_texture.has_mbqp);

    mfx50rt::hybridtsrq::FastFrameFeatures sparse_static_detail = high_texture;
    sparse_static_detail.edge_density = 0.60f;
    sparse_static_detail.noise_score = 0.38f;
    sparse_static_detail.motion_score = 0.006f;
    sparse_static_detail.scene_cut_score = 0.008f;
    RoiAnalysisResult sparse_texture_context =
        classified_roi_map(8, 1, 64, 0, 0, 0);
    sparse_texture_context.ctu_foreground[0] = 255;
    sparse_texture_context.ctu_importance[0] = 240;
    sparse_texture_context.ctu_edge[1] = 255;
    sparse_texture_context.ctu_texture[1] = 255;
    sparse_texture_context.ctu_edge[2] = 255;
    sparse_texture_context.ctu_texture[2] = 255;
    auto sparse_texture_risk = simple_class_spatial.decide(
        sparse_static_detail,
        sparse_texture_context,
        temporal_decision,
        quality,
        512,
        64);
    assert(sparse_texture_risk.scene_mode == SceneMode::SparseMidTexture);
    assert(!sparse_texture_risk.has_mbqp);

    RoiAnalysisResult moving_texture_risk =
        classified_roi_map(8, 1, 160, 0, 255, 255, 255);
    moving_texture_risk.ctu_importance[0] = 128;
    auto guarded_moving_texture_risk = simple_class_spatial.decide(
        narrow_texture_risk,
        moving_texture_risk,
        temporal_decision,
        quality,
        512,
        64);
    assert(guarded_moving_texture_risk.region_stats.edge_texture_roi_block_ratio > 0.80f);
    assert(guarded_moving_texture_risk.qp_stats.qp_p50 <= 39);

    auto weak_texture_background = simple_class_spatial.decide(
        normal,
        classified_roi_map(4, 1, 64, 0, 1, 1),
        temporal_decision,
        quality,
        256,
        64);
    assert(weak_texture_background.region_stats.high_texture_background_block_ratio == 0.0f);
    assert(weak_texture_background.region_stats.normal_background_block_ratio > 0.90f);

    QualityState real_quality_guard;
    real_quality_guard.active = true;
    real_quality_guard.proxy_mode = false;
    real_quality_guard.background_max_qp = 42;
    auto guarded_simple_class = simple_class_spatial.decide(
        normal,
        classified_roi_map(4, 1, 64, 0, 0, 0),
        temporal_decision,
        real_quality_guard,
        256,
        64);
    assert(guarded_simple_class.scene_mode == SceneMode::QualityRecovery);
    assert(guarded_simple_class.qp_stats.qp_p50 <= 42);

    QualityState proxy_quality_guard = real_quality_guard;
    proxy_quality_guard.proxy_mode = true;
    SpatialQpController proxy_disabled =
        spatial_for_profile("target90_simple_class", "\"disable_proxy_quality_guard\":1");
    SpatialQpController proxy_enabled =
        spatial_for_profile("target90_simple_class", "\"disable_proxy_quality_guard\":0");
    auto proxy_disabled_decision = proxy_disabled.decide(normal,
                                                         classified_roi_map(4, 1, 64, 0, 0, 0),
                                                         temporal_decision,
                                                         proxy_quality_guard,
                                                         256,
                                                         64);
    auto proxy_enabled_decision = proxy_enabled.decide(normal,
                                                       classified_roi_map(4, 1, 64, 0, 0, 0),
                                                       temporal_decision,
                                                       proxy_quality_guard,
                                                       256,
                                                       64);
    assert(proxy_disabled_decision.qp_stats.qp_p50 > proxy_enabled_decision.qp_stats.qp_p50);
    return 0;
}
