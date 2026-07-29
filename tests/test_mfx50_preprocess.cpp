#include "src/algo/mfx50_preprocess.h"
#include "mfx50_realtime.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

int absDiff(int a, int b) {
    return std::abs(a - b);
}

} // namespace

int main() {
    constexpr int width = 64;
    constexpr int height = 64;
    std::vector<uint8_t> y(static_cast<size_t>(width) * height, 112);
    std::vector<uint8_t> uv(static_cast<size_t>(width) * (height / 2), 128);

    for (int yy = 0; yy < height; ++yy) {
        for (int xx = 0; xx < width; ++xx) {
            y[static_cast<size_t>(yy) * width + xx] =
                static_cast<uint8_t>(112 + (((xx + yy) & 1) ? 10 : -10));
        }
    }

    const int plateX = 20;
    const int plateY = 24;
    const int plateW = 24;
    const int plateH = 10;
    for (int yy = plateY; yy < plateY + plateH; ++yy) {
        for (int xx = plateX; xx < plateX + plateW; ++xx) {
            const bool stroke = ((xx - plateX) % 6) < 2 || yy == plateY || yy == plateY + plateH - 1;
            y[static_cast<size_t>(yy) * width + xx] = stroke ? 235 : 36;
        }
    }

    const uint8_t backgroundBefore = y[5 * width + 5];
    const uint8_t plateStrokeBefore = y[plateY * width + plateX];
    const uint8_t plateFillBefore = y[(plateY + 3) * width + plateX + 4];

    const int rc = mfx50_preprocess_semantic_smooth_nv12(
        y.data(), uv.data(), width, height, width, width, 30);
    assert(rc == MFX50_OK);

    const uint8_t backgroundAfter = y[5 * width + 5];
    const uint8_t plateStrokeAfter = y[plateY * width + plateX];
    const uint8_t plateFillAfter = y[(plateY + 3) * width + plateX + 4];

    assert(absDiff(backgroundAfter, backgroundBefore) >= 3);
    assert(absDiff(plateStrokeAfter, plateStrokeBefore) <= 1);
    assert(absDiff(plateFillAfter, plateFillBefore) <= 1);

    return 0;
}
