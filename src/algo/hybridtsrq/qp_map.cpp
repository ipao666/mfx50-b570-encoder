#include "qp_map.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <vector>

namespace mfx50rt::hybridtsrq {

namespace {

int ceil_div(int value, int denom) {
    return denom > 0 ? (value + denom - 1) / denom : 0;
}

uint8_t clamp_u8(int value, int lo, int hi) {
    return static_cast<uint8_t>(std::max(lo, std::min(hi, value)));
}

int clamp_int(int value, int lo, int hi) {
    return std::max(lo, std::min(hi, value));
}

uint8_t map_value(const std::vector<uint8_t>& map, size_t idx, size_t expected) {
    return map.size() == expected && idx < map.size() ? map[idx] : 0;
}

CtuBlockClass classify_ctu(const FastFrameFeatures& fast,
                           const RoiAnalysisResult& roi,
                           uint8_t importance,
                           size_t idx,
                           size_t total) {
    const float f = static_cast<float>(importance) / 255.0f;
    const bool has_foreground = roi.ctu_foreground.size() == total;
    const bool has_edge = roi.ctu_edge.size() == total;
    const bool has_texture = roi.ctu_texture.size() == total;
    const bool has_motion = roi.ctu_motion.size() == total;
    const uint8_t foreground_score = map_value(roi.ctu_foreground, idx, total);
    const uint8_t edge_score = map_value(roi.ctu_edge, idx, total);
    const uint8_t texture_score = map_value(roi.ctu_texture, idx, total);
    const uint8_t motion_score = map_value(roi.ctu_motion, idx, total);
    const bool foreground = foreground_score >= 26;
    const bool edge = edge_score >= 26;
    const bool texture = texture_score >= 64;
    const bool motion = motion_score >= 26;
    const bool edge_texture = edge || texture ||
        (!has_edge && !has_texture && fast.edge_density >= 0.58f);
    const bool scene_risk = motion ||
        (!has_motion && fast.motion_score >= 0.30f) ||
        fast.noise_score >= 0.45f ||
        fast.hard_score >= 0.25f ||
        fast.scene_cut_score >= 0.45f;
    const bool strong_importance = f >= 0.80f;
    const bool weak_importance = f >= 0.60f;
    const bool transition_importance = f >= 0.45f;
    const bool has_local_semantic_maps = has_foreground || has_motion;
    const bool local_dynamic_texture = edge_texture && weak_importance && motion;
    const bool fallback_texture_roi =
        edge_texture && weak_importance && !has_local_semantic_maps &&
        roi.foreground_ratio > 0.03f;

    if (foreground || (!has_foreground && strong_importance)) {
        return strong_importance ? CtuBlockClass::StrongRoi : CtuBlockClass::WeakRoi;
    }
    if (local_dynamic_texture || fallback_texture_roi) {
        return CtuBlockClass::EdgeTextureRoi;
    }
    if (edge_texture && weak_importance) {
        return CtuBlockClass::EdgeTextureRoi;
    }
    if (edge_texture) return CtuBlockClass::HighTextureBackground;
    if (scene_risk) return CtuBlockClass::HardSceneBackground;
    if (transition_importance) return CtuBlockClass::Transition;
    if (f < 0.25f) return CtuBlockClass::FlatBackground;
    return CtuBlockClass::NormalBackground;
}

int qp_for_class(int anchor,
                 CtuBlockClass block_class,
                 const QualityState& quality,
                 const HybridTsrqQpTuning& tuning,
                 const ClassQpTable& table) {
    const bool quality_active = quality.active &&
        !(quality.proxy_mode && tuning.disable_proxy_quality_guard);
    auto qp = [&](int delta, int min_qp, int max_qp) {
        int effective_max = max_qp;
        if (quality_active) {
            effective_max = std::min(effective_max, quality.background_max_qp);
        }
        const int effective_min = std::min(min_qp, effective_max);
        return clamp_int(anchor + table.frame_anchor_offset + delta + quality.spatial_delta,
                         effective_min,
                         effective_max);
    };
    switch (block_class) {
        case CtuBlockClass::StrongRoi:
            return qp(table.strong_roi_delta,
                      table.strong_roi_min_qp,
                      table.strong_roi_max_qp);
        case CtuBlockClass::WeakRoi:
            return qp(table.weak_roi_delta,
                      table.weak_roi_min_qp,
                      table.weak_roi_max_qp);
        case CtuBlockClass::EdgeTextureRoi:
            if (tuning.disable_edge_texture_roi) {
                return qp(table.high_texture_background_delta,
                          table.high_texture_background_min_qp,
                          table.high_texture_background_max_qp);
            }
            return qp(table.edge_texture_roi_delta,
                      table.edge_texture_roi_min_qp,
                      table.edge_texture_roi_max_qp);
        case CtuBlockClass::HighTextureBackground:
            return qp(table.high_texture_background_delta,
                      table.high_texture_background_min_qp,
                      table.high_texture_background_max_qp);
        case CtuBlockClass::HardSceneBackground:
            return qp(table.hard_scene_background_delta,
                      table.hard_scene_background_min_qp,
                      table.hard_scene_background_max_qp);
        case CtuBlockClass::Transition:
            if (!tuning.enable_transition_protection) {
                return qp(table.normal_background_delta,
                          table.normal_background_min_qp,
                          table.normal_background_max_qp);
            }
            return qp(table.transition_delta,
                      table.transition_min_qp,
                      table.transition_max_qp);
        case CtuBlockClass::FlatBackground:
            return clamp_int(table.flat_background_qp +
                                 table.frame_anchor_offset +
                                 quality.spatial_delta,
                             1,
                             quality_active ? std::min(51, quality.background_max_qp) : 51);
        case CtuBlockClass::NormalBackground:
        default:
            return qp(table.normal_background_delta,
                      table.normal_background_min_qp,
                      table.normal_background_max_qp);
    }
}

int qp_from_importance(int anchor, uint8_t importance, const QualityState& quality) {
    const float f = static_cast<float>(importance) / 255.0f;
    int delta = 4;
    if (f >= 0.85f) delta = -6;
    else if (f >= 0.70f) delta = -4;
    else if (f >= 0.55f) delta = -2;
    else if (f <= 0.20f) delta = 8;
    else if (f <= 0.30f) delta = 4;
    delta += quality.spatial_delta;
    int qp = anchor + delta;
    if (f >= 0.70f) qp = std::max(22, std::min(36, qp));
    else if (f >= 0.55f) qp = std::max(26, std::min(39, qp));
    else qp = std::max(34, std::min(quality.background_max_qp, qp));
    return std::max(1, std::min(51, qp));
}

int qp_from_importance(int anchor,
                       uint8_t importance,
                       const QualityState& quality,
                       const HybridTsrqQpTuning& tuning) {
    if (tuning.target90_level <= 0) {
        return qp_from_importance(anchor, importance, quality);
    }
    if (tuning.spatial_passthrough) {
        const int qp = anchor + quality.spatial_delta;
        return std::max(1, std::min(tuning.global_qp_max, qp));
    }

    const float f = static_cast<float>(importance) / 255.0f;
    const bool quality_active = quality.active &&
        !(quality.proxy_mode && tuning.disable_proxy_quality_guard);
    if (tuning.absolute_region_qp) {
        int qp = tuning.absolute_background_qp;
        if (f >= 0.80f) qp = tuning.absolute_strong_roi_qp;
        else if (f >= 0.60f) qp = tuning.absolute_weak_roi_qp;
        else if (f >= 0.45f) qp = tuning.absolute_transition_qp;
        else if (f < 0.25f) qp = tuning.absolute_flat_background_qp;
        qp += quality.spatial_delta;
        return std::max(1, std::min(tuning.global_qp_max, qp));
    }

    int delta = tuning.normal_background_delta;
    int min_qp = 34;
    int max_qp = tuning.background_qp_max;

    if (f >= 0.80f) {
        delta = tuning.enable_roi_protection ? tuning.strong_roi_delta : tuning.transition_delta;
        min_qp = tuning.roi_qp_min;
        max_qp = tuning.enable_roi_protection ? tuning.roi_qp_max : tuning.background_qp_max;
    } else if (f >= 0.60f) {
        delta = tuning.enable_roi_protection ? tuning.weak_roi_delta : tuning.transition_delta;
        min_qp = tuning.weak_roi_qp_min;
        max_qp = tuning.enable_roi_protection ? tuning.weak_roi_qp_max : tuning.background_qp_max;
    } else if (f >= 0.45f) {
        delta = tuning.enable_transition_protection
            ? tuning.transition_delta
            : tuning.normal_background_delta;
        min_qp = tuning.enable_transition_protection ? tuning.weak_roi_qp_min : 34;
        max_qp = tuning.background_qp_max;
    } else if (f < 0.25f) {
        delta = tuning.flat_background_delta;
        min_qp = 34;
        max_qp = tuning.flat_background_qp_max;
    }

    if (tuning.hard_scene_guard_active && tuning.selective_hard_scene_guard) {
        if (f >= 0.60f) {
            max_qp = std::min(max_qp, tuning.hard_scene_important_qp_max);
        } else if (f >= 0.45f) {
            max_qp = std::min(max_qp, tuning.hard_scene_edge_transition_qp_max);
        } else if (f < 0.25f) {
            if (tuning.hard_scene_apply_to_flat_background) {
                max_qp = std::min(max_qp, tuning.hard_scene_selective_flat_qp_max);
            }
        } else {
            max_qp = std::min(max_qp, tuning.hard_scene_selective_background_qp_max);
        }
    }

    if (quality_active) {
        max_qp = std::min(max_qp, tuning.quality_guard_background_qp_max);
        max_qp = std::min(max_qp, quality.background_max_qp);
    }

    const int qp = anchor + delta + quality.spatial_delta;
    return std::max(1, std::min(tuning.global_qp_max, std::max(min_qp, std::min(max_qp, qp))));
}

float ratio(size_t count, size_t total) {
    return total > 0 ? static_cast<float>(count) / static_cast<float>(total) : 0.0f;
}

} // namespace

CtuMap build_ctu_importance_map(const RoiAnalysisResult& roi,
                                const FastFrameFeatures& fast,
                                int width,
                                int height,
                                int ctu_size) {
    CtuMap out;
    out.width = width;
    out.height = height;
    out.ctu_size = ctu_size > 0 ? ctu_size : 64;
    out.cols = ceil_div(width, out.ctu_size);
    out.rows = ceil_div(height, out.ctu_size);
    out.importance.assign(static_cast<size_t>(out.cols) * out.rows, 64);
    out.classes.assign(out.importance.size(), CtuBlockClass::NormalBackground);

    const bool compatible = roi.valid &&
                            roi.ctu_cols == out.cols &&
                            roi.ctu_rows == out.rows &&
                            roi.ctu_importance.size() == out.importance.size();
    if (compatible) {
        out.importance = roi.ctu_importance;
    } else {
        const int base = static_cast<int>(std::round(
            255.0f * (0.25f * fast.edge_density +
                      0.15f * fast.noise_score +
                      0.10f * fast.motion_score)));
        std::fill(out.importance.begin(), out.importance.end(), clamp_u8(base, 32, 192));
    }

    for (const RoiBox& box : roi.objects) {
        const int x0 = std::max(0, box.x / out.ctu_size);
        const int y0 = std::max(0, box.y / out.ctu_size);
        const int x1 = std::min(out.cols - 1, (box.x + box.w + out.ctu_size - 1) / out.ctu_size);
        const int y1 = std::min(out.rows - 1, (box.y + box.h + out.ctu_size - 1) / out.ctu_size);
        for (int cy = y0; cy <= y1; ++cy) {
            for (int cx = x0; cx <= x1; ++cx) {
                out.importance[static_cast<size_t>(cy) * out.cols + cx] =
                    std::max<uint8_t>(out.importance[static_cast<size_t>(cy) * out.cols + cx], 192);
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = cx + dx;
                        const int ny = cy + dy;
                        if (nx < 0 || ny < 0 || nx >= out.cols || ny >= out.rows) continue;
                        uint8_t& v = out.importance[static_cast<size_t>(ny) * out.cols + nx];
                        v = std::max<uint8_t>(v, 140);
                    }
                }
            }
        }
    }
    for (size_t i = 0; i < out.importance.size(); ++i) {
        out.classes[i] = classify_ctu(fast, roi, out.importance[i], i, out.importance.size());
    }
    return out;
}

