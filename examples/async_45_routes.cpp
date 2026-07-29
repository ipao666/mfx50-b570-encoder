#include "mfx50rt.h"

#include <array>
#include <cstdio>
#include <vector>

int main(void) {
    MFX50RT_Config cfg{};
    MFX50RT_DefaultConfig(&cfg);
    cfg.backend.type = MFX50RT_BACKEND_NULL;
    cfg.runtime.route_count = 45;
    cfg.runtime.worker_threads = 45;
    cfg.runtime.async_mode = 1;
    cfg.runtime.queue_depth_per_route = 16;
    cfg.algo.strategy = MFX50RT_STRATEGY_GLOBAL;

    MFX50RT_Handle h = nullptr;
    if (MFX50RT_Create(&cfg, &h) != MFX50RT_OK) return 1;
    std::array<unsigned char, 4096> bytes{};

    for (uint32_t route = 0; route < 45; ++route) {
        MFX50RT_InputPacket in{};
        in.size = sizeof(in);
        in.version = MFX50RT_API_VERSION;
        in.stream_id = route;
        in.data = bytes.data();
        in.data_size = static_cast<uint32_t>(bytes.size());
        in.pts = route;
        MFX50RT_Status st = MFX50RT_PushPacket(h, &in);
        if (st != MFX50RT_OK) {
            std::printf("push route %u failed: %s\n", route, MFX50RT_StatusString(st));
            MFX50RT_Close(h);
            return 2;
        }
    }

    int outputs = 0;
    while (outputs < 45) {
        MFX50RT_OutputPacket out{};
        MFX50RT_Status st = MFX50RT_PollPacket(h, &out, 1000);
        if (st != MFX50RT_OK) break;
        outputs++;
        MFX50RT_ReleasePacket(h, &out);
    }
    MFX50RT_GlobalStats stats{};
    stats.size = sizeof(stats);
    stats.version = MFX50RT_API_VERSION;
    MFX50RT_GetGlobalStats(h, &stats);
    std::printf("outputs=%d total_fps_out=%.2f compression=%.3f\n",
                outputs,
                stats.total_fps_out,
                stats.compression_ratio_avg);
    MFX50RT_Close(h);
    return outputs == 45 ? 0 : 3;
}
