#include "mfx50rt.h"
#include "src/algo/hybridtsrq/quality_guard.h"

#include <cassert>
#include <cstring>

using mfx50rt::hybridtsrq::FastFrameFeatures;
using mfx50rt::hybridtsrq::QualityGuard;

namespace {

MFX50RT_QualityMetric quality_metric(float ssim, float compression_ratio) {
    MFX50RT_QualityMetric metric{};
    metric.size = sizeof(metric);
    metric.version = MFX50RT_API_VERSION;
    metric.ssim = ssim;
    metric.compression_ratio = compression_ratio;
    return metric;
}

FastFrameFeatures low_risk_features() {
    FastFrameFeatures features;
    features.noise_score = 0.12f;
    features.motion_score = 0.08f;
    features.edge_density = 0.18f;
    return features;
}

void low_ssim_enters_hold_and_tightens_qp() {
    QualityGuard guard;
    guard.configure(0.90f, 0.82f, 3);
    guard.update(quality_metric(0.79f, 0.91f));

    guard.advanceFrame(low_risk_features(), 44);

    assert(guard.state().active);
    assert(!guard.state().proxy_mode);
    assert(guard.state().hold_frames_remaining == 2);
    assert(guard.state().temporal_delta == -2);
    assert(guard.state().background_max_qp == 42);
    assert(guard.state().spatial_delta == -1);
}

void quality_margin_with_low_compression_relaxes_background_qp() {
    QualityGuard guard;
    guard.configure(0.90f, 0.82f, 10);
    guard.update(quality_metric(0.835f, 0.80f));
    for (int i = 0; i < 19; ++i) {
        guard.update(quality_metric(0.919f, 0.80f));
    }

    guard.advanceFrame(low_risk_features(), 44);

    assert(!guard.state().active);
    assert(!guard.state().proxy_mode);
    assert(guard.state().avg_ssim > 0.91f);
    assert(guard.state().min_ssim > 0.83f);
    assert(guard.state().compression_ratio_avg < 0.90f);
    assert(guard.state().background_max_qp == 51);
    assert(guard.state().night_background_max_qp >= 46);
    assert(guard.state().spatial_delta >= 2);
}

void proxy_mode_uses_only_scene_risk_before_real_metrics() {
    QualityGuard guard;
    guard.configure(0.90f, 0.82f, 10);

    guard.advanceFrame(low_risk_features(), 47);
    assert(!guard.state().active);
    assert(guard.state().proxy_mode);
    assert(guard.state().background_max_qp == 48);

    FastFrameFeatures noisy = low_risk_features();
    noisy.noise_score = 0.90f;
    guard.advanceFrame(noisy, 47);

    assert(guard.state().active);
    assert(guard.state().proxy_mode);
    assert(guard.state().background_max_qp == 46);
    assert(guard.state().spatial_delta == 0);
}

} // namespace

int main() {
    low_ssim_enters_hold_and_tightens_qp();
    quality_margin_with_low_compression_relaxes_background_qp();
    proxy_mode_uses_only_scene_risk_before_real_metrics();
    return 0;
}