QpMap16x16 ctu_qp_to_16x16_mbqp(const CtuMap& ctu_map,
                                int width,
                                int height,
                                int frame_anchor_qp,
                                const QualityState& quality) {
    return ctu_qp_to_16x16_mbqp(ctu_map,
                                width,
                                height,
                                frame_anchor_qp,
                                quality,
                                HybridTsrqQpTuning{});
}

QpMap16x16 ctu_qp_to_16x16_mbqp(const CtuMap& ctu_map,
                                int width,
                                int height,
                                int frame_anchor_qp,
                                const QualityState& quality,
                                const HybridTsrqQpTuning& tuning) {
    QpMap16x16 out;
    out.width = width;
    out.height = height;
    out.block_cols = ceil_div(width, 16);
    out.block_rows = ceil_div(height, 16);
    out.qp.assign(static_cast<size_t>(out.block_cols) * out.block_rows,
                  clamp_u8(frame_anchor_qp, 1, 51));

    for (int by = 0; by < out.block_rows; ++by) {
        const int ctu_y = std::min(ctu_map.rows - 1, (by * 16) / std::max(1, ctu_map.ctu_size));
        for (int bx = 0; bx < out.block_cols; ++bx) {
            const int ctu_x = std::min(ctu_map.cols - 1, (bx * 16) / std::max(1, ctu_map.ctu_size));
            const uint8_t importance =
                (ctu_x >= 0 && ctu_y >= 0 && ctu_x < ctu_map.cols && ctu_y < ctu_map.rows)
                    ? ctu_map.importance[static_cast<size_t>(ctu_y) * ctu_map.cols + ctu_x]
                    : 64;
            const size_t ctu_idx = static_cast<size_t>(ctu_y) * ctu_map.cols + ctu_x;
            const bool has_class = ctu_idx < ctu_map.classes.size();
            const int qp = tuning.simple_class_policy && has_class
                ? qp_for_class(frame_anchor_qp,
                               ctu_map.classes[ctu_idx],
                               quality,
                               tuning,
                               tuning.active_class_qp_table)
                : qp_from_importance(frame_anchor_qp, importance, quality, tuning);
            out.qp[static_cast<size_t>(by) * out.block_cols + bx] =
                static_cast<uint8_t>(qp);
        }
    }
    std::vector<uint8_t> before_smoothing;
    if (tuning.enable_qp_smoothing) {
        before_smoothing = out.qp;
        const int smoothing_delta = tuning.simple_class_policy
            ? tuning.active_class_qp_table.smoothing_max_delta
            : tuning.max_neighbor_delta;
        smooth_qp_map(out, smoothing_delta);
        uint64_t changed = 0;
        for (size_t i = 0; i < out.qp.size(); ++i) {
            changed += static_cast<uint64_t>(
                std::abs(static_cast<int>(out.qp[i]) - static_cast<int>(before_smoothing[i])));
        }
        out.smoothing_changed_qp_avg = out.qp.empty()
            ? 0.0f
            : static_cast<float>(changed) / static_cast<float>(out.qp.size());
    }
    clamp_qp_map(out, 1, 51);
    return out;
}

