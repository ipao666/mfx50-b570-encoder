#pragma once

#include "mfx50_api.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MFX50_MAX_DEVICES 4
#define MFX50_MAX_PATH 512
#define MFX50_VERSION "0.1.0"

typedef struct MFX50_Context* MFX50_Handle;

typedef struct MFX50_DeviceRoute {
    const char* device_path;
    int route_count;
} MFX50_DeviceRoute;

typedef struct MFX50_Config {
    int route_count;
    int frames_per_route;
    int fps_num;
    int fps_den;
    int initial_qp;
    int initial_gop;
    int async_depth;
    int device_count;
    MFX50_DeviceRoute devices[MFX50_MAX_DEVICES];
    int write_outputs;
    const char* sample_path;
    int enable_internal_roi;
    int enable_quality_guard;
    int enable_motion_idr;
} MFX50_Config;

typedef struct MFX50_RouteStats {
    int route_id;
    int passed;
    int frames;
    double seconds;
    double fps;
    char device_path[MFX50_MAX_PATH];
} MFX50_RouteStats;

typedef struct MFX50_Stats {
    int requested_routes;
    int completed_routes;
    int routes_below_target_fps;
    int all_routes_realtime;
    int frames_per_route;
    double target_fps;
    double min_route_fps;
    double avg_route_fps;
    double max_route_fps;
    double common_time_sec;
    double wall_seconds;
    double aggregate_fps;
    char summary_path[MFX50_MAX_PATH];
    char par_path[MFX50_MAX_PATH];
    char log_path[MFX50_MAX_PATH];
} MFX50_Stats;

MFX50_API const char* MFX50_GetVersion(void);
MFX50_API int MFX50_DefaultConfig(MFX50_Config* config);
MFX50_API MFX50_Handle MFX50_Create(const MFX50_Config* config);
MFX50_API int MFX50_RunInputList(MFX50_Handle h,
                                 const char* input_list_path,
                                 const char* output_dir);
MFX50_API int MFX50_RunSingleInput(MFX50_Handle h,
                                   const char* input_path,
                                   const char* output_dir);
MFX50_API int MFX50_GetStats(MFX50_Handle h, MFX50_Stats* stats);
MFX50_API int MFX50_GetRouteStats(MFX50_Handle h,
                                  int route_index,
                                  MFX50_RouteStats* stats);
MFX50_API const char* MFX50_GetLastError(MFX50_Handle h);
MFX50_API void MFX50_Close(MFX50_Handle h);

#ifdef __cplusplus
}
#endif
