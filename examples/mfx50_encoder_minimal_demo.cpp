#include "mfx50_transcoder.h"

#include <cstdlib>
#include <cstdio>

static int parse_int_arg(const char* value, int fallback) {
    if (!value || !value[0]) return fallback;
    char* end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (!end || *end != '\0' || parsed <= 0) return fallback;
    return static_cast<int>(parsed);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: %s <input_list.txt> <output_dir> [sample_multi_transcode] [route_count] [frames_per_route]\n",
            argv[0]);
        return 2;
    }

    const char* input_list = argv[1];
    const char* output_dir = argv[2];
    const char* sample_path = argc >= 4 && argv[3][0] ? argv[3] : nullptr;
    const int route_count = argc >= 5 ? parse_int_arg(argv[4], 45) : 45;
    const int frames_per_route = argc >= 6 ? parse_int_arg(argv[5], 1000) : 1000;

    MFX50_Config cfg{};
    if (MFX50_DefaultConfig(&cfg) != 0) {
        std::fprintf(stderr, "MFX50_DefaultConfig failed\n");
        return 1;
    }

    cfg.route_count = route_count;
    cfg.frames_per_route = frames_per_route;
    cfg.fps_num = 30;
    cfg.fps_den = 1;
    cfg.initial_qp = 32;
    cfg.initial_gop = 60;
    cfg.async_depth = 2;

    cfg.device_count = 1;
    cfg.devices[0].device_path = "/dev/dri/renderD129";
    cfg.devices[0].route_count = route_count;

    cfg.write_outputs = 1;
    cfg.sample_path = sample_path;

    MFX50_Handle handle = MFX50_Create(&cfg);
    if (!handle) {
        std::fprintf(stderr, "MFX50_Create failed: %s\n", MFX50_GetLastError(nullptr));
        return 1;
    }

    int rc = MFX50_RunInputList(handle, input_list, output_dir);

    MFX50_Stats stats{};
    if (MFX50_GetStats(handle, &stats) == 0) {
        std::printf("version=%s\n", MFX50_GetVersion());
        std::printf("completed_routes=%d/%d\n", stats.completed_routes, stats.requested_routes);
        std::printf("target_fps=%.3f min_fps=%.3f avg_fps=%.3f max_fps=%.3f\n",
                    stats.target_fps,
                    stats.min_route_fps,
                    stats.avg_route_fps,
                    stats.max_route_fps);
        std::printf("all_routes_realtime=%d routes_below_target=%d\n",
                    stats.all_routes_realtime,
                    stats.routes_below_target_fps);
        std::printf("par=%s\nlog=%s\nsummary=%s\n",
                    stats.par_path,
                    stats.log_path,
                    stats.summary_path);
    }

    if (rc != 0) {
        std::fprintf(stderr, "MFX50_RunInputList returned %d: %s\n",
                     rc,
                     MFX50_GetLastError(handle));
    }

    MFX50_Close(handle);
    return rc == 0 ? 0 : 1;
}
