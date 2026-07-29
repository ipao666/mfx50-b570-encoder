#pragma once

#include "hybridtsrq_types.h"
#include "mfx50rt.h"

#include <deque>

namespace mfx50rt::hybridtsrq {

class QualityGuard {
public:
    void configure(float avg_ssim_target, float min_ssim_guard, int hold_frames);
    void update(const MFX50RT_QualityMetric& metric);
    void advanceFrame(const FastFrameFeatures& features, int qp_avg);
    const QualityState& state() const { return state_; }

private:
    struct Sample {
        float ssim = 0.0f;
        float compression_ratio = 0.0f;
    };

    void recompute();

    float avg_ssim_target_ = 0.90f;
    float min_ssim_guard_ = 0.82f;
    int hold_frames_ = 60;
    std::deque<Sample> samples_;
    QualityState state_;
};

} // namespace mfx50rt::hybridtsrq
