#include "src/algo/hybridtsrq/qp_map.h"

#include <cassert>
#include <cstdlib>

using namespace mfx50rt::hybridtsrq;

static void expect_size(int width, int height, int cols, int rows) {
    RoiAnalysisResult roi;
    roi.valid = false;
    FastFrameFeatures fast;
    fast.edge_density = 0.2f;
    QualityState quality;
    CtuMap ctu = build_ctu_importance_map(roi, fast, width, height, 64);
    QpMap16x16 map = ctu_qp_to_16x16_mbqp(ctu, width, height, 32, quality);
    assert(map.block_cols == cols);
    assert(map.block_rows == rows);
    assert(map.qp.size() == static_cast<size_t>(cols * rows));
    for (uint8_t qp : map.qp) {
        assert(qp >= 1 && qp <= 51);
    }
}

int main() {
    expect_size(1920, 1080, 120, 68);
    expect_size(1280, 720, 80, 45);
    expect_size(1919, 1079, 120, 68);

    QpMap16x16 map;
    map.width = 64;
    map.height = 16;
    map.block_cols = 4;
    map.block_rows = 1;
    map.qp = {10, 40, 10, 40};
    smooth_qp_map(map, 8);
    for (int i = 1; i < map.block_cols; ++i) {
        assert(std::abs(static_cast<int>(map.qp[i]) - static_cast<int>(map.qp[i - 1])) <= 8);
    }
    clamp_qp_map(map, 22, 42);
    for (uint8_t qp : map.qp) {
        assert(qp >= 22 && qp <= 42);
    }

    CtuMap ctu;
    ctu.width = 64;
    ctu.height = 16;
    ctu.ctu_size = 16;
    ctu.cols = 4;
    ctu.rows = 1;
    ctu.importance = {32, 90, 128, 220};
    QualityState quality;
    HybridTsrqQpTuning tuning;
    tuning.target90_level = 2;
    tuning.normal_background_delta = 14;
    tuning.flat_background_delta = 18;
    tuning.strong_roi_delta = -4;
    tuning.transition_delta = 0;
    tuning.background_qp_max = 51;
    tuning.flat_background_qp_max = 51;
    tuning.global_qp_max = 51;
    tuning.max_neighbor_delta = 51;
    tuning.selective_hard_scene_guard = 1;
    tuning.hard_scene_guard_active = 1;
    tuning.hard_scene_important_qp_max = 42;
    tuning.hard_scene_edge_transition_qp_max = 46;
    tuning.hard_scene_selective_background_qp_max = 50;
    tuning.hard_scene_selective_flat_qp_max = 51;
    tuning.hard_scene_apply_to_flat_background = 0;
    QpMap16x16 selective = ctu_qp_to_16x16_mbqp(ctu, 64, 16, 36, quality, tuning);
    assert(selective.qp[0] >= 50);
    assert(selective.qp[1] >= 50);
    assert(selective.qp[2] <= 46);
    assert(selective.qp[3] <= 42);
    return 0;
}
