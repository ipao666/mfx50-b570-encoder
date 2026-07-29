#pragma once

#include "hybridtsrq_types.h"
#include "mfx50rt.h"
#include "tuning_profile.h"

namespace mfx50rt::hybridtsrq {

class SpatialQpController {
public:
    void configure(const MFX50RT_AlgoPolicy& policy,
                   const MFX50RT_Capabilities& caps,
                   const MFX50RT_EffectiveConfig& effective);

    SpatialQpDecision decide(const FastFrameFeatures& fast,
                             const RoiAnalysisResult& roi,
                             const TemporalQpDecision& temporal,
                             const QualityState& quality,
                             int width,
                             int height) const;

private:
    MFX50RT_ControlStrategy strategy_ = MFX50RT_STRATEGY_GLOBAL;
    bool spatial_enabled_ = true;
    bool mbqp_enabled_ = true;
    bool roi_enabled_ = true;
    int max_roi_regions_ = 0;
    mutable int global_detail_recovery_hold_frames_ = 0;
    HybridTsrqQpTuning tuning_;
};

} // namespace mfx50rt::hybridtsrq
