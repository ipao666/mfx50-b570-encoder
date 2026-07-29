#include "static_reuse_gate.h"

#include <algorithm>

namespace mfx50rt::hybridtsrq {

namespace {

float ratio(float value, float limit) {
    return limit > 0.0f ? value / limit : 0.0f;
}

bool is_static_texture_background(const FastFrameFeatures& fast,
                                  const SpatialQpDecision& spatial) {
    return fast.motion_score <= 0.01f &&
           fast.scene_cut_score <= 0.01f &&
           !(fast.edge_density >= 0.70f && fast.noise_score >= 0.40f) &&
           spatial.region_stats.background_block_ratio >= 0.70f &&
           spatial.region_stats.foreground_block_ratio <= 0.05f &&
           spatial.region_stats.true_roi_block_ratio <= 0.06f &&
           spatial.region_stats.transition_block_ratio <= 0.08f &&
           spatial.region_stats.edge_texture_roi_block_ratio <= 0.01f &&
           spatial.region_stats.high_texture_background_block_ratio >= 0.30f;
}

float static_reuse_risk_score(const FastFrameFeatures& fast,
                              const SpatialQpDecision& spatial,
                              const QualityState& quality,
                              const TemporalQpDecision& temporal) {
    const bool static_texture_background =
        is_static_texture_background(fast, spatial);
    float score = 0.0f;
    score = std::max(score, ratio(fast.motion_score, 0.03f));
    score = std::max(score, ratio(fast.scene_cut_score, 0.03f));
    if (!static_texture_background) {
        score = std::max(score, ratio(fast.noise_score, 0.22f));
        score = std::max(score, ratio(fast.edge_density, 0.45f));
        score = std::max(score, ratio(spatial.region_stats.hard_scene_background_block_ratio, 0.05f));
    }
    score = std::max(score, ratio(spatial.region_stats.true_roi_block_ratio, 0.03f));
    score = std::max(score, ratio(spatial.region_stats.foreground_block_ratio, 0.05f));
    score = std::max(score, ratio(spatial.region_stats.transition_block_ratio,
                                  static_texture_background ? 0.08f : 0.04f));
    score = std::max(score, ratio(spatial.region_stats.edge_texture_roi_block_ratio, 0.02f));
    if (quality.active || temporal.force_idr) {
        score = std::max(score, 2.0f);
    }
    if (spatial.hard_guard_active && !static_texture_background) {
        score = std::max(score, 2.0f);
    }
    if (spatial.scene_mode != SceneMode::LowRiskStatic && !static_texture_background) {
        score = std::max(score, 1.5f);
    }
    return score;
}

} // namespace

StaticReuseDecision StaticReuseGate::decide(const FastFrameFeatures& fast,
                                            const TemporalQpDecision& temporal,
                                            const SpatialQpDecision& spatial,
                                            const QualityState& quality) {
    StaticReuseDecision out;
    out.risk_score = static_reuse_risk_score(fast, spatial, quality, temporal);
    if (out.risk_score <= 1.0f) {
        consecutive_static_frames_++;
    } else {
        consecutive_static_frames_ = 0;
    }
    out.consecutive_static_frames = consecutive_static_frames_;
    out.candidate = consecutive_static_frames_ >= 3 &&
                    (consecutive_static_frames_ % 12) != 0;
    return out;
}

} // namespace mfx50rt::hybridtsrq
