#include "mfx50rt.h"

#include <array>
#include <cstdio>

int main(void) {
    MFX50RT_Config cfg{};
    MFX50RT_DefaultConfig(&cfg);
    cfg.backend.type = MFX50RT_BACKEND_NULL;
    cfg.runtime.async_mode = 0;
    cfg.algo.strategy = MFX50RT_STRATEGY_GLOBAL;

    MFX50RT_Handle h = nullptr;
    if (MFX50RT_Create(&cfg, &h) != MFX50RT_OK) return 1;

    std::array<unsigned char, 2048> bytes{};
    for (int i = 0; i < 3; ++i) {
        MFX50RT_InputPacket in{};
        in.size = sizeof(in);
        in.version = MFX50RT_API_VERSION;
        in.stream_id = 0;
        in.data = bytes.data();
        in.data_size = static_cast<uint32_t>(bytes.size());
        in.pts = i * 40;
        MFX50RT_PushPacket(h, &in);
        MFX50RT_OutputPacket out{};
        if (MFX50RT_PollPacket(h, &out, 10) == MFX50RT_OK) {
            MFX50RT_ReleasePacket(h, &out);
        }
    }

    uint32_t count = 8;
    MFX50RT_DecisionTrace traces[8]{};
    for (auto& t : traces) {
        t.size = sizeof(t);
        t.version = MFX50RT_API_VERSION;
    }
    if (MFX50RT_GetDecisionTrace(h, 0, traces, &count) == MFX50RT_OK) {
        for (uint32_t i = 0; i < count; ++i) {
            std::printf("frame=%lld anchor_qp=%d strategy=%d reason=%s\n",
                        static_cast<long long>(traces[i].frame_id),
                        traces[i].frame_anchor_qp,
                        traces[i].effective_strategy,
                        traces[i].reason);
        }
    }
    MFX50RT_Close(h);
    return 0;
}
