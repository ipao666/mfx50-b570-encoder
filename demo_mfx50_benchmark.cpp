#include "mfx50_transcoder.h"

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <input_list.txt> <output_dir> [routes] [qp] [gop]\n", argv[0]);
        return 2;
    }

    MFX50_Config cfg{};
    MFX50_DefaultConfig(&cfg);
    if (argc >= 4) cfg.route_count = std::atoi(argv[3]);
    if (argc >= 5) cfg.initial_qp = std::atoi(argv[4]);
    if (argc >= 6) cfg.initial_gop = std::atoi(argv[5]);

    if (cfg.route_count == 50) {
        cfg.device_count = 2;
        cfg.devices[0].device_path = "/dev/dri/renderD129";
        cfg.devices[0].route_count = 44;
        cfg.devices[1].device_path = "/dev/dri/renderD128";
        cfg.devices[1].route_count = 6;
    } else {
        cfg.device_count = 1;
        cfg.devices[0].device_path = "/dev/dri/renderD129";
        cfg.devices[0].route_count = cfg.route_count;
    }

    MFX50_Handle h = MFX50_Create(&cfg);
    if (!h) {
        std::fprintf(stderr, "create failed: %s\n", MFX50_GetLastError(nullptr));
        return 1;
    }

    int rc = MFX50_RunInputList(h, argv[1], argv[2]);
    MFX50_Stats st{};
    MFX50_GetStats(h, &st);
    std::printf("rc=%d\n", rc);
    std::printf("real_routes=%d/%d all_routes_realtime=%d target_fps=%.3f\n",
                st.completed_routes, st.requested_routes, st.all_routes_realtime, st.target_fps);
    std::printf("min_route_fps=%.3f avg_route_fps=%.3f max_route_fps=%.3f below=%d\n",
                st.min_route_fps, st.avg_route_fps, st.max_route_fps, st.routes_below_target_fps);
    std::printf("summary=%s\n", st.summary_path);
    if (rc < 0) std::fprintf(stderr, "error: %s\n", MFX50_GetLastError(h));
    MFX50_Close(h);
    return rc < 0 ? 1 : 0;
}
