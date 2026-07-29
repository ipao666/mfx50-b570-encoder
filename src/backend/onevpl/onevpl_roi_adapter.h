#pragma once

#include "src/algo/hybridtsrq/hybridtsrq_types.h"

#include <vector>

namespace mfx50rt::onevpl {

std::vector<mfx50rt::hybridtsrq::RoiBox> cropRoiBoxes(
    const std::vector<mfx50rt::hybridtsrq::RoiBox>& boxes,
    int max_regions);

} // namespace mfx50rt::onevpl
