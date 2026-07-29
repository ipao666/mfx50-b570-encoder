#pragma once

#include "hybridtsrq_types.h"

namespace mfx50rt::hybridtsrq {

struct StaticReuseDecision {
    bool candidate = false;
    int consecutive_static_frames = 0;
    float risk_score = 0.0f;
};

class StaticReuseGate {
public:
    StaticReuseDecision decide(const FastFrameFeatures& fast,
                               const TemporalQpDecision& temporal,
                               const SpatialQpDecision& spatial,
                               const QualityState& quality);

private:
    int consecutive_static_frames_ = 0;
};

} // namespace mfx50rt::hybridtsrq
