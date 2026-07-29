#include "mfx50_mbqp_builder.h"

#include <algorithm>

namespace mfx50rt {

MbqpMap buildNeutralMbqpMap(int width, int height, int blockSize, uint8_t baseQp) {
    MbqpMap map;
    map.blockSize = blockSize > 0 ? blockSize : 16;
    map.widthInBlocks = std::max(0, (width + map.blockSize - 1) / map.blockSize);
    map.heightInBlocks = std::max(0, (height + map.blockSize - 1) / map.blockSize);
    map.qp.assign(static_cast<size_t>(map.widthInBlocks) *
                      static_cast<size_t>(map.heightInBlocks),
                  baseQp);
    return map;
}

} // namespace mfx50rt
