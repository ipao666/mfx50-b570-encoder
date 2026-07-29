#pragma once

#include "hybridtsrq_types.h"

#include <stdint.h>

namespace mfx50rt::hybridtsrq {

class FastAnalyzer {
public:
    FastFrameFeatures analyzeYPlane(const uint8_t* y,
                                    int width,
                                    int height,
                                    int pitch,
                                    int64_t pts);

private:
    bool has_previous_mean_ = false;
    double previous_mean_ = 0.0;
    uint64_t frame_id_ = 0;
};

} // namespace mfx50rt::hybridtsrq