QpDistributionStats summarize_qp_distribution(const QpMap16x16& map) {
    QpDistributionStats stats;
    if (map.qp.empty()) return stats;

    std::vector<uint8_t> sorted = map.qp;
    std::sort(sorted.begin(), sorted.end());
    auto percentile_qp = [&](double p) {
        const size_t idx = std::min(sorted.size() - 1,
                                    static_cast<size_t>((sorted.size() - 1) * p));
        return static_cast<int>(sorted[idx]);
    };
    stats.qp_p10 = percentile_qp(0.10);
    stats.qp_p50 = percentile_qp(0.50);
    stats.qp_p90 = percentile_qp(0.90);
    stats.qp_p95 = percentile_qp(0.95);

    size_t high_qp = 0;
    for (uint8_t qp : map.qp) {
        const uint8_t clamped = clamp_u8(qp, 1, 51);
        stats.histogram[clamped]++;
        if (clamped >= 46) high_qp++;
    }
    stats.high_qp_block_ratio = ratio(high_qp, map.qp.size());
    return stats;
}

RegionBlockStats summarize_region_blocks(const CtuMap& ctu_map,
                                         const RoiAnalysisResult& roi) {
    RegionBlockStats stats;
    const size_t total = ctu_map.importance.size();
    if (total == 0) return stats;

    size_t low = 0;
    size_t roi_blocks = 0;
    size_t transition = 0;
    size_t background = 0;
    size_t flat = 0;
    std::array<size_t, 8> classes{};
    for (uint8_t importance : ctu_map.importance) {
        const float f = static_cast<float>(importance) / 255.0f;
        if (f < 0.45f) {
            low++;
            background++;
        }
        if (f < 0.25f) flat++;
        if (f >= 0.75f) roi_blocks++;
        else if (f >= 0.45f) transition++;
    }
    for (CtuBlockClass block_class : ctu_map.classes) {
        const size_t idx = static_cast<size_t>(block_class);
        if (idx < classes.size()) classes[idx]++;
    }
    stats.low_importance_block_ratio = ratio(low, total);
    stats.roi_block_ratio = ratio(roi_blocks, total);
    stats.transition_block_ratio = ratio(transition, total);
    stats.background_block_ratio = ratio(background, total);
    stats.flat_background_block_ratio = ratio(flat, total);

    auto map_ratio = [&](const std::vector<uint8_t>& map) {
        if (map.size() != total) return 0.0f;
        return ratio(static_cast<size_t>(std::count_if(map.begin(), map.end(), [](uint8_t v) {
                         return v > 0;
                     })),
                     total);
    };
    stats.foreground_block_ratio = map_ratio(roi.ctu_foreground);
    stats.edge_block_ratio = map_ratio(roi.ctu_edge);
    stats.texture_block_ratio = map_ratio(roi.ctu_texture);
    stats.true_roi_block_ratio =
        ratio(classes[static_cast<size_t>(CtuBlockClass::StrongRoi)] +
                  classes[static_cast<size_t>(CtuBlockClass::WeakRoi)],
              total);
    stats.edge_texture_roi_block_ratio =
        ratio(classes[static_cast<size_t>(CtuBlockClass::EdgeTextureRoi)], total);
    stats.high_texture_background_block_ratio =
        ratio(classes[static_cast<size_t>(CtuBlockClass::HighTextureBackground)], total);
    stats.hard_scene_background_block_ratio =
        ratio(classes[static_cast<size_t>(CtuBlockClass::HardSceneBackground)], total);
    stats.normal_background_block_ratio =
        ratio(classes[static_cast<size_t>(CtuBlockClass::NormalBackground)], total);
    return stats;
}

