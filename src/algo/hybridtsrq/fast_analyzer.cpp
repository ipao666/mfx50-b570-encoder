#include "fast_analyzer.h"

#include <algorithm>
#include <cmath>

namespace mfx50rt::hybridtsrq {

namespace {

float clamp01(double value) {
    return static_cast<float>(std::max(0.0, std::min(1.0, value)));
}

} // namespace

FastFrameFeatures FastAnalyzer::analyzeYPlane(const uint8_t* y,
                                              int width,
                                              int height,
                                              int pitch,
                                              int64_t pts) {
    (void)pts;
    FastFrameFeatures out;
    out.frame_id = frame_id_++;
    out.width = width;
    out.height = height;
    if (!y || width <= 1 || height <= 1 || pitch < width) {
        out.hard_score = 1.0f;
        out.scene_cut_score = 1.0f;
        return out;
    }

    const int step_x = std::max(1, width / 160);
    const int step_y = std::max(1, height / 90);
    uint64_t count = 0;
    double sum = 0.0;
    double edge = 0.0;
    double noise = 0.0;

    for (int yy = step_y; yy < height - step_y; yy += step_y) {
        const uint8_t* row = y + static_cast<size_t>(yy) * pitch;
        const uint8_t* row_up = y + static_cast<size_t>(yy - step_y) * pitch;
        const uint8_t* row_dn = y + static_cast<size_t>(yy + step_y) * pitch;
        for (int xx = step_x; xx < width - step_x; xx += step_x) {
            const int center = row[xx];
            const int dx = std::abs(center - row[xx - step_x]) +
                           std::abs(center - row[xx + step_x]);
            const int dy = std::abs(center - row_up[xx]) +
                           std::abs(center - row_dn[xx]);
            const int local_avg = (row[xx - step_x] + row[xx + step_x] +
                                   row_up[xx] + row_dn[xx]) / 4;
            sum += center;
            edge += dx + dy;
            noise += std::abs(center - local_avg);
            count++;
        }
    }

    if (count == 0) {
        out.hard_score = 1.0f;
        out.scene_cut_score = 1.0f;
        return out;
    }

    const double mean = sum / static_cast<double>(count);
    out.mean_luma = static_cast<float>(mean);
    out.luma_delta = has_previous_mean_
        ? static_cast<float>(std::abs(mean - previous_mean_))
        : 0.0f;
    previous_mean_ = mean;
    has_previous_mean_ = true;

    out.edge_density = clamp01(edge / static_cast<double>(count) / 120.0);
    out.noise_score = clamp01(noise / static_cast<double>(count) / 40.0);
    out.motion_score = clamp01(static_cast<double>(out.luma_delta) / 48.0);
    out.scene_cut_score = clamp01(out.motion_score * 1.5);
    out.hard_score = clamp01(out.motion_score * 0.45 +
                             out.noise_score * 0.35 +
                             out.edge_density * 0.20);
    out.night_mode = out.mean_luma < 40.0f;
    return out;
}

} // namespace mfx50rt::hybridtsrq
