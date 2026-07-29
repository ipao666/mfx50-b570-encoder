#pragma once

#include "mfx50rt.h"

#include <stdint.h>

#include <array>
#include <string>
#include <vector>

namespace mfx50rt::hybridtsrq {

enum class CtuBlockClass : uint8_t {
    StrongRoi = 0,
    WeakRoi = 1,
    EdgeTextureRoi = 2,
    HighTextureBackground = 3,
    HardSceneBackground = 4,
    Transition = 5,
    NormalBackground = 6,
    FlatBackground = 7,
};

enum class SceneMode : uint8_t {
    Default = 0,
    LowRiskStatic = 1,
    SparseMidTexture = 2,
    LargeRoi = 3,
    HighTextureBackground = 4,
    HardSceneRisk = 5,
    QualityRecovery = 6,
};

struct FastFrameFeatures {
    uint64_t frame_id = 0;
    int width = 0;
    int height = 0;
    float mean_luma = 0.0f;
    float luma_delta = 0.0f;
    float edge_density = 0.0f;
    float noise_score = 0.0f;
    float motion_score = 0.0f;
    float scene_cut_score = 0.0f;
    float hard_score = 0.0f;
    bool night_mode = false;
};

struct RoiBox {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    float confidence = 0.0f;
    int type = 0;
};

struct RoiAnalysisResult {
    uint64_t frame_id = 0;
    bool valid = false;
    bool night_mode = false;
    float foreground_ratio = 0.0f;
    float global_motion = 0.0f;
    std::vector<RoiBox> objects;
    int ctu_cols = 0;
    int ctu_rows = 0;
    std::vector<uint8_t> ctu_foreground;
    std::vector<uint8_t> ctu_edge;
    std::vector<uint8_t> ctu_texture;
    std::vector<uint8_t> ctu_motion;
    std::vector<uint8_t> ctu_importance;
};

struct CtuMap {
    int width = 0;
    int height = 0;
    int ctu_size = 64;
    int cols = 0;
    int rows = 0;
    std::vector<uint8_t> importance;
    std::vector<CtuBlockClass> classes;
};

struct QpMap16x16 {
    int width = 0;
    int height = 0;
    int block_cols = 0;
    int block_rows = 0;
    float smoothing_changed_qp_avg = 0.0f;
    std::vector<uint8_t> qp;
};

struct QpDistributionStats {
    int qp_p10 = 0;
    int qp_p50 = 0;
    int qp_p90 = 0;
    int qp_p95 = 0;
    float high_qp_block_ratio = 0.0f;
    std::array<uint32_t, 52> histogram{};
};

struct RegionBlockStats {
    float low_importance_block_ratio = 0.0f;
    float roi_block_ratio = 0.0f;
    float transition_block_ratio = 0.0f;
    float background_block_ratio = 0.0f;
    float flat_background_block_ratio = 0.0f;
    float foreground_block_ratio = 0.0f;
    float edge_block_ratio = 0.0f;
    float texture_block_ratio = 0.0f;
    float true_roi_block_ratio = 0.0f;
    float edge_texture_roi_block_ratio = 0.0f;
    float high_texture_background_block_ratio = 0.0f;
    float hard_scene_background_block_ratio = 0.0f;
    float normal_background_block_ratio = 0.0f;
};

struct TemporalQpDecision {
    int qpi = 32;
    int qpp = 34;
    int qpb = 37;
    int base_scene_qp = 32;
    int frame_type_delta = 0;
    int frame_anchor_qp = 32;
    bool force_idr = false;
    int recommended_gop = 0;
    int recommended_b_frames = 0;
    int quality_guard_temporal_delta = 0;
};

struct SpatialQpDecision {
    bool has_mbqp = false;
    bool has_roi = false;
    SceneMode scene_mode = SceneMode::Default;
    std::string class_qp_table_name = "default";
    bool hard_scene_like = false;
    bool hard_guard_active = false;
    int class_qp_table_overwrite_count = 0;
    int spatial_min_qp = 0;
    int spatial_max_qp = 0;
    int spatial_avg_qp = 0;
    float smoothing_changed_qp_avg = 0.0f;
    QpDistributionStats qp_stats;
    RegionBlockStats region_stats;
    QpMap16x16 mbqp_map;
    std::vector<RoiBox> roi_boxes;
};

struct HybridTSRQDecision {
    uint64_t frame_id = 0;
    MFX50RT_ControlStrategy strategy = MFX50RT_STRATEGY_GLOBAL;
    TemporalQpDecision temporal;
    SpatialQpDecision spatial;
    bool static_reuse_candidate = false;
    int static_reuse_consecutive_frames = 0;
    float static_reuse_risk_score = 0.0f;
    bool quality_guard_on = false;
    char reason[512] = {0};
};

struct QualityState {
    bool active = false;
    int hold_frames_remaining = 0;
    float avg_ssim = 0.0f;
    float min_ssim = 1.0f;
    float p5_ssim = 0.0f;
    float compression_ratio_avg = 0.0f;
    int temporal_delta = 0;
    int background_max_qp = 48;
    int night_background_max_qp = 40;
    int spatial_delta = 0;
    bool proxy_mode = true;
};

} // namespace mfx50rt::hybridtsrq
