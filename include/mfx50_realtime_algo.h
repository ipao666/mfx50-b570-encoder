#pragma once

#include "mfx50_realtime.h"

#define MFX50RT_ALGO_FLAG_PREPROCESS 0x00000001u
#define MFX50RT_ALGO_FLAG_SMOOTH_SCALE 0x00000002u
#define MFX50RT_ALGO_FLAG_PRE_DENOISE 0x00000004u
#define MFX50RT_ALGO_FLAG_SCENE_ANALYZER 0x00000008u
#define MFX50RT_ALGO_FLAG_ADAPTIVE_PROFILE 0x00000010u
#define MFX50RT_ALGO_FLAG_ADAPTIVE_QP 0x00000020u
#define MFX50RT_ALGO_FLAG_MBQP 0x00000040u

typedef enum MFX50RT_MbqpDisabledReason {
    MFX50RT_MBQP_DISABLED_NONE = 0,
    MFX50RT_MBQP_DISABLED_UNSUPPORTED = 1,
    MFX50RT_MBQP_DISABLED_NOT_PROBED = 2,
    MFX50RT_MBQP_DISABLED_RUNTIME_FAILED = 3
} MFX50RT_MbqpDisabledReason;

typedef struct MFX50RT_AlgoConfig {
    uint32_t struct_size;
    uint32_t abi_version;

    int enable_preprocess;
    int enable_smooth_scale;
    int smooth_scale_factor;

    int enable_pre_denoise;
    int pre_denoise_strength;

    int enable_scene_analyzer;
    int enable_adaptive_profile;
    int enable_adaptive_qp;

    int enable_mbqp;
    int mbqp_strength;
    int mbqp_block_size;

    int target_output_ratio_permille;
    int fallback_profile;
    int aggressive_profile;

    int reserved[32];
} MFX50RT_AlgoConfig;

typedef struct MFX50RT_AlgoCaps {
    uint32_t struct_size;

    int supports_preprocess;
    int supports_scene_analyzer;
    int supports_adaptive_qp;
    int supports_mbqp;
    int supports_hevc_mbqp;
    int supports_runtime_qp_ctrl;

    int reserved[32];
} MFX50RT_AlgoCaps;

typedef struct MFX50RT_FrameAnalysis {
    uint32_t struct_size;

    uint64_t frame_index;
    int64_t pts;

    float flat_score;
    float motion_score;
    float noise_score;
    float edge_score;
    float scene_cut_score;
    float hard_score;

    int suggested_profile;
    int suggested_qp_delta;
    int suggested_smooth_scale_factor;
    int suggested_denoise_strength;
    int suggest_force_idr;

    int reserved[32];
} MFX50RT_FrameAnalysis;

MFX50RT_API int MFX50RT_DefaultAlgoConfig(MFX50RT_AlgoConfig* cfg);

MFX50RT_API int MFX50RT_SetAlgoConfig(
    MFX50RT_Handle h,
    const MFX50RT_AlgoConfig* cfg);

MFX50RT_API int MFX50RT_GetAlgoConfig(
    MFX50RT_Handle h,
    MFX50RT_AlgoConfig* cfg);

MFX50RT_API int MFX50RT_GetAlgoCaps(
    MFX50RT_Handle h,
    MFX50RT_AlgoCaps* caps);
