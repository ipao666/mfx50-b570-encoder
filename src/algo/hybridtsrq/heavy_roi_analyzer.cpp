#include "heavy_roi_analyzer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>

namespace mfx50rt::hybridtsrq {

namespace {

constexpr int kDownsample = 4;
constexpr int kTrafficForegroundThreshold = 16;
constexpr int kNonTrafficForegroundThreshold = 34;
constexpr int kTemporalMotionThreshold = 5;
constexpr int kTextSignAreaBudgetNum = 15;
constexpr int kTextSignAreaBudgetDen = 1000;

int ceil_div(int value, int denom) {
    return (value + denom - 1) / denom;
}

uint8_t clamp_importance(float value) {
    int v = static_cast<int>(std::round(value * 255.0f));
    return static_cast<uint8_t>(std::max(0, std::min(255, v)));
}

float box_iou(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    const int ax1 = ax + aw;
    const int ay1 = ay + ah;
    const int bx1 = bx + bw;
    const int by1 = by + bh;
    const int ix0 = std::max(ax, bx);
    const int iy0 = std::max(ay, by);
    const int ix1 = std::min(ax1, bx1);
    const int iy1 = std::min(ay1, by1);
    const int iw = std::max(0, ix1 - ix0);
    const int ih = std::max(0, iy1 - iy0);
    const int inter = iw * ih;
    const int area_a = std::max(0, aw) * std::max(0, ah);
    const int area_b = std::max(0, bw) * std::max(0, bh);
    const int uni = area_a + area_b - inter;
    return uni > 0 ? static_cast<float>(inter) / static_cast<float>(uni) : 0.0f;
}

} // namespace

void HeavyRoiAnalyzer::configure(int width, int height, int ctu_size, int interval) {
    width_ = width;
    height_ = height;
    ctu_size_ = ctu_size > 0 ? ctu_size : 64;
    interval_ = interval > 0 ? interval : 5;
    ds_w_ = std::max(1, width_ / kDownsample);
    ds_h_ = std::max(1, height_ / kDownsample);
    ds_y_.assign(static_cast<size_t>(ds_w_) * ds_h_, 0);
    ds_u_.assign(ds_y_.size(), 128);
    ds_v_.assign(ds_y_.size(), 128);
    prev_ds_y_.assign(ds_y_.size(), 0);
    bg_y_.assign(ds_y_.size(), 0);
    fg_mask_.assign(ds_y_.size(), 0);
    temp_mask_.assign(ds_y_.size(), 0);
    has_previous_frame_ = false;
    has_chroma_ = false;
}

bool HeavyRoiAnalyzer::shouldAnalyze(const FastFrameFeatures& fast) const {
    if (fast.scene_cut_score >= 0.70f) {
        frames_until_next_ = 0;
    }
    if (frames_until_next_ <= 0) {
        frames_until_next_ = interval_ + overload_extra_interval_;
        return true;
    }
    frames_until_next_--;
    return false;
}

void HeavyRoiAnalyzer::markOverload() {
    overload_extra_interval_ = std::min(20, overload_extra_interval_ + 2);
}

RoiAnalysisResult HeavyRoiAnalyzer::analyzeYPlane(const uint8_t* y,
                                                 int width,
                                                 int height,
                                                 int pitch) {
    return analyzeNV12(y, width, height, pitch, nullptr, 0);
}

RoiAnalysisResult HeavyRoiAnalyzer::analyzeNV12(const uint8_t* y,
                                               int width,
                                               int height,
                                               int pitch,
                                               const uint8_t* uv,
                                               int uv_pitch) {
    RoiAnalysisResult out;
    out.frame_id = frame_id_++;
    out.ctu_cols = ceil_div(width, ctu_size_);
    out.ctu_rows = ceil_div(height, ctu_size_);
    if (!y || width <= 0 || height <= 0 || pitch < width) {
        return out;
    }
    if (width != width_ || height != height_) {
        configure(width, height, ctu_size_, interval_);
    }

    downsample(y, pitch, width, height);
    has_chroma_ = false;
    if (uv && uv_pitch >= width) {
        downsampleNV12Chroma(uv, uv_pitch, width, height);
        has_chroma_ = true;
    }
    if (!has_background_) {
        bg_y_ = ds_y_;
        prev_ds_y_ = ds_y_;
        has_previous_frame_ = true;
        has_background_ = true;
        out.valid = true;
        out.ctu_importance.assign(static_cast<size_t>(out.ctu_cols) * out.ctu_rows, 64);
        return out;
    }
    buildForegroundMask();
    morphOpen();
    extractRois(out, width, height);
    extractTextSignRois(out, width, height);
    buildCtuFeatures(out, width, height);
    out.valid = true;

    for (int y = 0; y < ds_h_; ++y) {
        for (int x = 0; x < ds_w_; ++x) {
            const size_t i = static_cast<size_t>(y) * ds_w_ + x;
            const bool traffic_corridor = isTrafficCorridorPixel(x, y);
            if (fg_mask_[i] == 0 || !traffic_corridor) {
                const int keep = traffic_corridor ? 15 : 7;
                bg_y_[i] = static_cast<uint8_t>(
                    (static_cast<int>(bg_y_[i]) * keep + ds_y_[i]) / (keep + 1));
            }
        }
    }
    prev_ds_y_ = ds_y_;
    has_previous_frame_ = true;
    return out;
}

void HeavyRoiAnalyzer::downsample(const uint8_t* y, int pitch, int width, int height) {
    for (int dy = 0; dy < ds_h_; ++dy) {
        for (int dx = 0; dx < ds_w_; ++dx) {
            int sum = 0;
            int count = 0;
            for (int yy = 0; yy < kDownsample; ++yy) {
                int sy = std::min(height - 1, dy * kDownsample + yy);
                const uint8_t* row = y + static_cast<size_t>(sy) * pitch;
                for (int xx = 0; xx < kDownsample; ++xx) {
                    int sx = std::min(width - 1, dx * kDownsample + xx);
                    sum += row[sx];
                    count++;
                }
            }
            ds_y_[static_cast<size_t>(dy) * ds_w_ + dx] = static_cast<uint8_t>(sum / std::max(1, count));
        }
    }
}

void HeavyRoiAnalyzer::downsampleNV12Chroma(const uint8_t* uv,
                                           int uv_pitch,
                                           int width,
                                           int height) {
    if (!uv || uv_pitch < width) {
        has_chroma_ = false;
        std::fill(ds_u_.begin(), ds_u_.end(), 128);
        std::fill(ds_v_.begin(), ds_v_.end(), 128);
        return;
    }
    for (int dy = 0; dy < ds_h_; ++dy) {
        for (int dx = 0; dx < ds_w_; ++dx) {
            int sum_u = 0;
            int sum_v = 0;
            int count = 0;
            const int chroma_x0 = (dx * kDownsample) / 2;
            const int chroma_y0 = (dy * kDownsample) / 2;
            for (int yy = 0; yy < kDownsample / 2; ++yy) {
                const int cy = std::min(height / 2 - 1, chroma_y0 + yy);
                if (cy < 0) continue;
                const uint8_t* row = uv + static_cast<size_t>(cy) * uv_pitch;
                for (int xx = 0; xx < kDownsample / 2; ++xx) {
                    const int cx = std::min(width / 2 - 1, chroma_x0 + xx);
                    if (cx < 0) continue;
                    const int off = cx * 2;
                    sum_u += row[off];
                    sum_v += row[off + 1];
                    count++;
                }
            }
            const size_t idx = static_cast<size_t>(dy) * ds_w_ + dx;
            ds_u_[idx] = static_cast<uint8_t>(sum_u / std::max(1, count));
            ds_v_[idx] = static_cast<uint8_t>(sum_v / std::max(1, count));
        }
    }
}

void HeavyRoiAnalyzer::buildForegroundMask() {
    int64_t global_delta_sum = 0;
    for (size_t i = 0; i < ds_y_.size(); ++i) {
        global_delta_sum += static_cast<int>(ds_y_[i]) - static_cast<int>(bg_y_[i]);
    }
    const int global_delta = ds_y_.empty()
        ? 0
        : static_cast<int>(global_delta_sum / static_cast<int64_t>(ds_y_.size()));

    for (int y = 0; y < ds_h_; ++y) {
        for (int x = 0; x < ds_w_; ++x) {
            const size_t i = static_cast<size_t>(y) * ds_w_ + x;
            const int bg_diff =
                std::abs(static_cast<int>(ds_y_[i]) - static_cast<int>(bg_y_[i]) - global_delta);
            const int temporal_diff = has_previous_frame_
                ? std::abs(static_cast<int>(ds_y_[i]) - static_cast<int>(prev_ds_y_[i]))
                : bg_diff;
            const bool traffic_corridor = isTrafficCorridorPixel(x, y);
            const int threshold =
                traffic_corridor ? kTrafficForegroundThreshold : kNonTrafficForegroundThreshold;
            const bool traffic_motion =
                traffic_corridor &&
                bg_diff >= threshold &&
                (temporal_diff >= kTemporalMotionThreshold ||
                 bg_diff >= threshold + 12);
            const bool strong_nontraffic_motion =
                !traffic_corridor &&
                y > ds_h_ / 3 &&
                bg_diff >= kNonTrafficForegroundThreshold + 10 &&
                temporal_diff >= kTemporalMotionThreshold + 7;
            fg_mask_[i] = (traffic_motion || strong_nontraffic_motion) ? 255 : 0;
        }
    }
}

bool HeavyRoiAnalyzer::isTrafficCorridorPixel(int x, int y) const {
    if (ds_w_ <= 0 || ds_h_ <= 0) return true;
    const float fx = (static_cast<float>(x) + 0.5f) / static_cast<float>(ds_w_);
    const float fy = (static_cast<float>(y) + 0.5f) / static_cast<float>(ds_h_);
    if (fy < 0.10f) return false;

    const float half_width = 0.24f + 0.39f * fy;
    const bool in_perspective_road =
        std::abs(fx - 0.50f) <= half_width;
    const bool upper_side_texture =
        fy < 0.50f && (fx < 0.12f || fx > 0.88f);
    return in_perspective_road && !upper_side_texture;
}

bool HeavyRoiAnalyzer::isLaneMarkCandidate(size_t idx, int gx, int gy) const {
    if (idx >= ds_y_.size()) return false;
    const int edge = gx + gy;
    return ds_y_[idx] >= 168 && edge >= 34 && gx >= gy / 2;
}

void HeavyRoiAnalyzer::morphOpen() {
    std::fill(temp_mask_.begin(), temp_mask_.end(), 0);
    for (int y = 1; y < ds_h_ - 1; ++y) {
        for (int x = 1; x < ds_w_ - 1; ++x) {
            bool all = true;
            for (int dy = -1; dy <= 1 && all; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (fg_mask_[static_cast<size_t>(y + dy) * ds_w_ + x + dx] == 0) {
                        all = false;
                        break;
                    }
                }
            }
            temp_mask_[static_cast<size_t>(y) * ds_w_ + x] = all ? 255 : 0;
        }
    }
    std::fill(fg_mask_.begin(), fg_mask_.end(), 0);
    for (int y = 1; y < ds_h_ - 1; ++y) {
        for (int x = 1; x < ds_w_ - 1; ++x) {
            bool any = false;
            for (int dy = -1; dy <= 1 && !any; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (temp_mask_[static_cast<size_t>(y + dy) * ds_w_ + x + dx] != 0) {
                        any = true;
                        break;
                    }
                }
            }
            fg_mask_[static_cast<size_t>(y) * ds_w_ + x] = any ? 255 : 0;
        }
    }
}

