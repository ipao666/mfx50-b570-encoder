#include "onevpl_roi_adapter.h"

#include <algorithm>

namespace mfx50rt::onevpl {

std::vector<mfx50rt::hybridtsrq::RoiBox> cropRoiBoxes(
    const std::vector<mfx50rt::hybridtsrq::RoiBox>& boxes,
    int max_regions) {
    std::vector<mfx50rt::hybridtsrq::RoiBox> out = boxes;
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.confidence > b.confidence;
    });
    if (max_regions > 0 && out.size() > static_cast<size_t>(max_regions)) {
        out.resize(static_cast<size_t>(max_regions));
    }
    return out;
}

} // namespace mfx50rt::onevpl
