#pragma once

#include "src/algo/hybridtsrq/hybridtsrq_types.h"
#include "src/backend/onevpl/onevpl_realtime_internal.h"

#include <string>

namespace mfx50rt::onevpl {

struct MbqpFrameSummary {
    bool valid = false;
    int qp_min = 0;
    int qp_max = 0;
    int qp_avg = 0;
    int block_count = 0;
};

MbqpFrameSummary summarizeMbqpMap(const mfx50rt::hybridtsrq::QpMap16x16& map);
bool fillInternalMbqpDecisionFromMap(const mfx50rt::hybridtsrq::QpMap16x16& map,
                                     int frame_anchor_qp,
                                     MFX50RT_InternalEncodeDecision* out,
                                     std::string* error);
bool fillInternalForceMbqpPattern(const char* pattern,
                                  int width,
                                  int height,
                                  int frame_anchor_qp,
                                  MFX50RT_InternalEncodeDecision* out,
                                  std::string* error);

} // namespace mfx50rt::onevpl
