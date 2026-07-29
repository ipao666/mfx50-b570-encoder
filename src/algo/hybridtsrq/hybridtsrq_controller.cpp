#include "hybridtsrq_controller.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mfx50rt::hybridtsrq {

namespace {

int clamp_qp(int value) {
    return std::max(1, std::min(51, value));
}

} // namespace

void HybridTSRQController::configure(uint32_t stream_id,
                                     const MFX50RT_Config& config,
                                     const MFX50RT_Capabilities& caps,
                                     const MFX50RT_EffectiveConfig& effective) {
    stream_id_ = stream_id;
    config_ = config;
    caps_ = caps;
    effective_ = effective;
    temporal_.configure(config_.algo, config_.pipeline);
    spatial_.configure(config_.algo, caps_, effective_);
    quality_.configure(config_.algo.avg_ssim_target,
                       config_.algo.min_ssim_guard,
                       config_.algo.quality_guard_hold_frames);
    const int w = config_.pipeline.width > 0 ? config_.pipeline.width : 1920;
    const int h = config_.pipeline.height > 0 ? config_.pipeline.height : 1080;
    heavy_.configure(w, h, 64, config_.algo.heavy_analyze_interval);
}

HybridTSRQDecision HybridTSRQController::decideFromYPlane(const uint8_t* y,
                                                         int width,
                                                         int height,
                                                         int pitch,
                                                         int64_t pts) {
    return decideFromNV12(y, width, height, pitch, nullptr, 0, pts);
}

HybridTSRQDecision HybridTSRQController::decideFromNV12(const uint8_t* y,
                                                        int width,
                                                        int height,
                                                        int pitch,
                                                        const uint8_t* uv,
                                                        int uv_pitch,
                                                        int64_t pts) {
    if (width <= 0) width = config_.pipeline.width > 0 ? config_.pipeline.width : 1920;
    if (height <= 0) height = config_.pipeline.height > 0 ? config_.pipeline.height : 1080;
    if (pitch <= 0) pitch = width;
    if (uv_pitch <= 0) uv_pitch = pitch;

    if (y && width > 1 && height > 1 && pitch >= width && config_.algo.enable_fast_analyzer) {
        last_fast_ = fast_.analyzeYPlane(y, width, height, pitch, pts);
    } else {
        last_fast_ = {};
        last_fast_.frame_id = synthetic_frame_id_++;
        last_fast_.width = width;
        last_fast_.height = height;
        last_fast_.mean_luma = 96.0f;
        last_fast_.edge_density = 0.18f;
        last_fast_.noise_score = 0.10f;
        last_fast_.motion_score = 0.08f;
        last_fast_.hard_score = 0.12f;
    }

    if (config_.algo.enable_quality_guard) {
        quality_.advanceFrame(last_fast_, last_qp_avg_);
    }

    if (y && config_.algo.enable_heavy_analyzer && heavy_.shouldAnalyze(last_fast_)) {
        RoiAnalysisResult roi = heavy_.analyzeNV12(y, width, height, pitch, uv, uv_pitch);
        if (roi.valid) last_roi_ = roi;
    }
    if (!last_roi_.valid) {
        last_roi_.frame_id = last_fast_.frame_id;
        last_roi_.valid = false;
        last_roi_.ctu_cols = (width + 63) / 64;
        last_roi_.ctu_rows = (height + 63) / 64;
        last_roi_.foreground_ratio = 0.0f;
    }

    HybridTSRQDecision decision;
    decision.frame_id = last_fast_.frame_id;
    decision.strategy = effective_.effective_strategy;
    decision.temporal = temporal_.decide(last_fast_, quality_.state(), decision.frame_id);
    decision.temporal.frame_anchor_qp = clamp_qp(decision.temporal.frame_anchor_qp);
    decision.spatial = spatial_.decide(last_fast_,
                                       last_roi_,
                                       decision.temporal,
                                       quality_.state(),
                                       width,
                                       height);
    const StaticReuseDecision reuse =
        static_reuse_.decide(last_fast_, decision.temporal, decision.spatial, quality_.state());
    decision.static_reuse_candidate = reuse.candidate;
    decision.static_reuse_consecutive_frames = reuse.consecutive_static_frames;
    decision.static_reuse_risk_score = reuse.risk_score;
    decision.quality_guard_on = quality_.state().active;
    if (decision.spatial.spatial_avg_qp > 0) {
        last_qp_avg_ = decision.spatial.spatial_avg_qp;
    } else {
        last_qp_avg_ = decision.temporal.frame_anchor_qp;
    }

    const char* strategy = "global";
    if (decision.strategy == MFX50RT_STRATEGY_MBQP_CQP) strategy = "mbqp_cqp";
    else if (decision.strategy == MFX50RT_STRATEGY_ROI_DELTA_QP) strategy = "roi_delta_qp";
    std::snprintf(decision.reason,
                  sizeof(decision.reason),
                  "HybridTSRQ %s anchor_qp=%d roi=%zu mbqp=%s static_reuse=%s risk=%.3f",
                  strategy,
                  decision.temporal.frame_anchor_qp,
                  decision.spatial.roi_boxes.size(),
                  decision.spatial.has_mbqp ? "on" : "off",
                  decision.static_reuse_candidate ? "candidate" : "no",
                  decision.static_reuse_risk_score);
    trace_.push(stream_id_,
                pts,
                inferFrameType(decision.temporal, decision.frame_id),
                decision,
                last_fast_,
                last_roi_,
                quality_.state());
    return decision;
}

void HybridTSRQController::updateQualityMetric(const MFX50RT_QualityMetric& metric) {
    quality_.update(metric);
}

MFX50RT_Status HybridTSRQController::copyTrace(MFX50RT_DecisionTrace* traces,
                                               uint32_t* inout_count) const {
    return trace_.copyTo(traces, inout_count);
}

void HybridTSRQController::recordEncodeControlResult(int64_t frame_id,
                                                     int mbqp_attached,
                                                     const char* actual_encode_control,
                                                     const char* reason) {
    trace_.updateEncodeControl(stream_id_,
                               frame_id,
                               mbqp_attached,
                               actual_encode_control,
                               reason);
}

int HybridTSRQController::inferFrameType(const TemporalQpDecision& temporal,
                                         uint64_t frame_id) const {
    if (frame_id == 0 || temporal.force_idr || temporal.frame_type_delta == temporal.qpi - temporal.base_scene_qp) {
        return 1;
    }
    if (temporal.frame_type_delta == temporal.qpb - temporal.base_scene_qp) {
        return 3;
    }
    return 2;
}

} // namespace mfx50rt::hybridtsrq
