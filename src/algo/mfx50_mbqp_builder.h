#pragma once

#include <stdint.h>
#include <vector>

namespace mfx50rt {

struct MbqpMap {
    int blockSize = 16;
    int widthInBlocks = 0;
    int heightInBlocks = 0;
    std::vector<uint8_t> qp;
};

MbqpMap buildNeutralMbqpMap(int width, int height, int blockSize, uint8_t baseQp);

} // namespace mfx50rt
