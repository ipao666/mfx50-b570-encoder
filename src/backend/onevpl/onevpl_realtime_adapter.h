#pragma once

#include "mfx50rt.h"
#include "src/backend/onevpl/onevpl_realtime_internal.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mfx50rt::onevpl {

struct RealtimeBackendImpl;

struct RealtimeOutputPacket {
    uint32_t stream_id = 0;
    std::vector<uint8_t> data;
    int64_t pts = 0;
    int64_t dts = 0;
    uint32_t flags = 0;
    int32_t frame_type = 0;
    void* user_opaque = nullptr;
};

class RealtimeBackend {
public:
    static std::unique_ptr<RealtimeBackend> create(const MFX50RT_Config& config,
                                                   std::string* error);
    ~RealtimeBackend();

    RealtimeBackend(const RealtimeBackend&) = delete;
    RealtimeBackend& operator=(const RealtimeBackend&) = delete;

    MFX50RT_Status pushPacket(const MFX50RT_InputPacket& packet, std::string* error);
    MFX50RT_Status flush(std::string* error);
    MFX50RT_Status pollOne(RealtimeOutputPacket* out, std::string* error);
    MFX50RT_Status pollAll(std::vector<RealtimeOutputPacket>* out, std::string* error);
    MFX50RT_Status setFrameDecisionCallback(MFX50RT_InternalFrameDecisionCallback cb,
                                            void* opaque,
                                            std::string* error);
    MFX50RT_Status setEncodeControlEventCallback(MFX50RT_InternalEncodeControlEventCallback cb,
                                                 void* opaque,
                                                 std::string* error);

private:
    explicit RealtimeBackend(std::unique_ptr<RealtimeBackendImpl> impl);

    std::unique_ptr<RealtimeBackendImpl> impl_;
};

} // namespace mfx50rt::onevpl
