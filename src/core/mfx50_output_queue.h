#pragma once

#include "mfx50_realtime.h"

#include <deque>
#include <mutex>
#include <stdint.h>
#include <vector>

namespace mfx50rt {

struct OutputPacket {
    int streamId = 0;
    std::vector<uint8_t> data;
    int64_t pts = 0;
    int64_t dts = 0;
    int isKeyframe = 0;
    int frameType = 0;
    void* userOpaque = nullptr;
};

class OutputQueue {
public:
    void push(const OutputPacket& pkt);
    int poll(MFX50RT_EncodedPacket* outPacket);
    std::deque<OutputPacket> drainAll();
    size_t size() const;
    bool empty() const;

private:
    mutable std::mutex mu_;
    std::deque<OutputPacket> packets_;
};

} // namespace mfx50rt