void HeavyRoiAnalyzer::extractRois(RoiAnalysisResult& out, int width, int height) {
    std::vector<uint8_t> visited(fg_mask_.size(), 0);
    const int min_area = 10;
    const int frame_area = std::max(1, ds_w_ * ds_h_);
    for (int sy = 0; sy < ds_h_; ++sy) {
        for (int sx = 0; sx < ds_w_; ++sx) {
            size_t start = static_cast<size_t>(sy) * ds_w_ + sx;
            if (fg_mask_[start] == 0 || visited[start]) continue;
            int min_x = sx, max_x = sx, min_y = sy, max_y = sy;
            std::vector<std::pair<int, int>> pixels;
            std::queue<std::pair<int, int>> q;
            q.push({sx, sy});
            visited[start] = 1;
            while (!q.empty()) {
                auto [x, y] = q.front();
                q.pop();
                pixels.push_back({x, y});
                min_x = std::min(min_x, x);
                max_x = std::max(max_x, x);
                min_y = std::min(min_y, y);
                max_y = std::max(max_y, y);
                const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& d : dirs) {
                    int nx = x + d[0];
                    int ny = y + d[1];
                    if (nx < 0 || ny < 0 || nx >= ds_w_ || ny >= ds_h_) continue;
                    size_t idx = static_cast<size_t>(ny) * ds_w_ + nx;
                    if (visited[idx] || fg_mask_[idx] == 0) continue;
                    visited[idx] = 1;
                    q.push({nx, ny});
                }
            }

            const int raw_box_w = max_x - min_x + 1;
            const int raw_box_h = max_y - min_y + 1;
            if (raw_box_w <= 0 || raw_box_h <= 0) continue;

            std::vector<int> col_counts(static_cast<size_t>(raw_box_w), 0);
            std::vector<int> row_counts(static_cast<size_t>(raw_box_h), 0);
            for (const auto& p : pixels) {
                col_counts[static_cast<size_t>(p.first - min_x)]++;
                row_counts[static_cast<size_t>(p.second - min_y)]++;
            }

            int trim_min_x = min_x;
            int trim_max_x = max_x;
            int trim_min_y = min_y;
            int trim_max_y = max_y;
            const int col_threshold = std::max(2, raw_box_h / 10);
            const int row_threshold = std::max(2, raw_box_w / 10);
            while (trim_min_x <= trim_max_x &&
                   col_counts[static_cast<size_t>(trim_min_x - min_x)] < col_threshold) {
                trim_min_x++;
            }
            while (trim_max_x >= trim_min_x &&
                   col_counts[static_cast<size_t>(trim_max_x - min_x)] < col_threshold) {
                trim_max_x--;
            }
            while (trim_min_y <= trim_max_y &&
                   row_counts[static_cast<size_t>(trim_min_y - min_y)] < row_threshold) {
                trim_min_y++;
            }
            while (trim_max_y >= trim_min_y &&
                   row_counts[static_cast<size_t>(trim_max_y - min_y)] < row_threshold) {
                trim_max_y--;
            }
            if (trim_min_x > trim_max_x || trim_min_y > trim_max_y) continue;

            int trimmed_area = 0;
            int trimmed_traffic_hits = 0;
            for (const auto& p : pixels) {
                if (p.first < trim_min_x || p.first > trim_max_x ||
                    p.second < trim_min_y || p.second > trim_max_y) {
                    continue;
                }
                trimmed_area++;
                if (isTrafficCorridorPixel(p.first, p.second)) trimmed_traffic_hits++;
            }

            const int box_w = trim_max_x - trim_min_x + 1;
            const int box_h = trim_max_y - trim_min_y + 1;
            const float fill_ratio =
                static_cast<float>(trimmed_area) / static_cast<float>(std::max(1, box_w * box_h));
            const float traffic_ratio =
                static_cast<float>(trimmed_traffic_hits) / std::max(1, trimmed_area);
            const float area_ratio = static_cast<float>(trimmed_area) / static_cast<float>(frame_area);
            const float aspect = static_cast<float>(box_w) / static_cast<float>(std::max(1, box_h));
            const float min_fill = (box_w <= 5 && box_h <= 5) ? 0.20f : 0.10f;
            const bool likely_lane_streak =
                (aspect > 5.5f || aspect < 0.18f) && fill_ratio < 0.42f;
            if (trimmed_area < min_area ||
                box_w < 3 ||
                box_h < 3 ||
                aspect < 0.12f ||
                aspect > 8.0f ||
                area_ratio > 0.14f ||
                fill_ratio < min_fill ||
                likely_lane_streak ||
                traffic_ratio < 0.42f ||
                out.objects.size() >= 64) {
                continue;
            }
            RoiBox box;
            box.x = std::max(0, trim_min_x * kDownsample);
            box.y = std::max(0, trim_min_y * kDownsample);
            box.w = std::min(width - box.x, box_w * kDownsample);
            box.h = std::min(height - box.y, box_h * kDownsample);
            box.confidence =
                std::min(1.0f, 0.22f + trimmed_area / 160.0f + 0.35f * traffic_ratio);
            box.type = 1;
            out.objects.push_back(box);
            if (box.w >= 28 && box.h >= 20 && out.objects.size() < 64) {
                RoiBox plate;
                plate.w = std::max(20, box.w * 65 / 100);
                plate.h = std::max(14, box.h * 28 / 100);
                plate.x = std::max(0, box.x + (box.w - plate.w) / 2);
                plate.y = std::min(height - plate.h,
                                   box.y + std::max(box.h * 52 / 100,
                                                    box.h - plate.h - 4));
                plate.confidence = std::min(1.0f, box.confidence + 0.20f);
                plate.type = 2;
                out.objects.push_back(plate);
            }
        }
    }
}

