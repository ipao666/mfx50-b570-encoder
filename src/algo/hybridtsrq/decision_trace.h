#pragma once

#include "hybridtsrq_types.h"
#include "mfx50rt.h"

#include <deque>
#include <mutex>

namespace mfx50rt::hybridtsrq {

class DecisionTraceBuffer {
public:
    explicit DecisionTraceBuffer(size_t capacity = 256);

    void push(uint32_t stream_id,
              int64_t pts,
              int frame_type,
              const HybridTSRQDecision& decision,
              const FastFrameFeatures& fast,
              const RoiAnalysisResult& roi,
              const QualityState& quality);

    MFX50RT_Status copyTo(MFX50RT_DecisionTrace* traces, uint32_t* inout_count) const;
    void updateEncodeControl(uint32_t stream_id,
                             int64_t frame_id,
                             int mbqp_attached,
                             const char* actual_encode_control,
                             const char* encode_ctrl_reason);

private:
    size_t capacity_ = 256;
    mutable std::mutex mutex_;
    std::deque<MFX50RT_DecisionTrace> traces_;
};

} // namespace mfx50rt::hybridtsrq
