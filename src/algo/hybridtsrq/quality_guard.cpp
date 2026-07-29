#include "quality_guard.h"

#include <algorithm>
#include <numeric>
#include <vector>

namespace mfx50rt::hybridtsrq {

void QualityGuard::configure(float avg_ssim_target, float min_ssim_guard, int hold_frames) {
    avg_ssim_target_ = avg_ssim_target > 0.0f ? avg_ssim_target : 0.90f;
    min_ssim_guard_ = min_ssim_guard > 0.0f ? min_ssim_guard : 0.82f;
    hold_frames_ = hold_frames > 0 ? hold_frames : 60;
}

void QualityGuard::update(const MFX50RT_QualityMetric& metric) {
    if (metric.ssim <= 0.0f) return;
    Sample s;
    s.ssim = metric.ssim;
    s.compression_ratio = metric.compression_ratio;
    samples_.push_back(s);
    while (samples_.size() > 120) samples_.pop_front();
    state_.proxy_mode = false;
    recompute();
    if (state_.min_ssim < min_ssim_guard_) {
        state_.active = true;
        state_.hold_frames_remaining = hold_frames_;
    }
}

void QualityGuard::advanceFrame(const FastFrameFeatures& features, int qp_avg) {
    const bool has_real_quality = !samples_.empty();
    constexpr float kTailGuardMargin = 0.01f;
    constexpr float kCompressionRelaxAvgMargin = 0.01f;
    constexpr float kCompressionRelaxMinMargin = 0.01f;
    if (state_.hold_frames_remaining > 0) {
        state_.hold_frames_remaining--;
        state_.active = true;
    } else if (has_real_quality) {
        state_.proxy_mode = false;
        state_.active = state_.avg_ssim < avg_ssim_target_ ||
                        state_.p5_ssim < min_ssim_guard_ + kTailGuardMargin;
    } else {
        state_.proxy_mode = true;
        state_.active = features.noise_score > 0.85f ||
                        features.motion_score > 0.95f ||
                        (qp_avg > 46 && features.edge_density > 0.55f);
    }

    if (state_.active) {
        if (state_.proxy_mode) {
            state_.temporal_delta = 0;
            state_.background_max_qp = 46;
            state_.night_background_max_qp = 44;
            state_.spatial_delta = 0;
        } else if (state_.min_ssim < min_ssim_guard_) {
            state_.temporal_delta = -2;
            state_.background_max_qp = 42;
            state_.night_background_max_qp = 40;
            state_.spatial_delta = -1;
        } else {
            state_.temporal_delta = -1;
            state_.background_max_qp = 46;
            state_.night_background_max_qp = 44;
            state_.spatial_delta = 0;
        }
    } else if (!state_.proxy_mode &&
               state_.avg_ssim >= avg_ssim_target_ + kCompressionRelaxAvgMargin &&
               state_.min_ssim >= min_ssim_guard_ + kCompressionRelaxMinMargin &&
               state_.compression_ratio_avg < 0.90f) {
        state_.temporal_delta = 0;
        state_.background_max_qp = 51;
        state_.night_background_max_qp = 46;
        state_.spatial_delta = 2;
    } else {
        state_.temporal_delta = 0;
        state_.background_max_qp = 48;
        state_.night_background_max_qp = 40;
        state_.spatial_delta = 0;
    }
}

void QualityGuard::recompute() {
    if (samples_.empty()) return;
    std::vector<float> ssim;
    ssim.reserve(samples_.size());
    double sum_ssim = 0.0;
    double sum_ratio = 0.0;
    for (const Sample& s : samples_) {
        ssim.push_back(s.ssim);
        sum_ssim += s.ssim;
        sum_ratio += s.compression_ratio;
    }
    std::sort(ssim.begin(), ssim.end());
    state_.avg_ssim = static_cast<float>(sum_ssim / samples_.size());
    state_.min_ssim = ssim.front();
    const size_t p5_idx = std::min(ssim.size() - 1, static_cast<size_t>(ssim.size() * 0.05));
    state_.p5_ssim = ssim[p5_idx];
    state_.compression_ratio_avg = static_cast<float>(sum_ratio / samples_.size());
}

} // namespace mfx50rt::hybridtsrq
