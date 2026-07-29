#include "mfx50_adaptive_decision.h"

#include "mfx50_realtime.h"

namespace mfx50rt {

AdaptiveDecision decideAdaptiveProfile(const FrameAnalysis& analysis) {
    AdaptiveDecision decision;
    decision.profile = analysis.suggestedProfile;
    decision.qpDelta = analysis.suggestedQpDelta;
    decision.smoothScaleFactor = analysis.suggestedSmoothScaleFactor;
    decision.denoiseStrength = analysis.suggestedDenoiseStrength;
    decision.forceIdr = analysis.suggestForceIdr;

    if (decision.profile == 0) {
        decision.profile = MFX50_PROFILE_COMPRESS_85_PROBE;
    }
    return decision;
}

} // namespace mfx50rt
