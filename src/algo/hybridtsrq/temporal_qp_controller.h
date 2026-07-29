#pragma once

#include "hybridtsrq_types.h"
#include "mfx50rt.h"
#include "tuning_profile.h"

namespace mfx50rt::hybridtsrq {

class TemporalQpController {
public:
    void configure(const MFX50RT_AlgoPolicy& policy, const MFX50RT_PipelineConfig& pipeline);
    TemporalQpDecision decide(const FastFrameFeatures& features,
                              const QualityState& quality,
                              uint64_t frame_id);

private:
    MFX50RT_AlgoProfile profile_ = MFX50RT_PROFILE_TARGET_90_SSIM_GUARD;
    int configured_gop_ = 60;
    int configured_b_frames_ = 0;
    int conservative_frames_ = 0;
    int idr_cooldown_ = 0;
    HybridTsrqQpTuning tuning_;
};

} // namespace mfx50rt::hybridtsrq
