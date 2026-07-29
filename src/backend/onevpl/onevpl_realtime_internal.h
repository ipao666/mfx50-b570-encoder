#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MFX50RT_INTERNAL_API_VERSION 1u

typedef enum MFX50RT_InternalControlStrategy {
    MFX50RT_INTERNAL_CONTROL_GLOBAL = 0,
    MFX50RT_INTERNAL_CONTROL_MBQP = 1,
    MFX50RT_INTERNAL_CONTROL_ROI = 2
} MFX50RT_InternalControlStrategy;

typedef struct MFX50RT_InternalSurfaceView {
    uint32_t size;
    uint32_t version;
    uint32_t stream_id;
    uint64_t frame_id;
    int64_t pts;
    int64_t dts;
    int32_t width;
    int32_t height;
    int32_t fourcc;
    const uint8_t* y_ptr;
    int32_t y_pitch;
    uint8_t reserved[128];
} MFX50RT_InternalSurfaceView;

typedef struct MFX50RT_InternalRoiRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
    int32_t delta_qp;
} MFX50RT_InternalRoiRect;

typedef struct MFX50RT_InternalEncodeDecision {
    uint32_t size;
    uint32_t version;
    MFX50RT_InternalControlStrategy strategy;
    int32_t force_idr;
    int32_t qpi;
    int32_t qpp;
    int32_t qpb;
    int32_t base_scene_qp;
    int32_t frame_anchor_qp;
    int32_t has_mbqp;
    int32_t mbqp_block_size;
    int32_t mbqp_pitch;
    int32_t mbqp_block_cols;
    int32_t mbqp_block_rows;
    uint32_t mbqp_num_qp_alloc;
    uint8_t* mbqp_qp_buffer;
    uint32_t mbqp_qp_capacity;
    int32_t has_roi;
    int32_t roi_count;
    MFX50RT_InternalRoiRect roi[256];
    int32_t spatial_min_qp;
    int32_t spatial_max_qp;
    int32_t spatial_avg_qp;
    float foreground_ratio;
    float edge_density;
    float motion_score;
    float noise_score;
    float scene_cut_score;
    int32_t quality_guard_state;
    int32_t static_reuse_candidate;
    int32_t static_reuse_consecutive_frames;
    float static_reuse_risk_score;
    char reason[512];
    uint8_t reserved[244];
} MFX50RT_InternalEncodeDecision;

typedef struct MFX50RT_InternalEncodeControlEvent {
    uint32_t size;
    uint32_t version;
    uint32_t stream_id;
    uint64_t frame_id;
    int64_t pts;
    MFX50RT_InternalControlStrategy requested_strategy;
    MFX50RT_InternalControlStrategy actual_strategy;
    int32_t mbqp_init_enabled;
    int32_t mbqp_requested;
    int32_t mbqp_attached;
    int32_t mbqp_block_size;
    int32_t mbqp_pitch;
    int32_t mbqp_block_cols;
    int32_t mbqp_block_rows;
    uint32_t mbqp_num_qp_alloc;
    int32_t qp_min;
    int32_t qp_max;
    int32_t qp_avg;
    int32_t fallback;
    char reason[512];
    uint8_t reserved[256];
} MFX50RT_InternalEncodeControlEvent;

typedef int (*MFX50RT_InternalFrameDecisionCallback)(
    void* opaque,
    const MFX50RT_InternalSurfaceView* surface,
    MFX50RT_InternalEncodeDecision* decision);

typedef void (*MFX50RT_InternalEncodeControlEventCallback)(
    void* opaque,
    const MFX50RT_InternalEncodeControlEvent* event);

int mfx50_realtime_set_frame_decision_callback(
    void* old_realtime_handle,
    MFX50RT_InternalFrameDecisionCallback cb,
    void* opaque);

int mfx50_realtime_set_encode_control_event_callback(
    void* old_realtime_handle,
    MFX50RT_InternalEncodeControlEventCallback cb,
    void* opaque);

#ifdef __cplusplus
}
#endif
