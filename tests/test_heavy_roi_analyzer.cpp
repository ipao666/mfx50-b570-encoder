#include "src/algo/hybridtsrq/heavy_roi_analyzer.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using mfx50rt::hybridtsrq::HeavyRoiAnalyzer;
using mfx50rt::hybridtsrq::RoiAnalysisResult;
using mfx50rt::hybridtsrq::RoiBox;

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 360;

void set_px(std::vector<uint8_t>& y, int x, int yy, uint8_t value) {
    if (x < 0 || yy < 0 || x >= kWidth || yy >= kHeight) return;
    y[static_cast<size_t>(yy) * kWidth + x] = value;
}

void set_uv(std::vector<uint8_t>& uv, int x, int yy, uint8_t u, uint8_t v) {
    if (x < 0 || yy < 0 || x >= kWidth || yy >= kHeight) return;
    const int cx = x / 2;
    const int cy = yy / 2;
    const size_t off = static_cast<size_t>(cy) * kWidth + static_cast<size_t>(cx) * 2;
    if (off + 1 >= uv.size()) return;
    uv[off] = u;
    uv[off + 1] = v;
}

void fill_rect(std::vector<uint8_t>& y, int x, int yy, int w, int h, uint8_t value) {
    for (int py = yy; py < yy + h; ++py) {
        for (int px = x; px < x + w; ++px) {
            set_px(y, px, py, value);
        }
    }
}

void fill_uv_rect(std::vector<uint8_t>& uv,
                  int x,
                  int yy,
                  int w,
                  int h,
                  uint8_t u,
                  uint8_t v) {
    for (int py = yy; py < yy + h; ++py) {
        for (int px = x; px < x + w; ++px) {
            set_uv(uv, px, py, u, v);
        }
    }
}

void stroke_rect(std::vector<uint8_t>& y, int x, int yy, int w, int h, uint8_t value) {
    for (int px = x; px < x + w; ++px) {
        set_px(y, px, yy, value);
        set_px(y, px, yy + h - 1, value);
    }
    for (int py = yy; py < yy + h; ++py) {
        set_px(y, x, py, value);
        set_px(y, x + w - 1, py, value);
    }
}

void fill_circle(std::vector<uint8_t>& y, int cx, int cy, int r, uint8_t value) {
    const int r2 = r * r;
    for (int py = cy - r; py <= cy + r; ++py) {
        for (int px = cx - r; px <= cx + r; ++px) {
            const int dx = px - cx;
            const int dy = py - cy;
            if (dx * dx + dy * dy <= r2) set_px(y, px, py, value);
        }
    }
}

void fill_uv_circle(std::vector<uint8_t>& uv,
                    int cx,
                    int cy,
                    int r,
                    uint8_t u,
                    uint8_t v) {
    const int r2 = r * r;
    for (int py = cy - r; py <= cy + r; ++py) {
        for (int px = cx - r; px <= cx + r; ++px) {
            const int dx = px - cx;
            const int dy = py - cy;
            if (dx * dx + dy * dy <= r2) set_uv(uv, px, py, u, v);
        }
    }
}

void stroke_circle(std::vector<uint8_t>& y, int cx, int cy, int r, uint8_t value) {
    const int inner = (r - 3) * (r - 3);
    const int outer = (r + 3) * (r + 3);
    for (int py = cy - r - 3; py <= cy + r + 3; ++py) {
        for (int px = cx - r - 3; px <= cx + r + 3; ++px) {
            const int dx = px - cx;
            const int dy = py - cy;
            const int d2 = dx * dx + dy * dy;
            if (d2 >= inner && d2 <= outer) set_px(y, px, py, value);
        }
    }
}

void add_tree_texture(std::vector<uint8_t>& y, int x, int yy, int w, int h) {
    for (int py = yy; py < yy + h; ++py) {
        for (int px = x; px < x + w; ++px) {
            const int v = 54 + ((px * 17 + py * 31 + ((px ^ py) * 7)) & 95);
            set_px(y, px, py, static_cast<uint8_t>(v));
        }
    }
}

