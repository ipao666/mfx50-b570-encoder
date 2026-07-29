#pragma once

#include <stdint.h>

namespace mfx50rt {

struct FrameAnalysis {
    uint64_t frameIndex = 0;
    int64_t pts = 0;
    float flatScore = 0.0f;
    float motionScore = 0.0f;
    float noiseScore = 0.0f;
    float edgeScore = 0.0f;
    float sceneCutScore = 0.0f;
    float hardScore = 0.0f;
    int suggestedProfile = 0;
    int suggestedQpDelta = 0;
    int suggestedSmoothScaleFactor = 0;
    int suggestedDenoiseStrength = 0;
    int suggestForceIdr = 0;
};

class SceneAnalyzer {
public:
    FrameAnalysis analyzeNv12YPlane(const uint8_t* y,
                                    int width,
                                    int height,
                                    int pitch,
                                    int64_t pts);

private:
    bool hasPrevious_ = false;
    double previousMean_ = 0.0;
    uint64_t frameIndex_ = 0;
};

} // namespace mfx50rt
