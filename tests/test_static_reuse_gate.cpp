#include "src/algo/hybridtsrq/static_reuse_gate.h"

#include <cassert>

using mfx50rt::hybridtsrq::FastFrameFeatures;
using mfx50rt::hybridtsrq::QualityState;
using mfx50rt::hybridtsrq::SceneMode;
using mfx50rt::hybridtsrq::SpatialQpDecision;
using mfx50rt::hybridtsrq::StaticReuseGate;
using mfx50rt::hybridtsrq::TemporalQpDecision;

namespace {

FastFrameFeatures low_risk_static_features() {
    FastFrameFeatures f;
    f.edge_density = 0.18f;
    f.noise_score = 0.10f;
    f.motion_score = 0.01f;
    f.scene_cut_score = 0.01f;
    return f;
}

SpatialQpDecision low_risk_static_spatial() {
    SpatialQpDecision s;
    s.scene_mode = SceneMode::LowRiskStatic;
    s.region_stats.background_block_ratio = 0.95f;
    s.region_stats.flat_background_block_ratio = 0.90f;
    s.region_stats.true_roi_block_ratio = 0.0f;
    s.region_stats.transition_block_ratio = 0.01f;
    s.region_stats.hard_scene_background_block_ratio = 0.0f;
    s.region_stats.edge_texture_roi_block_ratio = 0.0f;
    return s;
}

SpatialQpDecision static_high_texture_traffic_spatial() {
    SpatialQpDecision s;
    s.scene_mode = SceneMode::HardSceneRisk;
    s.hard_guard_active = true;
    s.region_stats.background_block_ratio = 0.85f;
    s.region_stats.flat_background_block_ratio = 0.75f;
    s.region_stats.foreground_block_ratio = 0.02f;
    s.region_stats.true_roi_block_ratio = 0.03f;
    s.region_stats.transition_block_ratio = 0.05f;
    s.region_stats.edge_texture_roi_block_ratio = 0.0f;
    s.region_stats.high_texture_background_block_ratio = 0.52f;
    s.region_stats.hard_scene_background_block_ratio = 0.42f;
    return s;
}

} // namespace

int main() {
    StaticReuseGate gate;
    TemporalQpDecision temporal;
    QualityState quality;
    FastFrameFeatures fast = low_risk_static_features();
    SpatialQpDecision spatial = low_risk_static_spatial();

    auto first = gate.decide(fast, temporal, spatial, quality);
    auto second = gate.decide(fast, temporal, spatial, quality);
    auto third = gate.decide(fast, temporal, spatial, quality);
    assert(!first.candidate);
    assert(!second.candidate);
    assert(third.candidate);
    assert(third.consecutive_static_frames == 3);
    assert(third.risk_score <= 1.0f);

    fast.motion_score = 0.20f;
    auto moving = gate.decide(fast, temporal, spatial, quality);
    assert(!moving.candidate);
    assert(moving.consecutive_static_frames == 0);
    assert(moving.risk_score > 1.0f);

    fast = low_risk_static_features();
    quality.active = true;
    auto guarded = gate.decide(fast, temporal, spatial, quality);
    assert(!guarded.candidate);
    assert(guarded.consecutive_static_frames == 0);

    quality.active = false;
    spatial.region_stats.true_roi_block_ratio = 0.20f;
    auto roi_risk = gate.decide(fast, temporal, spatial, quality);
    assert(!roi_risk.candidate);
    assert(roi_risk.consecutive_static_frames == 0);

    StaticReuseGate texture_gate;
    FastFrameFeatures texture_static;
    texture_static.edge_density = 0.68f;
    texture_static.noise_score = 0.38f;
    texture_static.motion_score = 0.004f;
    texture_static.scene_cut_score = 0.004f;
    SpatialQpDecision texture_spatial = static_high_texture_traffic_spatial();
    auto texture_first = texture_gate.decide(texture_static, temporal, texture_spatial, quality);
    auto texture_second = texture_gate.decide(texture_static, temporal, texture_spatial, quality);
    auto texture_third = texture_gate.decide(texture_static, temporal, texture_spatial, quality);
    assert(!texture_first.candidate);
    assert(!texture_second.candidate);
    assert(texture_third.candidate);
    assert(texture_third.risk_score <= 1.0f);

    texture_static.motion_score = 0.06f;
    auto texture_motion = texture_gate.decide(texture_static, temporal, texture_spatial, quality);
    assert(!texture_motion.candidate);
    assert(texture_motion.consecutive_static_frames == 0);

    StaticReuseGate severe_texture_gate;
    FastFrameFeatures severe_texture_static = texture_static;
    severe_texture_static.edge_density = 0.72f;
    severe_texture_static.noise_score = 0.41f;
    severe_texture_static.motion_score = 0.004f;
    severe_texture_static.scene_cut_score = 0.004f;
    auto severe_first =
        severe_texture_gate.decide(severe_texture_static, temporal, texture_spatial, quality);
    auto severe_second =
        severe_texture_gate.decide(severe_texture_static, temporal, texture_spatial, quality);
    auto severe_third =
        severe_texture_gate.decide(severe_texture_static, temporal, texture_spatial, quality);
    assert(!severe_first.candidate);
    assert(!severe_second.candidate);
    assert(!severe_third.candidate);
    assert(severe_third.risk_score > 1.0f);

    StaticReuseGate refresh_gate;
    int candidates = 0;
    int refreshes = 0;
    for (int i = 0; i < 24; ++i) {
        auto d = refresh_gate.decide(fast, temporal, low_risk_static_spatial(), quality);
        if (d.candidate) candidates++;
        if (d.consecutive_static_frames >= 3 && !d.candidate) refreshes++;
    }
    assert(candidates > 0);
    assert(refreshes > 0);
    return 0;
}
