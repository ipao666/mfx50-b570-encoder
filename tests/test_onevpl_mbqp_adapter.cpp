#include "src/backend/onevpl/onevpl_mbqp_adapter.h"

#include <cassert>
#include <vector>

using namespace mfx50rt::onevpl;
using namespace mfx50rt::hybridtsrq;

int main() {
    QpMap16x16 map;
    map.width = 32;
    map.height = 16;
    map.block_cols = 2;
    map.block_rows = 1;
    map.qp = {12, 60};

    std::vector<uint8_t> buffer(2);
    MFX50RT_InternalEncodeDecision decision{};
    decision.size = sizeof(decision);
    decision.version = MFX50RT_INTERNAL_API_VERSION;
    decision.mbqp_qp_buffer = buffer.data();
    decision.mbqp_qp_capacity = static_cast<uint32_t>(buffer.size());

    std::string error;
    assert(fillInternalMbqpDecisionFromMap(map, 33, &decision, &error));
    assert(decision.strategy == MFX50RT_INTERNAL_CONTROL_MBQP);
    assert(decision.has_mbqp == 1);
    assert(decision.mbqp_block_cols == 2);
    assert(decision.mbqp_block_rows == 1);
    assert(decision.mbqp_pitch == 2);
    assert(buffer[0] == 12);
    assert(buffer[1] == 51);
    assert(decision.spatial_min_qp == 12);
    assert(decision.spatial_max_qp == 51);

    std::vector<uint8_t> pattern_buffer(4);
    MFX50RT_InternalEncodeDecision pattern{};
    pattern.size = sizeof(pattern);
    pattern.version = MFX50RT_INTERNAL_API_VERSION;
    pattern.mbqp_qp_buffer = pattern_buffer.data();
    pattern.mbqp_qp_capacity = static_cast<uint32_t>(pattern_buffer.size());
    assert(fillInternalForceMbqpPattern("checkerboard", 32, 32, 33, &pattern, &error));
    assert(pattern.mbqp_block_cols == 2);
    assert(pattern.mbqp_block_rows == 2);
    assert(pattern_buffer[0] == 24);
    assert(pattern_buffer[1] == 44);
    assert(pattern_buffer[2] == 44);
    assert(pattern_buffer[3] == 24);
    return 0;
}
