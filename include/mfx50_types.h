#ifndef MFX50_TYPES_H
#define MFX50_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(MFX50_POLICY_DLL_BUILD)
#    define MFX50_POLICY_API __declspec(dllexport)
#  else
#    define MFX50_POLICY_API __declspec(dllimport)
#  endif
#else
#  define MFX50_POLICY_API
#endif

#define MFX50_POLICY_API_VERSION 1u
#define MFX50_MAX_METADATA_OBJECTS 256
#define MFX50_MAX_ROIS 256

typedef struct MFX50_PolicyContext MFX50_PolicyContext;
typedef struct MFX50_PolicyStream MFX50_PolicyStream;

typedef enum MFX50_Status {
    MFX50_OK = 0,
    MFX50_ERR_INVALID_PARAM = -1,
    MFX50_ERR_UNSUPPORTED = -2,
    MFX50_ERR_NOT_READY = -3,
    MFX50_ERR_NO_MEMORY = -4,
    MFX50_ERR_BAD_STATE = -5,
    MFX50_ERR_VERSION_MISMATCH = -6,
    MFX50_ERR_BUFFER_TOO_SMALL = -7,
    MFX50_ERR_AGAIN = -8,
    MFX50_ERR_DEVICE = -9,
    MFX50_ERR_EOS = -10,
    MFX50_ERR_NOT_IMPLEMENTED = -11
} MFX50_Status;

typedef enum MFX50_Mode {
    MFX50_MODE_QUALITY = 0,
    MFX50_MODE_COMPRESS_85 = 1,
    MFX50_MODE_TARGET_90 = 2,
    MFX50_MODE_CUSTOM = 3
} MFX50_Mode;

typedef enum MFX50_Codec {
    MFX50_CODEC_UNKNOWN = 0,
    MFX50_CODEC_H264 = 1,
    MFX50_CODEC_H265 = 2,
    MFX50_CODEC_HEVC = MFX50_CODEC_H265,
    MFX50_CODEC_AV1 = 3,
    MFX50_CODEC_RAW = 100
} MFX50_Codec;

typedef enum MFX50_PixelFormat {
    MFX50_PIXFMT_UNKNOWN = 0,
    MFX50_PIXFMT_NV12 = 1,
    MFX50_PIXFMT_YUV420 = 2,
    MFX50_PIXFMT_GRAY = 3,
    MFX50_PIXFMT_P010 = 4,
    MFX50_PIXFMT_RGB24 = 10,
    MFX50_PIXFMT_RGBA = 11
} MFX50_PixelFormat;

typedef enum MFX50_ResetMode {
    MFX50_RESET_SOFT = 0,
    MFX50_RESET_HARD = 1,
    MFX50_RESET_SCENE_CUT = 2
} MFX50_ResetMode;

typedef enum MFX50_MetadataType {
    MFX50_OBJECT_UNKNOWN = 0,
    MFX50_OBJECT_VEHICLE = 1,
    MFX50_OBJECT_PLATE = 2,
    MFX50_OBJECT_SIGN = 3,
    MFX50_OBJECT_TEXT = 4,
    MFX50_OBJECT_ROAD = 5,
    MFX50_OBJECT_BACKGROUND = 6
} MFX50_MetadataType;

typedef enum MFX50_RoiSource {
    MFX50_ROI_SOURCE_INTERNAL = 0,
    MFX50_ROI_SOURCE_METADATA = 1,
    MFX50_ROI_SOURCE_FUSED = 2
} MFX50_RoiSource;

typedef enum MFX50_ProfileId {
    MFX50_PROFILE_BASE_Q36B48 = 1,
    MFX50_PROFILE_DAY_GUARD_Q34 = 2,
    MFX50_PROFILE_RISK_Q35 = 3,
    MFX50_PROFILE_QUALITY_Q32 = 4
} MFX50_ProfileId;

typedef enum MFX50_DecisionFlags {
    MFX50_DECISION_FLAG_QP = 1u << 0,
    MFX50_DECISION_FLAG_B_FRAMES = 1u << 1,
    MFX50_DECISION_FLAG_ROI = 1u << 2,
    MFX50_DECISION_FLAG_DYNAMIC_GOP = 1u << 3,
    MFX50_DECISION_FLAG_IDR = 1u << 4,
    MFX50_DECISION_FLAG_BACKGROUND_QP = 1u << 5,
    MFX50_DECISION_FLAG_DENOISE = 1u << 6,
    MFX50_DECISION_FLAG_FRAME_REUSE = 1u << 7
} MFX50_DecisionFlags;