void add_tree_chroma(std::vector<uint8_t>& uv, int x, int yy, int w, int h) {
    for (int py = yy; py < yy + h; ++py) {
        for (int px = x; px < x + w; ++px) {
            const int jitter = ((px * 19 + py * 23 + ((px ^ py) * 11)) & 63) - 32;
            const int u = std::max(0, std::min(255, 104 + jitter));
            const int v = std::max(0, std::min(255, 108 - jitter / 2));
            set_uv(uv, px, py, static_cast<uint8_t>(u), static_cast<uint8_t>(v));
        }
    }
}

bool overlap(const RoiBox& box, int x, int y, int w, int h) {
    const int ix0 = std::max(box.x, x);
    const int iy0 = std::max(box.y, y);
    const int ix1 = std::min(box.x + box.w, x + w);
    const int iy1 = std::min(box.y + box.h, y + h);
    return ix1 > ix0 && iy1 > iy0;
}

bool has_text_roi_overlap(const RoiAnalysisResult& result, int x, int y, int w, int h) {
    for (const auto& box : result.objects) {
        if (box.type == 3 && overlap(box, x, y, w, h)) return true;
    }
    return false;
}

void assert_no_text_roi_overlap(const RoiAnalysisResult& result, int x, int y, int w, int h) {
    if (!has_text_roi_overlap(result, x, y, w, h)) return;
    std::fprintf(stderr, "unexpected text ROI overlap with %d,%d %dx%d\n", x, y, w, h);
    for (const auto& box : result.objects) {
        if (box.type != 3) continue;
        std::fprintf(stderr,
                     "text roi: x=%d y=%d w=%d h=%d conf=%.3f\n",
                     box.x,
                     box.y,
                     box.w,
                     box.h,
                     box.confidence);
    }
    assert(false);
}

void assert_text_roi_overlap(const RoiAnalysisResult& result, int x, int y, int w, int h) {
    if (has_text_roi_overlap(result, x, y, w, h)) return;
    std::fprintf(stderr, "missing text ROI overlap with %d,%d %dx%d\n", x, y, w, h);
    for (const auto& box : result.objects) {
        if (box.type != 3) continue;
        std::fprintf(stderr,
                     "text roi: x=%d y=%d w=%d h=%d conf=%.3f\n",
                     box.x,
                     box.y,
                     box.w,
                     box.h,
                     box.confidence);
    }
    assert(false);
}

int text_roi_area(const RoiAnalysisResult& result) {
    int area = 0;
    for (const auto& box : result.objects) {
        if (box.type == 3) area += box.w * box.h;
    }
    return area;
}

