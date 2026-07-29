#include "mfx50_output_queue.h"

#include <cstring>

namespace mfx50rt {

void OutputQueue::push(const OutputPacket& pkt) {
    std::lock_guard<std::mutex> lock(mu_);
    packets_.push_back(pkt);
}

int OutputQueue::poll(MFX50RT_EncodedPacket* outPacket) {
    if (!outPacket) {
        return MFX50_ERR_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(mu_);
    if (packets_.empty()) {
        return MFX50_ERR_NO_OUTPUT;
    }

    const OutputPacket& pkt = packets_.front();
    if (!outPacket->data || outPacket->capacity < pkt.data.size()) {
        outPacket->size = pkt.data.size();
        return MFX50_ERR_BUFFER_TOO_SMALL;
    }

    std::memcpy(outPacket->data, pkt.data.data(), pkt.data.size());
    outPacket->struct_size = sizeof(*outPacket);
    outPacket->stream_id = pkt.streamId;
    outPacket->size = pkt.data.size();
    outPacket->pts = pkt.pts;
    outPacket->dts = pkt.dts;
    outPacket->is_keyframe = pkt.isKeyframe;
    outPacket->frame_type = pkt.frameType;
    outPacket->user_opaque = pkt.userOpaque;
    packets_.pop_front();
    return MFX50_OK;
}

std::deque<OutputPacket> OutputQueue::drainAll() {
    std::lock_guard<std::mutex> lock(mu_);
    std::deque<OutputPacket> out;
    out.swap(packets_);
    return out;
}

size_t OutputQueue::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return packets_.size();
}

bool OutputQueue::empty() const {
    std::lock_guard<std::mutex> lock(mu_);
    return packets_.empty();
}

} // namespace mfx50rt