void smooth_qp_map(QpMap16x16& map, int max_neighbor_delta) {
    if (map.block_cols <= 0 || map.block_rows <= 0 || max_neighbor_delta <= 0) return;
    for (int y = 0; y < map.block_rows; ++y) {
        for (int x = 1; x < map.block_cols; ++x) {
            uint8_t& cur = map.qp[static_cast<size_t>(y) * map.block_cols + x];
            uint8_t prev = map.qp[static_cast<size_t>(y) * map.block_cols + x - 1];
            if (cur > prev + max_neighbor_delta) cur = prev + max_neighbor_delta;
            if (prev > cur + max_neighbor_delta) cur = prev - max_neighbor_delta;
        }
    }
    for (int y = 1; y < map.block_rows; ++y) {
        for (int x = 0; x < map.block_cols; ++x) {
            uint8_t& cur = map.qp[static_cast<size_t>(y) * map.block_cols + x];
            uint8_t prev = map.qp[static_cast<size_t>(y - 1) * map.block_cols + x];
            if (cur > prev + max_neighbor_delta) cur = prev + max_neighbor_delta;
            if (prev > cur + max_neighbor_delta) cur = prev - max_neighbor_delta;
        }
    }
}

void clamp_qp_map(QpMap16x16& map, int min_qp, int max_qp) {
    for (uint8_t& qp : map.qp) {
        qp = clamp_u8(qp, min_qp, max_qp);
    }
}

} // namespace mfx50rt::hybridtsrq
