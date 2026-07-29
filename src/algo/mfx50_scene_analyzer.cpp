#include "mfx50_scene_analyzer.h"

#include "mfx50_realtime.h"

#include <algorithm>
#include <cmath>

namespace mfx50rt {

namespace {

float clamp01(double value) {
    return static_cast<float>(std::max(0.0, std::min(1.0, value)));
}

} // namespace

FrameAnalysis SceneAnalyzer::analyzeNv12YPlane(const uint8_t* y,
                                               int width,
                                               int height,
                                               int pitch,
                                               int64_t pts) {
    FrameAnalysis out;
    out.frameIndex = frameIndex_++;
    out.pts = pts;
    out.suggestedProfile = MFX50_PROFILE_COMPRESS_85_PROBE;
    if (!y || width <= 1 || height <= 1 || pitch < width) {
        out.hardScore = 1.0f;
        out.suggestedProfile = MFX50_PROFILE_QUALITY_90_NEAR;
        return out;
    }

    const int stepX = std::max(1, width / 160);
    const int stepY = std::max(1, height / 90);
    uint64_t count = 0;
    double sum = 0.0;
    double edge = 0.0;
    double noise = 0.0;

    for (int yy = stepY; yy < height - stepY; yy += stepY) {
        const uint8_t* row = y + static_cast<size_t>(yy) * pitch;
        const uint8_t* rowUp = y + static_cast<size_t>(yy - stepY) * pitch;
        const uint8_t* rowDn = y + static_cast<size_t>(yy + stepY) * pitch;
        for (int xx = stepX; xx < width - stepX; xx += stepX) {
            const int center = row[xx];
            const int dx = std::abs(center - row[xx - stepX]) +
                           std::abs(center - row[xx + stepX]);
            const int dy = std::abs(center - rowUp[xx]) +
                           std::abs(center - rowDn[xx]);
            const int localAvg = (row[xx - stepX] + row[xx + stepX] +
                                  rowUp[xx] + rowDn[xx]) / 4;
            sum += center;
            edge += dx + dy;
            noise += std::abs(center - localAvg);
            count++;
        }
    }

    if (count == 0) {
        out.hardScore = 1.0f;
        out.suggestedProfile = MFX50_PROFILE_QUALITY_90_NEAR;
        return out;
    }

    const double mean = sum / static_cast<double>(count);
    const double edgeNorm = edge / static_cast<double>(count) / 120.0;
    const double noiseNorm = noise / static_cast<double>(count) / 40.0;
    const double motionNorm = hasPrevious_
        ? std::abs(mean - previousMean_) / 48.0
        : 0.0;
    previousMean_ = mean;
    hasPrevious_ = true;

    out.edgeScore = clamp01(edgeNorm);
    out.noiseScore = clamp01(noiseNorm);
    out.motionScore = clamp01(motionNorm);
    out.flatScore = clamp01(1.0 - (out.edgeScore * 0.65 + out.noiseScore * 0.35));
    out.sceneCutScore = clamp01(out.motionScore * 1.5);
    out.hardScore = clamp01(out.motionScore * 0.45 + out.noiseScore * 0.35 + out.edgeScore * 0.20);

    if (out.flatScore > 0.75f && out.motionScore < 0.25f && out.noiseScore < 0.25f) {
        out.suggestedProfile = MFX50_PROFILE_COMPRESS_90_PROBE_B;
        out.suggestedQpDelta = 1;
        out.suggestedSmoothScaleFactor = 30;
    }
    else if (out.hardScore > 0.70f || out.noiseScore > 0.60f || out.motionScore > 0.75f) {
        out.suggestedProfile = MFX50_PROFILE_QUALITY_90_NEAR;
        out.suggestedQpDelta = -1;
        out.suggestedDenoiseStrength = out.noiseScore > 0.60f ? 30 : 0;
    }
    else {
        out.suggestedProfile = MFX50_PROFILE_COMPRESS_85_PROBE;
    }

    return out;
}

} // namespace mfx50rt