RoiAnalysisResult analyze_static_nv12_frame(const std::vector<uint8_t>& y,
                                            const std::vector<uint8_t>& uv) {
    HeavyRoiAnalyzer analyzer;
    analyzer.configure(kWidth, kHeight, 64, 1);
    (void)analyzer.analyzeNV12(y.data(), kWidth, kHeight, kWidth, uv.data(), kWidth);
    return analyzer.analyzeNV12(y.data(), kWidth, kHeight, kWidth, uv.data(), kWidth);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "--dump-nv12") {
        const int width = std::atoi(argv[3]);
        const int height = std::atoi(argv[4]);
        std::ifstream in(argv[2], std::ios::binary);
        std::vector<uint8_t> data(static_cast<size_t>(width) * height * 3 / 2);
        in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!in || width <= 0 || height <= 0) {
            std::fprintf(stderr, "failed to read nv12 frame\n");
            return 2;
        }
        HeavyRoiAnalyzer analyzer;
        analyzer.configure(width, height, 64, 1);
        (void)analyzer.analyzeNV12(data.data(),
                                   width,
                                   height,
                                   width,
                                   data.data() + static_cast<size_t>(width) * height,
                                   width);
        RoiAnalysisResult result =
            analyzer.analyzeNV12(data.data(),
                                 width,
                                 height,
                                 width,
                                 data.data() + static_cast<size_t>(width) * height,
                                 width);
        for (size_t i = 0; i < result.objects.size(); ++i) {
            const RoiBox& box = result.objects[i];
            std::printf("idx=%zu type=%d x=%d y=%d w=%d h=%d conf=%.3f\n",
                        i,
                        box.type,
                        box.x,
                        box.y,
                        box.w,
                        box.h,
                        box.confidence);
        }
        return 0;
    }

    std::vector<uint8_t> vertical_sign(static_cast<size_t>(kWidth) * kHeight, 92);
    std::vector<uint8_t> vertical_uv(static_cast<size_t>(kWidth) * kHeight / 2, 128);
    add_tree_texture(vertical_sign, 470, 35, 95, 250);
    add_tree_chroma(vertical_uv, 470, 35, 95, 250);
    fill_rect(vertical_sign, 570, 58, 44, 138, 146);
    fill_uv_rect(vertical_uv, 570, 58, 44, 138, 94, 176);
    stroke_rect(vertical_sign, 570, 58, 44, 138, 42);
    for (int i = 0; i < 5; ++i) {
        fill_rect(vertical_sign, 583, 75 + i * 22, 18, 5, 34);
        fill_rect(vertical_sign, 591, 68 + i * 22, 5, 18, 34);
    }
    RoiAnalysisResult vertical = analyze_static_nv12_frame(vertical_sign, vertical_uv);
    assert_text_roi_overlap(vertical, 570, 58, 44, 138);
    assert_no_text_roi_overlap(vertical, 485, 80, 46, 140);
    assert(text_roi_area(vertical) <= kWidth * kHeight / 12);

    std::vector<uint8_t> budget_sign(static_cast<size_t>(kWidth) * kHeight, 88);
    std::vector<uint8_t> budget_uv(static_cast<size_t>(kWidth) * kHeight / 2, 128);
    add_tree_texture(budget_sign, 430, 70, 90, 150);
    add_tree_texture(budget_sign, 35, 80, 140, 120);
    add_tree_texture(budget_sign, 205, 95, 105, 120);
    add_tree_chroma(budget_uv, 430, 70, 90, 150);
    fill_rect(budget_sign, 560, 62, 42, 132, 148);
    fill_uv_rect(budget_uv, 560, 62, 42, 132, 94, 176);
    stroke_rect(budget_sign, 560, 62, 42, 132, 42);
    for (int i = 0; i < 4; ++i) {
        fill_rect(budget_sign, 574, 80 + i * 26, 16, 6, 32);
        fill_rect(budget_sign, 582, 72 + i * 26, 5, 20, 32);
    }
    RoiAnalysisResult budgeted = analyze_static_nv12_frame(budget_sign, budget_uv);
    assert_text_roi_overlap(budgeted, 560, 62, 42, 132);
    assert_no_text_roi_overlap(budgeted, 430, 80, 80, 120);
    assert_no_text_roi_overlap(budgeted, 60, 90, 95, 90);
    assert(text_roi_area(budgeted) <= kWidth * kHeight / 10);

    std::vector<uint8_t> round_sign(static_cast<size_t>(kWidth) * kHeight, 90);
    std::vector<uint8_t> round_uv(static_cast<size_t>(kWidth) * kHeight / 2, 128);
    add_tree_texture(round_sign, 475, 45, 90, 230);
    add_tree_chroma(round_uv, 475, 45, 90, 230);
    fill_circle(round_sign, 606, 96, 24, 166);
    fill_uv_circle(round_uv, 606, 96, 24, 96, 174);
    stroke_circle(round_sign, 606, 96, 24, 30);
    fill_rect(round_sign, 592, 91, 28, 8, 30);
    fill_rect(round_sign, 602, 80, 7, 32, 30);
    RoiAnalysisResult round = analyze_static_nv12_frame(round_sign, round_uv);
    assert_text_roi_overlap(round, 582, 72, 48, 56);
    assert_no_text_roi_overlap(round, 490, 90, 40, 130);
    assert(text_roi_area(round) <= kWidth * kHeight / 12);

    return 0;
}