bool HeavyRoiAnalyzer::isTextSignZonePixel(int x, int y) const {
    if (ds_w_ <= 0 || ds_h_ <= 0) return false;
    const float fx = (static_cast<float>(x) + 0.5f) / static_cast<float>(ds_w_);
    const float fy = (static_cast<float>(y) + 0.5f) / static_cast<float>(ds_h_);
    const bool roadside_sign_zone =
        fy >= 0.08f && fy <= 0.82f && (fx <= 0.24f || fx >= 0.72f);
    if (!roadside_sign_zone) return false;
    if (isTrafficCorridorPixel(x, y)) return false;
    return roadside_sign_zone;
}

bool HeavyRoiAnalyzer::overlapsExistingRoi(const RoiAnalysisResult& out,
                                           int x,
                                           int y,
                                           int w,
                                           int h) const {
    for (const auto& box : out.objects) {
        if (box_iou(x, y, w, h, box.x, box.y, box.w, box.h) > 0.28f) {
            return true;
        }
    }
    return false;
}

void HeavyRoiAnalyzer::extractTextSignRois(RoiAnalysisResult& out, int width, int height) {
    if (ds_w_ <= 2 || ds_h_ <= 2 || out.objects.size() >= 64) return;

    std::vector<uint8_t> stroke_mask(ds_y_.size(), 0);
    std::vector<uint8_t> color_mask(ds_y_.size(), 0);
    std::vector<uint8_t> dilated(ds_y_.size(), 0);
    for (int y = 1; y < ds_h_ - 1; ++y) {
        for (int x = 1; x < ds_w_ - 1; ++x) {
            if (!isTextSignZonePixel(x, y)) continue;
            const size_t idx = static_cast<size_t>(y) * ds_w_ + x;
            int local_min = 255;
            int local_max = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int v = ds_y_[static_cast<size_t>(y + dy) * ds_w_ + x + dx];
                    local_min = std::min(local_min, v);
                    local_max = std::max(local_max, v);
                }
            }
            const int gx = std::abs(static_cast<int>(ds_y_[idx - 1]) -
                                    static_cast<int>(ds_y_[idx + 1]));
            const int gy = std::abs(static_cast<int>(ds_y_[idx - ds_w_]) -
                                    static_cast<int>(ds_y_[idx + ds_w_]));
            const int edge = gx + gy;
            const int contrast = local_max - local_min;
            const bool bright_stroke =
                ds_y_[idx] >= 132 && local_max >= 148 && contrast >= 24 && edge >= 32;
            const bool dark_text_on_panel =
                local_max >= 78 && contrast >= 16 && edge >= 20 &&
                ds_y_[idx] <= local_max - 10;
            const bool high_contrast_stroke =
                local_max >= 95 && contrast >= 30 && edge >= 34;
            if (bright_stroke || dark_text_on_panel || high_contrast_stroke) {
                stroke_mask[idx] = 255;
            }
        }
    }

    const int integral_w = ds_w_ + 1;
    const int integral_area = integral_w * (ds_h_ + 1);
    std::vector<int> stroke_integral(static_cast<size_t>(integral_area), 0);
    std::vector<int> color_integral(static_cast<size_t>(integral_area), 0);
    std::vector<double> y_integral(static_cast<size_t>(integral_area), 0.0);
    std::vector<double> y2_integral(static_cast<size_t>(integral_area), 0.0);
    std::vector<double> u_integral(static_cast<size_t>(integral_area), 0.0);
    std::vector<double> v_integral(static_cast<size_t>(integral_area), 0.0);
    std::vector<double> u2_integral(static_cast<size_t>(integral_area), 0.0);
    std::vector<double> v2_integral(static_cast<size_t>(integral_area), 0.0);
    for (int y = 0; y < ds_h_; ++y) {
        int row_stroke = 0;
        int row_color = 0;
        double row_y = 0.0;
        double row_y2 = 0.0;
        double row_u = 0.0;
        double row_v = 0.0;
        double row_u2 = 0.0;
        double row_v2 = 0.0;
        for (int x = 0; x < ds_w_; ++x) {
            const size_t src_idx = static_cast<size_t>(y) * ds_w_ + x;
            const int v = ds_y_[src_idx];
            const int u_chroma = has_chroma_ ? ds_u_[src_idx] : 128;
            const int v_chroma = has_chroma_ ? ds_v_[src_idx] : 128;
            const int du_chroma = u_chroma - 128;
            const int dv_chroma = v_chroma - 128;
            const bool strong_color_pixel =
                has_chroma_ && (du_chroma * du_chroma + dv_chroma * dv_chroma >= 38 * 38);
            row_stroke += stroke_mask[src_idx] ? 1 : 0;
            if (strong_color_pixel && isTextSignZonePixel(x, y)) {
                color_mask[src_idx] = 255;
            }
            row_color += strong_color_pixel ? 1 : 0;
            row_y += v;
            row_y2 += static_cast<double>(v) * v;
            row_u += u_chroma;
            row_v += v_chroma;
            row_u2 += static_cast<double>(u_chroma) * u_chroma;
            row_v2 += static_cast<double>(v_chroma) * v_chroma;
            const size_t dst_idx = static_cast<size_t>(y + 1) * integral_w + (x + 1);
            const size_t prev_row_idx = static_cast<size_t>(y) * integral_w + (x + 1);
            stroke_integral[dst_idx] = stroke_integral[prev_row_idx] + row_stroke;
            color_integral[dst_idx] = color_integral[prev_row_idx] + row_color;
            y_integral[dst_idx] = y_integral[prev_row_idx] + row_y;
            y2_integral[dst_idx] = y2_integral[prev_row_idx] + row_y2;
            u_integral[dst_idx] = u_integral[prev_row_idx] + row_u;
            v_integral[dst_idx] = v_integral[prev_row_idx] + row_v;
            u2_integral[dst_idx] = u2_integral[prev_row_idx] + row_u2;
            v2_integral[dst_idx] = v2_integral[prev_row_idx] + row_v2;
        }
    }

    auto rect_sum_int = [&](const std::vector<int>& integral,
                            int x0,
                            int y0,
                            int x1,
                            int y1) {
        x0 = std::max(0, std::min(ds_w_, x0));
        y0 = std::max(0, std::min(ds_h_, y0));
        x1 = std::max(0, std::min(ds_w_, x1));
        y1 = std::max(0, std::min(ds_h_, y1));
        return integral[static_cast<size_t>(y1) * integral_w + x1] -
               integral[static_cast<size_t>(y0) * integral_w + x1] -
               integral[static_cast<size_t>(y1) * integral_w + x0] +
               integral[static_cast<size_t>(y0) * integral_w + x0];
    };
    auto rect_sum_double = [&](const std::vector<double>& integral,
                               int x0,
                               int y0,
                               int x1,
                               int y1) {
        x0 = std::max(0, std::min(ds_w_, x0));
        y0 = std::max(0, std::min(ds_h_, y0));
        x1 = std::max(0, std::min(ds_w_, x1));
        y1 = std::max(0, std::min(ds_h_, y1));
        return integral[static_cast<size_t>(y1) * integral_w + x1] -
               integral[static_cast<size_t>(y0) * integral_w + x1] -
               integral[static_cast<size_t>(y1) * integral_w + x0] +
               integral[static_cast<size_t>(y0) * integral_w + x0];
    };

    struct SignCandidate {
        float score = 0.0f;
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        int kind = 0;
        int priority = 0;
    };
    std::vector<SignCandidate> sign_candidates;
    const int side_ranges[2][2] = {
        {1, std::max(2, static_cast<int>(ds_w_ * 0.26f))},
        {std::max(1, static_cast<int>(ds_w_ * 0.70f)), ds_w_ - 1},
    };
    const int scan_y0 = static_cast<int>(ds_h_ * 0.08f);
    const int scan_y1 = static_cast<int>(ds_h_ * 0.82f);
    auto surrounding_contrast = [&](int x, int y, int w, int h, double inside_mean) {
        const int pad_x = std::max(3, w / 3);
        const int pad_y = std::max(3, h / 3);
        const int ox0 = std::max(0, x - pad_x);
        const int oy0 = std::max(0, y - pad_y);
        const int ox1 = std::min(ds_w_, x + w + pad_x);
        const int oy1 = std::min(ds_h_, y + h + pad_y);
        const int outer_area = std::max(1, (ox1 - ox0) * (oy1 - oy0));
        const int inner_area = std::max(1, w * h);
        const int surround_area = std::max(1, outer_area - inner_area);
        const double outer_sum = rect_sum_double(y_integral, ox0, oy0, ox1, oy1);
        const double inner_sum = inside_mean * static_cast<double>(inner_area);
        const double surround_mean = (outer_sum - inner_sum) / static_cast<double>(surround_area);
        return std::abs(inside_mean - surround_mean);
    };
    struct ChromaStats {
        double magnitude = 0.0;
        double variance = 0.0;
        double surround = 0.0;
    };
    auto chroma_stats = [&](int x, int y, int w, int h) {
        ChromaStats stats;
        if (!has_chroma_) return stats;
        const int area = std::max(1, w * h);
        const double sum_u = rect_sum_double(u_integral, x, y, x + w, y + h);
        const double sum_v = rect_sum_double(v_integral, x, y, x + w, y + h);
        const double sum_u2 = rect_sum_double(u2_integral, x, y, x + w, y + h);
        const double sum_v2 = rect_sum_double(v2_integral, x, y, x + w, y + h);
        const double mean_u = sum_u / static_cast<double>(area);
        const double mean_v = sum_v / static_cast<double>(area);
        const double du = mean_u - 128.0;
        const double dv = mean_v - 128.0;
        stats.magnitude = std::sqrt(du * du + dv * dv);
        const double var_u = sum_u2 / static_cast<double>(area) - mean_u * mean_u;
        const double var_v = sum_v2 / static_cast<double>(area) - mean_v * mean_v;
        stats.variance = std::max(0.0, var_u + var_v);

        const int pad_x = std::max(3, w / 3);
        const int pad_y = std::max(3, h / 3);
        const int ox0 = std::max(0, x - pad_x);
        const int oy0 = std::max(0, y - pad_y);
        const int ox1 = std::min(ds_w_, x + w + pad_x);
        const int oy1 = std::min(ds_h_, y + h + pad_y);
        const int outer_area = std::max(1, (ox1 - ox0) * (oy1 - oy0));
        const int surround_area = std::max(1, outer_area - area);
        const double outer_u = rect_sum_double(u_integral, ox0, oy0, ox1, oy1);
        const double outer_v = rect_sum_double(v_integral, ox0, oy0, ox1, oy1);
        const double surround_u = (outer_u - sum_u) / static_cast<double>(surround_area);
        const double surround_v = (outer_v - sum_v) / static_cast<double>(surround_area);
        const double sdu = mean_u - surround_u;
        const double sdv = mean_v - surround_v;
        stats.surround = std::sqrt(sdu * sdu + sdv * sdv);
        return stats;
    };
    auto round_ring_score = [&](int x, int y, int w, int h) {
        int ring_hits = 0;
        int ring_total = 0;
        int inner_hits = 0;
        int inner_total = 0;
        const float cx = static_cast<float>(x) + static_cast<float>(w) * 0.5f;
        const float cy = static_cast<float>(y) + static_cast<float>(h) * 0.5f;
        const float rx = std::max(1.0f, static_cast<float>(w) * 0.5f);
        const float ry = std::max(1.0f, static_cast<float>(h) * 0.5f);
        for (int yy = y; yy < y + h; ++yy) {
            if (yy <= 0 || yy >= ds_h_ - 1) continue;
            for (int xx = x; xx < x + w; ++xx) {
                if (xx <= 0 || xx >= ds_w_ - 1) continue;
                const float nx = (static_cast<float>(xx) + 0.5f - cx) / rx;
                const float ny = (static_cast<float>(yy) + 0.5f - cy) / ry;
                const float r2 = nx * nx + ny * ny;
                const bool hit = stroke_mask[static_cast<size_t>(yy) * ds_w_ + xx] != 0;
                if (r2 >= 0.55f && r2 <= 1.30f) {
                    ring_total++;
                    if (hit) ring_hits++;
                }
                else if (r2 < 0.42f) {
                    inner_total++;
                    if (hit) inner_hits++;
                }
            }
        }
        const float ring_density =
            static_cast<float>(ring_hits) / static_cast<float>(std::max(1, ring_total));
        const float inner_density =
            static_cast<float>(inner_hits) / static_cast<float>(std::max(1, inner_total));
        return ring_density * 2.0f + inner_density;
    };

    if (has_chroma_) {
        std::vector<uint8_t> visited_color(color_mask.size(), 0);
        for (int sy = 1; sy < ds_h_ - 1; ++sy) {
            for (int sx = 1; sx < ds_w_ - 1; ++sx) {
                const size_t start = static_cast<size_t>(sy) * ds_w_ + sx;
                if (!color_mask[start] || visited_color[start]) continue;
                int min_x = sx;
                int max_x = sx;
                int min_y = sy;
                int max_y = sy;
                int area = 0;
                std::queue<std::pair<int, int>> q;
                q.push({sx, sy});
                visited_color[start] = 1;
                while (!q.empty()) {
                    auto [x, y] = q.front();
                    q.pop();
                    area++;
                    min_x = std::min(min_x, x);
                    max_x = std::max(max_x, x);
                    min_y = std::min(min_y, y);
                    max_y = std::max(max_y, y);
                    const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    for (const auto& d : dirs) {
                        const int nx = x + d[0];
                        const int ny = y + d[1];
                        if (nx <= 0 || ny <= 0 || nx >= ds_w_ - 1 || ny >= ds_h_ - 1) continue;
                        const size_t nidx = static_cast<size_t>(ny) * ds_w_ + nx;
                        if (visited_color[nidx] || !color_mask[nidx]) continue;
                        visited_color[nidx] = 1;
                        q.push({nx, ny});
                    }
                }

                const int box_w = max_x - min_x + 1;
                const int box_h = max_y - min_y + 1;
                if (area < 8 || box_w < 4 || box_h < 4) {
                    continue;
                }
                const int box_area = std::max(1, box_w * box_h);
                const float fill_ratio = static_cast<float>(area) / static_cast<float>(box_area);
                const float aspect = static_cast<float>(box_w) / static_cast<float>(std::max(1, box_h));
                if (fill_ratio < 0.24f ||
                    aspect < 0.10f ||
                    aspect > 3.4f ||
                    box_area > std::max(64, ds_w_ * ds_h_ / 35)) {
                    continue;
                }
                const ChromaStats chroma = chroma_stats(min_x, min_y, box_w, box_h);
                if (chroma.magnitude < 34.0 || chroma.variance > 820.0) {
                    continue;
                }
                const int stroke = rect_sum_int(stroke_integral, min_x, min_y, max_x + 1, max_y + 1);
                const double sum_y = rect_sum_double(y_integral, min_x, min_y, max_x + 1, max_y + 1);
                const double mean = sum_y / static_cast<double>(box_area);
                const double contrast_to_surround =
                    surrounding_contrast(min_x, min_y, box_w, box_h, mean);
                if (stroke < std::max(2, box_area / 160) && contrast_to_surround < 4.0) {
                    continue;
                }
                float score =
                    10.0f + fill_ratio * 2.2f +
                    static_cast<float>(std::min(30.0, chroma.magnitude)) / 18.0f +
                    static_cast<float>(std::min(18.0, contrast_to_surround)) / 18.0f;
                const bool round_like = aspect >= 0.55f && aspect <= 1.80f;
                if (round_like) score += round_ring_score(min_x, min_y, box_w, box_h) * 2.0f;
                const int kind = round_like ? 3 : (aspect < 0.82f ? 1 : 2);
                sign_candidates.push_back({score, min_x, min_y, box_w, box_h, kind, 1});
            }
        }
    }

    for (const auto& range : side_ranges) {
        const int scan_x0 = range[0];
        const int scan_x1 = range[1];
        if (scan_x1 - scan_x0 < 8) continue;
        for (int wh = 10; wh <= std::min(112, scan_y1 - scan_y0); wh += 6) {
            for (int ww = 8; ww <= std::min(96, scan_x1 - scan_x0); ww += 6) {
                const float aspect =
                    static_cast<float>(ww) / static_cast<float>(std::max(1, wh));
                if (aspect < 0.10f || aspect > 3.20f) continue;
                const bool vertical_panel = aspect >= 0.12f && aspect <= 0.82f && wh >= 28;
                const bool horizontal_panel = aspect >= 0.82f && aspect <= 3.20f && ww >= 20 && wh >= 10;
                const bool round_or_square_sign =
                    aspect >= 0.55f && aspect <= 1.80f && std::min(ww, wh) >= 10 &&
                    std::max(ww, wh) <= 64;
                if (!vertical_panel && !horizontal_panel && !round_or_square_sign) continue;
                for (int y = scan_y0; y + wh <= scan_y1; y += 4) {
                    for (int x = scan_x0; x + ww <= scan_x1; x += 4) {
                        const int center_px = x + ww / 2;
                        const int center_py = y + wh / 2;
                        if (!isTextSignZonePixel(center_px, center_py)) continue;
                        const int area = ww * wh;
                        const int stroke = rect_sum_int(stroke_integral, x, y, x + ww, y + wh);
                        const int strong_color =
                            rect_sum_int(color_integral, x, y, x + ww, y + wh);
                        const float strong_color_ratio =
                            static_cast<float>(strong_color) / static_cast<float>(area);
                        const int min_stroke =
                            (vertical_panel || round_or_square_sign)
                                ? std::max(4, area / 180)
                                : std::max(6, area / 110);
                        if (stroke < min_stroke) continue;
                        const float density = static_cast<float>(stroke) / static_cast<float>(area);
                        const float min_density =
                            (vertical_panel || round_or_square_sign) ? 0.004f : 0.007f;
                        if (density < min_density || density > 0.24f) continue;
                        const double sum_y = rect_sum_double(y_integral, x, y, x + ww, y + wh);
                        const double sum_y2 = rect_sum_double(y2_integral, x, y, x + ww, y + wh);
                        const double mean = sum_y / static_cast<double>(area);
                        const double var = sum_y2 / static_cast<double>(area) - mean * mean;
                        const double max_var =
                            (vertical_panel || round_or_square_sign) ? 1700.0 : 2100.0;
                        if (mean < 32.0 || mean > 182.0 || var > max_var) continue;
                        const double contrast_to_surround =
                            surrounding_contrast(x, y, ww, wh, mean);
                        const ChromaStats chroma = chroma_stats(x, y, ww, wh);
                        const bool colored_panel =
                            has_chroma_ &&
                            chroma.magnitude >= 34.0 &&
                            chroma.variance <= 520.0 &&
                            chroma.surround >= 4.5;
                        const bool foliage_like_chroma =
                            has_chroma_ &&
                            chroma.variance > 360.0 &&
                            chroma.surround < 9.0;
                        const float ring_score =
                            round_or_square_sign ? round_ring_score(x, y, ww, wh) : 0.0f;
                        const bool has_panel_contrast = contrast_to_surround >= 5.0;
                        const bool has_round_edge = round_or_square_sign && ring_score >= 0.08f;
                        if (round_or_square_sign &&
                            !colored_panel &&
                            density > 0.20f &&
                            chroma.magnitude < 12.0) {
                            continue;
                        }
                        if (round_or_square_sign &&
                            !colored_panel &&
                            density > 0.14f &&
                            var > 360.0) {
                            continue;
                        }
                        if (has_chroma_ &&
                            strong_color_ratio > 0.015f &&
                            strong_color_ratio < 0.20f &&
                            (vertical_panel || horizontal_panel || round_or_square_sign)) {
                            continue;
                        }
                        if (has_chroma_ && chroma.variance > 700.0 && !colored_panel) {
                            continue;
                        }
                        if (foliage_like_chroma && !has_round_edge && !colored_panel) {
                            continue;
                        }
                        if (!colored_panel && !has_panel_contrast && !has_round_edge &&
                            stroke < area / 40) {
                            continue;
                        }
                        const float variance_score =
                            static_cast<float>(std::max(0.0, max_var - var) / max_var);
                        const float size_score =
                            static_cast<float>(std::min(ww, wh)) /
                            static_cast<float>(std::max(1, std::min(ds_w_, ds_h_)));
                        float score =
                            density * 70.0f + variance_score * 1.1f +
                            static_cast<float>(std::min(24.0, contrast_to_surround)) / 16.0f +
                            size_score * 2.0f + std::min(1.1f, stroke / 90.0f);
                        if (vertical_panel) score += 0.7f;
                        if (horizontal_panel) score += 0.4f;
                        if (round_or_square_sign) score += ring_score * 4.0f + 0.5f;
                        if (colored_panel) {
                            score +=
                                std::min(2.4f,
                                         static_cast<float>(chroma.magnitude / 18.0 +
                                                            chroma.surround / 18.0));
                        }
                        if (!colored_panel && !has_round_edge) {
                            score = std::min(score, 8.0f);
                        }
                        if (score < 2.8f) continue;
                        const int kind = round_or_square_sign ? 3 : (vertical_panel ? 1 : 2);
                        sign_candidates.push_back({score, x, y, ww, wh, kind});
                    }
                }
            }
        }
    }
    std::sort(sign_candidates.begin(),
              sign_candidates.end(),
              [](const SignCandidate& a, const SignCandidate& b) {
                  if (a.priority != b.priority) {
                      return a.priority > b.priority;
                  }
                  return a.score > b.score;
              });
    int added = 0;
    int text_area_used = 0;
    const int text_area_budget =
        std::max(std::max(1, width * height * kTextSignAreaBudgetNum / kTextSignAreaBudgetDen),
                 std::min(width * height / 12, 8192));
    for (const auto& cand : sign_candidates) {
        if (out.objects.size() >= 64) break;
        if (added >= 4) break;
        RoiBox box;
        box.x = std::max(0, (cand.x - 1) * kDownsample);
        box.y = std::max(0, (cand.y - 2) * kDownsample);
        box.w = std::min(width - box.x, (cand.w + 2) * kDownsample);
        box.h = std::min(height - box.y, (cand.h + 4) * kDownsample);
        if (cand.kind == 1) {
            if (box.w < 24 || box.h < 64) {
                continue;
            }
        }
        else {
            if (box.w < 24 || box.h < 28) continue;
        }
        if (text_area_used + box.w * box.h > text_area_budget) {
            continue;
        }
        if (overlapsExistingRoi(out, box.x, box.y, box.w, box.h)) {
            continue;
        }
        bool duplicate = false;
        for (const auto& existing : out.objects) {
            if (existing.type == 3 &&
                box_iou(box.x, box.y, box.w, box.h, existing.x, existing.y, existing.w, existing.h) >
                    0.18f) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        box.confidence = std::min(1.0f, 0.55f + cand.score * 0.05f);
        box.type = 3;
        out.objects.push_back(box);
        text_area_used += box.w * box.h;
        added++;
    }

    for (int y = 1; y < ds_h_ - 1; ++y) {
        for (int x = 1; x < ds_w_ - 1; ++x) {
            const size_t idx = static_cast<size_t>(y) * ds_w_ + x;
            if (!stroke_mask[idx]) continue;
            for (int dy = -5; dy <= 5; ++dy) {
                const int yy = y + dy;
                if (yy <= 0 || yy >= ds_h_ - 1) continue;
                for (int dx = -4; dx <= 4; ++dx) {
                    const int xx = x + dx;
                    if (xx <= 0 || xx >= ds_w_ - 1) continue;
                    if (isTextSignZonePixel(xx, yy)) {
                        dilated[static_cast<size_t>(yy) * ds_w_ + xx] = 255;
                    }
                }
            }
        }
    }

    std::vector<uint8_t> visited(dilated.size(), 0);
    for (int sy = 1; sy < ds_h_ - 1 && added < 8 && out.objects.size() < 64; ++sy) {
        for (int sx = 1; sx < ds_w_ - 1 && added < 8 && out.objects.size() < 64; ++sx) {
            const size_t start = static_cast<size_t>(sy) * ds_w_ + sx;
            if (!dilated[start] || visited[start]) continue;

            int min_x = sx, max_x = sx, min_y = sy, max_y = sy;
            int stroke_pixels = 0;
            int bright_pixels = 0;
            std::queue<std::pair<int, int>> q;
            q.push({sx, sy});
            visited[start] = 1;
            while (!q.empty()) {
                auto [x, y] = q.front();
                q.pop();
                const size_t idx = static_cast<size_t>(y) * ds_w_ + x;
                if (stroke_mask[idx]) {
                    stroke_pixels++;
                    if (ds_y_[idx] >= 142) bright_pixels++;
                }
                min_x = std::min(min_x, x);
                max_x = std::max(max_x, x);
                min_y = std::min(min_y, y);
                max_y = std::max(max_y, y);
                const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& d : dirs) {
                    const int nx = x + d[0];
                    const int ny = y + d[1];
                    if (nx <= 0 || ny <= 0 || nx >= ds_w_ - 1 || ny >= ds_h_ - 1) continue;
                    const size_t nidx = static_cast<size_t>(ny) * ds_w_ + nx;
                    if (visited[nidx] || !dilated[nidx]) continue;
                    visited[nidx] = 1;
                    q.push({nx, ny});
                }
            }

            if (stroke_pixels < 3) continue;

            const int raw_box_w_ds = max_x - min_x + 1;
            const int raw_box_h_ds = max_y - min_y + 1;
            if (raw_box_w_ds <= 0 || raw_box_h_ds <= 0) continue;
            std::vector<int> col_counts(static_cast<size_t>(raw_box_w_ds), 0);
            std::vector<int> row_counts(static_cast<size_t>(raw_box_h_ds), 0);
            for (int yy = min_y; yy <= max_y; ++yy) {
                for (int xx = min_x; xx <= max_x; ++xx) {
                    if (!stroke_mask[static_cast<size_t>(yy) * ds_w_ + xx]) continue;
                    col_counts[static_cast<size_t>(xx - min_x)]++;
                    row_counts[static_cast<size_t>(yy - min_y)]++;
                }
            }
            int trim_min_x = min_x;
            int trim_max_x = max_x;
            int trim_min_y = min_y;
            int trim_max_y = max_y;
            while (trim_min_x <= trim_max_x &&
                   col_counts[static_cast<size_t>(trim_min_x - min_x)] == 0) {
                trim_min_x++;
            }
            while (trim_max_x >= trim_min_x &&
                   col_counts[static_cast<size_t>(trim_max_x - min_x)] == 0) {
                trim_max_x--;
            }
            while (trim_min_y <= trim_max_y &&
                   row_counts[static_cast<size_t>(trim_min_y - min_y)] == 0) {
                trim_min_y++;
            }
            while (trim_max_y >= trim_min_y &&
                   row_counts[static_cast<size_t>(trim_max_y - min_y)] == 0) {
                trim_max_y--;
            }
            if (trim_min_x > trim_max_x || trim_min_y > trim_max_y) continue;
            min_x = std::max(1, trim_min_x - 2);
            max_x = std::min(ds_w_ - 2, trim_max_x + 2);
            min_y = std::max(1, trim_min_y - 2);
            max_y = std::min(ds_h_ - 2, trim_max_y + 2);

            stroke_pixels = 0;
            bright_pixels = 0;
            for (int yy = min_y; yy <= max_y; ++yy) {
                for (int xx = min_x; xx <= max_x; ++xx) {
                    if (!stroke_mask[static_cast<size_t>(yy) * ds_w_ + xx]) continue;
                    stroke_pixels++;
                    if (ds_y_[static_cast<size_t>(yy) * ds_w_ + xx] >= 142) bright_pixels++;
                }
            }
            if (stroke_pixels < 6) continue;

            const int box_w_ds = max_x - min_x + 1;
            const int box_h_ds = max_y - min_y + 1;
            const int box_area_ds = std::max(1, box_w_ds * box_h_ds);
            const float fill_ratio =
                static_cast<float>(stroke_pixels) / static_cast<float>(box_area_ds);
            const float bright_ratio =
                static_cast<float>(bright_pixels) / static_cast<float>(std::max(1, stroke_pixels));
            const double comp_sum =
                rect_sum_double(y_integral, min_x, min_y, max_x + 1, max_y + 1);
            const double comp_mean = comp_sum / static_cast<double>(box_area_ds);
            const double comp_surround =
                surrounding_contrast(min_x, min_y, box_w_ds, box_h_ds, comp_mean);
            const ChromaStats comp_chroma = chroma_stats(min_x, min_y, box_w_ds, box_h_ds);
            const bool comp_colored_panel =
                has_chroma_ &&
                comp_chroma.magnitude >= 34.0 &&
                comp_chroma.variance <= 520.0 &&
                comp_chroma.surround >= 4.5;
            const bool comp_foliage_like =
                has_chroma_ &&
                comp_chroma.variance > 360.0 &&
                comp_chroma.surround < 9.0;
            if (comp_foliage_like && !comp_colored_panel) continue;

            RoiBox box;
            box.x = std::max(0, min_x * kDownsample);
            box.y = std::max(0, min_y * kDownsample);
            box.w = std::min(width - box.x, box_w_ds * kDownsample);
            box.h = std::min(height - box.y, box_h_ds * kDownsample);
            if (box.w < 24 || box.h < 28) continue;

            const int full_area = std::max(1, box.w * box.h);
            const int frame_area = std::max(1, width * height);
            const float aspect =
                static_cast<float>(box.w) / static_cast<float>(std::max(1, box.h));
            if (box.h > std::max(120, height / 3) ||
                box.w > std::max(180, width / 5) ||
                full_area > frame_area / 45 ||
                aspect < 0.10f ||
                aspect > 3.2f) {
                continue;
            }
            if (fill_ratio < 0.010f || fill_ratio > 0.42f) continue;
            if (stroke_pixels < 6) continue;
            if (!comp_colored_panel &&
                comp_surround < 5.0 &&
                stroke_pixels < box_area_ds / 35) {
                continue;
            }
            if (text_area_used + box.w * box.h > text_area_budget) continue;
            if (overlapsExistingRoi(out, box.x, box.y, box.w, box.h)) continue;

            box.confidence =
                std::min(1.0f,
                         0.38f + std::min(0.36f, stroke_pixels / 110.0f) +
                             0.20f * bright_ratio);
            box.type = 3;
            out.objects.push_back(box);
            text_area_used += box.w * box.h;
            added++;
        }
    }
}

