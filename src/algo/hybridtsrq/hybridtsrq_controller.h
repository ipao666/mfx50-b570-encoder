#pragma once

#include "decision_trace.h"
#include "fast_analyzer.h"
#include "heavy_roi_analyzer.h"
#include "quality_guard.h"
#include "spatial_qp_controller.h"
#include "static_reuse_gate.h"
#include "temporal_qp_controller.h"

namespace mfx50rt::hybridtsrq {

class HybridTSRQController {
public:
    void configure(uint32_t stream_id,
                   const MFX50RT_Config& config,
                   const MFX50RT_Capabilities& caps,
                   const MFX50RT_EffectiveConfig& effective);

    HybridTSRQDecision decideFromYPlane(const uint8_t* y,
                                        int width,
                                        int height,
                                        int pitch,
                                        int64_t pts);
    HybridTSRQDecision decideFromNV12(const uint8_t* y,
                                      int width,
                                      int height,
                                      int pitch,
                                      const uint8_t* uv,
                                      int uv_pitch,
                                      int64_t pts);

    void updateQualityMetric(const MFX50RT_QualityMetric& metric);
    MFX50RT_Status copyTrace(MFX50RT_DecisionTrace* traces, uint32_t* inout_count) const;
    void recordEncodeControlResult(int64_t frame_id,
                                   int mbqp_attached,
                                   const char* actual_encode_control,
                                   const char* reason);

    const QualityState& qualityState() const { return quality_.state(); }
    const FastFrameFeatures& lastFastFeatures() const { return last_fast_; }
    const RoiAnalysisResult& lastRoi() const { return last_roi_; }

private:
    int inferFrameType(const TemporalQpDecision& temporal, uint64_t frame_id) const;

    uint32_t stream_id_ = 0;
    MFX50RT_Config config_{};
    MFX50RT_Capabilities caps_{};
    MFX50RT_EffectiveConfig effective_{};
    FastAnalyzer fast_;
    HeavyRoiAnalyzer heavy_;
    TemporalQpController temporal_;
    SpatialQpController spatial_;
    StaticReuseGate static_reuse_;
    QualityGuard quality_;
    DecisionTraceBuffer trace_{256};
    FastFrameFeatures last_fast_{};
    RoiAnalysisResult last_roi_{};
    uint64_t synthetic_frame_id_ = 0;
    int last_qp_avg_ = 32;
};

} // namespace mfx50rt::hybridtsrq
