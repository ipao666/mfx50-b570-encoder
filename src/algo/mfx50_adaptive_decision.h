#pragma once

#include "mfx50_realtime.h"
#include "mfx50_scene_analyzer.h"

namespace mfx50rt {

struct AdaptiveDecision {
    int profile = MFX50_PROFILE_COMPRESS_85_PROBE;
    int qpDelta = 0;
    int smoothScaleFactor = 0;
    int denoiseStrength = 0;
    int forceIdr = 0;
};

AdaptiveDecision decideAdaptiveProfile(const FrameAnalysis& analysis);

} // namespace mfx50rt
