#include "temporal_qp_controller.h"

#include <algorithm>

namespace mfx50rt::hybridtsrq {

namespace {

int clamp_qp(int qp) {
    return std::max(1, std::min(51, qp));
}

int base_scene_qp(const FastFrameFeatures& f) {
    if (f.scene_cut_score > 0.70f) return 28;
    if (f.night_mode && f.noise_score > 0.55f) return 30;
    if (f.night_mode) return 32;
    if (f.motion_score > 0.65f) return 30;
    if (f.edge_density < 0.20f && f.motion_score < 0.12f) return 34;
    return 32;
}

} // namespace

void TemporalQpController::configure(const MFX50RT_AlgoPolicy& policy,
                                     const MFX50RT_PipelineConfig& pipeline) {
    profile_ = policy.profile;
    configured_gop_ = pipeline.gop_size > 0 ? pipeline.gop_size : 60;
    configured_b_frames_ = std::max(0, pipeline.b_frames);
    tuning_ = tuning_from_policy(policy);
}

TemporalQpDecision TemporalQpController::decide(const FastFrameFeatures& features,
                                                const QualityState& quality,
                                                uint64_t frame_id) {
    TemporalQpDecision d;
    d.base_scene_qp = clamp_qp(base_scene_qp(features) + tuning_.base_qp_offset);
    if (tuning_.target90_level > 0 &&
        tuning_.hard_scene_base_qp_relief > 0 &&
        is_hard_scene_for_qp_guard(features)) {
        d.base_scene_qp = clamp_qp(d.base_scene_qp - tuning_.hard_scene_base_qp_relief);
    }
    const bool quality_active = quality.active &&
        !(quality.proxy_mode && tuning_.disable_proxy_quality_guard);
    const int quality_delta = quality_active ? quality.temporal_delta : 0;
    d.quality_guard_temporal_delta = quality_delta;

    if (idr_cooldown_ > 0) idr_cooldown_--;
    const bool scene_cut = features.scene_cut_score >= 0.70f;
    if (scene_cut && idr_cooldown_ == 0) {
        d.force_idr = true;
        conservative_frames_ = 5;
        idr_cooldown_ = 30;
    }

    const bool high_compression = profile_ == MFX50RT_PROFILE_TARGET_90_SSIM_GUARD ||
                                  profile_ == MFX50RT_PROFILE_MAX_COMPRESSION;
    const bool quality_mode = quality_active || conservative_frames_ > 0 ||
                              profile_ == MFX50RT_PROFILE_SAFE;

    if (quality_mode) {
        d.qpi = clamp_qp(d.base_scene_qp - 4 + quality_delta);
        d.qpp = clamp_qp(d.base_scene_qp - 1 + quality_delta);
        d.qpb = clamp_qp(d.base_scene_qp + 1 + quality_delta);
    } else if (high_compression) {
        d.qpi = clamp_qp(d.base_scene_qp + tuning_.temporal_i_delta);
        d.qpp = clamp_qp(d.base_scene_qp + tuning_.temporal_p_delta);
        d.qpb = clamp_qp(d.base_scene_qp + tuning_.temporal_b_delta);
    } else {
        d.qpi = clamp_qp(d.base_scene_qp - 3);
        d.qpp = clamp_qp(d.base_scene_qp);
        d.qpb = clamp_qp(d.base_scene_qp + 3);
    }

    int frame_type_delta = 0;
    if (frame_id == 0 || d.force_idr || (configured_gop_ > 0 && frame_id % configured_gop_ == 0)) {
        frame_type_delta = d.qpi - d.base_scene_qp;
    } else if (configured_b_frames_ > 0 && (frame_id % (configured_b_frames_ + 1)) != 0) {
        frame_type_delta = d.qpb - d.base_scene_qp;
    } else {
        frame_type_delta = d.qpp - d.base_scene_qp;
    }
    d.frame_type_delta = frame_type_delta;
    d.frame_anchor_qp = clamp_qp(d.base_scene_qp + frame_type_delta + quality_delta);

    if (features.scene_cut_score > 0.70f || features.motion_score > 0.75f) {
        d.recommended_gop = std::max(15, std::min(configured_gop_, 30));
    } else if (features.edge_density < 0.20f && features.motion_score < 0.08f) {
        d.recommended_gop = std::max(configured_gop_, 180);
    } else {
        d.recommended_gop = configured_gop_;
    }
    d.recommended_b_frames = configured_b_frames_;
    if (conservative_frames_ > 0) conservative_frames_--;
    return d;
}

} // namespace mfx50rt::hybridtsrq