typedef struct MFX50_Version {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
    const char* build;
} MFX50_Version;

typedef struct MFX50_EncoderCaps {
    uint32_t struct_size;
    int32_t supports_b_frames;
    int32_t supports_roi;
    int32_t supports_mbqp;
    int32_t supports_dynamic_gop;
    int32_t supports_idr_request;
    int32_t supports_frame_reuse;
    int32_t supports_negative_delta_qp;
    int32_t supports_positive_delta_qp;
    int32_t supports_per_frame_qp;
    int32_t supports_roi_delta_qp;
    int32_t supports_roi_absolute_qp;
    int32_t max_roi_count;
    int32_t roi_alignment;
    int32_t mbqp_block_size;
    int32_t min_qp;
    int32_t max_qp;
    int32_t min_delta_qp;
    int32_t max_delta_qp;
    int32_t max_b_frame_dist;
    int32_t max_gop_size;
    uint8_t reserved[256];
} MFX50_EncoderCaps;

typedef struct MFX50_PolicyConfig {
    uint32_t struct_size;
    uint32_t api_version;
    int32_t mode;
    int32_t target_streams;
    float input_fps;
    float target_encode_fps;
    int32_t max_latency_frames;
    MFX50_EncoderCaps encoder_caps;
    char policy_file[256];
    uint8_t reserved[512];
} MFX50_PolicyConfig;

typedef struct MFX50_StreamConfig {
    uint32_t struct_size;
    int32_t stream_id;
    int32_t width;
    int32_t height;
    float fps;
    int32_t input_codec;
    int32_t pixel_format;
    int32_t is_realtime;
    uint8_t reserved[256];
} MFX50_StreamConfig;

typedef struct MFX50_FrameFeatures {
    uint32_t struct_size;
    int32_t frame_index;
    int64_t pts;
    float mean_y;
    float edge_density;
    float motion_score;
    float dark_ratio;
    float overexposed_ratio;
    float road_y_variance;
    float scene_cut_score;
    float recent_bitrate;
    float recent_compression_percent;
    float recent_encode_fps;
    float recent_latency_ms;
    int32_t recent_drop_frames;
    uint8_t reserved[256];
} MFX50_FrameFeatures;

typedef struct MFX50_AnalyzeFrame {
    uint32_t struct_size;
    int32_t width;
    int32_t height;
    int64_t pts;
    int32_t frame_index;
    const uint8_t* y_plane;
    int32_t y_stride;
    const uint8_t* uv_plane;
    int32_t uv_stride;
    int32_t pixel_format;
    int32_t is_lowres;
    uint8_t reserved[256];
} MFX50_AnalyzeFrame;

typedef struct MFX50_MetadataObject {
    int32_t type;
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
    float confidence;
    int32_t track_id;
    int64_t timestamp;
    int32_t priority;
    int32_t suggested_delta_qp;
    uint8_t reserved[64];
} MFX50_MetadataObject;

typedef struct MFX50_Metadata {
    uint32_t struct_size;
    int32_t object_count;
    MFX50_MetadataObject objects[MFX50_MAX_METADATA_OBJECTS];
    uint8_t reserved[256];
} MFX50_Metadata;

typedef struct MFX50_Roi {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
    int32_t delta_qp;
    int32_t type;
    int32_t priority;
    int32_t source;
    float confidence;
    int32_t track_id;
} MFX50_Roi;

typedef struct MFX50_EncodeDecision {
    uint32_t struct_size;
    uint64_t decision_id;
    int32_t profile_id;
    int32_t qpi;
    int32_t qpp;
    int32_t qpb;
    int32_t gop_size;
    int32_t b_frame_dist;
    int32_t request_idr;
    int32_t reuse_previous_frame;
    int32_t background_delta_qp;
    int32_t denoise_strength;
    uint32_t applied_flags;
    uint32_t disabled_flags;
    char reason[128];
    int32_t roi_count;
    MFX50_Roi rois[MFX50_MAX_ROIS];
    uint8_t reserved[1024];
} MFX50_EncodeDecision;

typedef struct MFX50_PolicyStats {
    uint32_t struct_size;
    uint64_t frames_seen;
    uint64_t decisions_made;
    int32_t current_profile_id;
    float avg_roi_count;
    float avg_roi_area_percent;
    uint64_t idr_request_count;
    uint64_t fallback_count;
    uint64_t disabled_flags_count;
    uint64_t metadata_object_count;
    uint8_t reserved[256];
} MFX50_PolicyStats;

typedef void (*MFX50_LogCallback)(int32_t level, const char* message, void* user_data);

#ifdef __cplusplus
}
#endif

#endif