void HeavyRoiAnalyzer::buildCtuFeatures(RoiAnalysisResult& out, int width, int height) {
    const int count = out.ctu_cols * out.ctu_rows;
    out.ctu_foreground.assign(count, 0);
    out.ctu_edge.assign(count, 0);
    out.ctu_texture.assign(count, 0);
    out.ctu_motion.assign(count, 0);
    out.ctu_importance.assign(count, 0);

    int total_fg = 0;
    for (int cy = 0; cy < out.ctu_rows; ++cy) {
        for (int cx = 0; cx < out.ctu_cols; ++cx) {
            int fg = 0;
            int edge = 0;
            int tex = 0;
            int lane = 0;
            int traffic = 0;
            int total = 0;
            int x0 = (cx * ctu_size_) / kDownsample;
            int y0 = (cy * ctu_size_) / kDownsample;
            int x1 = std::min(ds_w_, ceil_div(std::min(width, (cx + 1) * ctu_size_), kDownsample));
            int y1 = std::min(ds_h_, ceil_div(std::min(height, (cy + 1) * ctu_size_), kDownsample));
            for (int y = y0 + 1; y < y1 - 1; ++y) {
                for (int x = x0 + 1; x < x1 - 1; ++x) {
                    size_t idx = static_cast<size_t>(y) * ds_w_ + x;
                    int gx = std::abs(static_cast<int>(ds_y_[idx - 1]) - ds_y_[idx + 1]);
                    int gy = std::abs(static_cast<int>(ds_y_[idx - ds_w_]) - ds_y_[idx + ds_w_]);
                    const bool traffic_corridor = isTrafficCorridorPixel(x, y);
                    if (traffic_corridor) traffic++;
                    if (fg_mask_[idx] && traffic_corridor) fg++;
                    if (traffic_corridor && isLaneMarkCandidate(idx, gx, gy)) lane++;
                    if (traffic_corridor && gx + gy > 40) edge++;
                    if (traffic_corridor) tex += gx + gy;
                    total++;
                }
            }
            total = std::max(1, total);
            total_fg += fg;
            float fg_score = static_cast<float>(fg) / total;
            float lane_score = static_cast<float>(lane) / total;
            float traffic_score = static_cast<float>(traffic) / total;
            float edge_score = static_cast<float>(edge) / total;
            float texture_score = std::min(1.0f, static_cast<float>(tex) / total / 120.0f);
            float motion_score = fg_score;
            float roi_track_score = fg_score > 0.035f ? 1.0f : 0.0f;
            bool text_sign_roi = false;
            const int ctu_x0 = cx * ctu_size_;
            const int ctu_y0 = cy * ctu_size_;
            const int ctu_x1 = std::min(width, (cx + 1) * ctu_size_);
            const int ctu_y1 = std::min(height, (cy + 1) * ctu_size_);
            for (const auto& box : out.objects) {
                if (box.type != 3) continue;
                const int ix0 = std::max(ctu_x0, box.x);
                const int iy0 = std::max(ctu_y0, box.y);
                const int ix1 = std::min(ctu_x1, box.x + box.w);
                const int iy1 = std::min(ctu_y1, box.y + box.h);
                if (ix1 > ix0 && iy1 > iy0) {
                    text_sign_roi = true;
                    break;
                }
            }
            float importance =
                0.62f * fg_score +
                0.20f * lane_score +
                0.08f * motion_score +
                0.06f * roi_track_score +
                0.04f * traffic_score;
            if (fg_score > 0.035f) importance = std::max(importance, 0.80f);
            else if (lane_score > 0.06f) importance = std::max(importance, 0.60f);
            else if (text_sign_roi) importance = std::max(importance, 0.56f);
            else if (traffic_score > 0.50f) importance = std::max(importance, 0.28f);
            if (fg_score < 0.015f && lane_score < 0.04f) {
                importance = std::min(importance, traffic_score > 0.40f ? 0.32f : 0.16f);
            }
            if (text_sign_roi) {
                importance = std::max(importance, 0.56f);
                edge_score = std::max(edge_score, 0.12f);
            }
            size_t out_idx = static_cast<size_t>(cy) * out.ctu_cols + cx;
            const float semantic_fg = fg_score > 0.035f ? std::max(fg_score, 0.12f) : fg_score;
            out.ctu_foreground[out_idx] = clamp_importance(semantic_fg);
            out.ctu_edge[out_idx] = clamp_importance(edge_score);
            out.ctu_texture[out_idx] = clamp_importance(texture_score);
            out.ctu_motion[out_idx] = clamp_importance(std::max(motion_score, lane_score * 0.50f));
            out.ctu_importance[out_idx] = clamp_importance(importance);
        }
    }
    out.foreground_ratio = static_cast<float>(total_fg) / std::max<size_t>(1, fg_mask_.size());
}

} // namespace mfx50rt::hybridtsrq
