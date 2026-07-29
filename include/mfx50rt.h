#ifndef MFX50RT_H
#define MFX50RT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(MFX50RT_DLL_BUILD)
#    define MFX50RT_API __declspec(dllexport)
#  else
#    define MFX50RT_API __declspec(dllimport)
#  endif
#else
#  define MFX50RT_API
#endif

#define MFX50RT_API_VERSION 1u
#define MFX50RT_STREAM_ALL 0xffffffffu

typedef void* MFX50RT_Handle;

typedef enum MFX50RT_Status {
    MFX50RT_OK = 0,
    MFX50RT_ERR_UNKNOWN = -1,
    MFX50RT_ERR_INVALID_ARG = -2,
    MFX50RT_ERR_NOT_READY = -3,
    MFX50RT_ERR_AGAIN = -4,
    MFX50RT_ERR_BUFFER_TOO_SMALL = -5,
    MFX50RT_ERR_UNSUPPORTED = -6,
    MFX50RT_ERR_DEVICE = -7,
    MFX50RT_ERR_EOS = -8
} MFX50RT_Status;

typedef enum MFX50RT_BackendType {
    MFX50RT_BACKEND_AUTO = 0,
    MFX50RT_BACKEND_ONEVPL = 1,
    MFX50RT_BACKEND_FFMPEG = 2,
    MFX50RT_BACKEND_VAAPI = 3,
    MFX50RT_BACKEND_NVENC = 4,
    MFX50RT_BACKEND_CPU = 5,
    MFX50RT_BACKEND_NULL = 99
} MFX50RT_BackendType;

typedef enum MFX50RT_Codec {
    MFX50RT_CODEC_AUTO = 0,
    MFX50RT_CODEC_H264 = 1,
    MFX50RT_CODEC_HEVC = 2,
    MFX50RT_CODEC_AV1 = 3
} MFX50RT_Codec;

typedef enum MFX50RT_RateControl {
    MFX50RT_RC_AUTO = 0,
    MFX50RT_RC_CQP = 1,
    MFX50RT_RC_QVBR = 2,
    MFX50RT_RC_ICQ = 3,
    MFX50RT_RC_VBR = 4,
    MFX50RT_RC_CBR = 5
} MFX50RT_RateControl;

typedef enum MFX50RT_AlgoProfile {
    MFX50RT_PROFILE_SAFE = 0,
    MFX50RT_PROFILE_BALANCED = 1,
    MFX50RT_PROFILE_TARGET_90_SSIM_GUARD = 2,
    MFX50RT_PROFILE_MAX_COMPRESSION = 3,
    MFX50RT_PROFILE_LOW_LATENCY = 4,
    MFX50RT_PROFILE_CUSTOM = 100
} MFX50RT_AlgoProfile;

typedef enum MFX50RT_ControlStrategy {
    MFX50RT_STRATEGY_AUTO = 0,
    MFX50RT_STRATEGY_MBQP_CQP = 1,
    MFX50RT_STRATEGY_ROI_DELTA_QP = 2,
    MFX50RT_STRATEGY_GLOBAL = 3
} MFX50RT_ControlStrategy;

typedef enum MFX50RT_PreprocessMode {
    MFX50RT_PREPROCESS_OFF = 0,
    MFX50RT_PREPROCESS_AUTO = 1,
    MFX50RT_PREPROCESS_LIGHT = 2,
    MFX50RT_PREPROCESS_STRONG = 3
} MFX50RT_PreprocessMode;

typedef enum MFX50RT_PacketFlags {
    MFX50RT_PACKET_FLAG_KEYFRAME = 1u << 0,
    MFX50RT_PACKET_FLAG_EOS = 1u << 1,
    MFX50RT_PACKET_FLAG_CONFIG = 1u << 2,
    MFX50RT_PACKET_FLAG_DISCONT = 1u << 3
} MFX50RT_PacketFlags;

typedef enum MFX50RT_EventType {
    MFX50RT_EVENT_INFO = 0,
    MFX50RT_EVENT_WARNING = 1,
    MFX50RT_EVENT_ERROR = 2,
    MFX50RT_EVENT_ROUTE_STARTED = 100,
    MFX50RT_EVENT_ROUTE_STOPPED = 101,
    MFX50RT_EVENT_FALLBACK = 200,
    MFX50RT_EVENT_QUALITY_GUARD_ON = 201,
    MFX50RT_EVENT_QUALITY_GUARD_OFF = 202,
    MFX50RT_EVENT_OVERLOAD = 203,
    MFX50RT_EVENT_SCENE_CUT = 300,
    MFX50RT_EVENT_FORCE_IDR = 301
} MFX50RT_EventType;

typedef struct MFX50RT_BackendConfig {
    uint32_t size;
    uint32_t version;
    MFX50RT_BackendType type;
    int32_t device_index;
    char device_name[128];
    char library_path[512];
    int32_t prefer_hw_decode;
    int32_t prefer_hw_encode;
    int32_t prefer_video_memory;
    int32_t prefer_zero_copy;
    int32_t async_depth;
    int32_t low_latency;
    char backend_options_json[2048];
    uint8_t reserved[128];
} MFX50RT_BackendConfig;

typedef struct MFX50RT_PipelineConfig {
    uint32_t size;
    uint32_t version;
    MFX50RT_Codec input_codec;
    MFX50RT_Codec output_codec;
    int32_t width;
    int32_t height;
    int32_t fps_num;
    int32_t fps_den;
    int32_t gop_size;
    int32_t idr_interval;
    int32_t b_frames;
    MFX50RT_RateControl rc_mode;
    int32_t target_bitrate_kbps;
    int32_t max_bitrate_kbps;
    int32_t low_latency;
    int32_t annexb_output;
    uint8_t reserved[128];
} MFX50RT_PipelineConfig;

typedef struct MFX50RT_AlgoPolicy {
    uint32_t size;
    uint32_t version;
    MFX50RT_AlgoProfile profile;
    MFX50RT_ControlStrategy strategy;
    int32_t target_compression_percent;
    float avg_ssim_target;
    float min_ssim_guard;
    int32_t enable_temporal_qp;
    int32_t enable_adaptive_gop;
    int32_t enable_scene_cut_idr;
    int32_t enable_b_frame_policy;
    int32_t enable_spatial_qp;
    int32_t enable_roi;
    int32_t enable_mbqp;
    int32_t enable_roi_delta_qp;
    int32_t enable_fast_analyzer;
    int32_t enable_heavy_analyzer;
    int32_t heavy_analyze_interval;
    int32_t enable_preprocess;
    MFX50RT_PreprocessMode preprocess_mode;
    int32_t enable_quality_guard;
    int32_t quality_guard_hold_frames;
    char expert_options_json[4096];
    uint8_t reserved[256];
} MFX50RT_AlgoPolicy;

typedef struct MFX50RT_RuntimeConfig {
    uint32_t size;
    uint32_t version;
    int32_t route_count;
    int32_t worker_threads;
    int32_t queue_depth_per_route;
    int32_t async_mode;
    int32_t enable_callback;
    int32_t log_level;
    uint8_t reserved[128];
} MFX50RT_RuntimeConfig;

typedef struct MFX50RT_Config {
    uint32_t size;
    uint32_t version;
    MFX50RT_BackendConfig backend;
    MFX50RT_PipelineConfig pipeline;
    MFX50RT_AlgoPolicy algo;
    MFX50RT_RuntimeConfig runtime;
    void* user_opaque;
    uint8_t reserved[256];
} MFX50RT_Config;

typedef struct MFX50RT_InputPacket {
    uint32_t size;
    uint32_t version;
    uint32_t stream_id;
    const uint8_t* data;
    uint32_t data_size;
    int64_t pts;
    int64_t dts;
    uint32_t flags;
    void* user_opaque;
    uint8_t reserved[64];
} MFX50RT_InputPacket;

typedef struct MFX50RT_OutputPacket {
    uint32_t size;
    uint32_t version;
    uint32_t stream_id;
    const uint8_t* data;
    uint32_t data_size;
    int64_t pts;
    int64_t dts;
    uint32_t flags;
    int32_t codec;
    int32_t frame_type;
    int32_t qp_avg;
    int32_t qp_min;
    int32_t qp_max;
    void* packet_handle;
    void* user_opaque;
    uint8_t reserved[128];
} MFX50RT_OutputPacket;

typedef struct MFX50RT_Capabilities {
    uint32_t size;
    uint32_t version;
    int32_t supports_hw_decode;
    int32_t supports_hw_encode;
    int32_t supports_video_memory;
    int32_t supports_zero_copy;
    int32_t supports_hevc_decode;
    int32_t supports_hevc_encode;
    int32_t supports_h264_decode;
    int32_t supports_h264_encode;
    int32_t supports_av1_decode;
    int32_t supports_av1_encode;
    int32_t supports_cqp;
    int32_t supports_qvbr;
    int32_t supports_icq;
    int32_t supports_vbr;
    int32_t supports_cbr;
    int32_t supports_ipb_qp;
    int32_t supports_force_idr;
    int32_t supports_b_frames;
    int32_t supports_roi_delta_qp;
    int32_t supports_mbqp;
    int32_t supports_ctu_qp_map;
    int32_t max_roi_regions;
    int32_t max_width;
    int32_t max_height;
    int32_t max_async_depth;
    char backend_name[128];
    char device_name[128];
    char driver_desc[256];
    uint8_t reserved[256];
} MFX50RT_Capabilities;

typedef struct MFX50RT_EffectiveConfig {
    uint32_t size;
    uint32_t version;
    MFX50RT_BackendType requested_backend;
    MFX50RT_BackendType effective_backend;
    MFX50RT_ControlStrategy requested_strategy;
    MFX50RT_ControlStrategy effective_strategy;
    MFX50RT_RateControl requested_rc_mode;
    MFX50RT_RateControl effective_rc_mode;
    int32_t temporal_qp_enabled;
    int32_t spatial_qp_enabled;
    int32_t mbqp_enabled;
    int32_t roi_delta_qp_enabled;
    int32_t preprocess_enabled;
    int32_t quality_guard_enabled;
    char fallback_reason[512];
    char effective_options_json[4096];
    uint8_t reserved[256];
} MFX50RT_EffectiveConfig;

typedef struct MFX50RT_QualityMetric {
    uint32_t size;
    uint32_t version;
    uint32_t stream_id;
    int64_t frame_id;
    int64_t pts;
    float ssim;
    float psnr;
    float vmaf;
    float compression_ratio;
    uint32_t input_bytes;
    uint32_t output_bytes;
    uint8_t reserved[128];
} MFX50RT_QualityMetric;

typedef struct MFX50RT_RouteStats {
    uint32_t size;
    uint32_t version;
    uint32_t stream_id;
    uint64_t input_packets;
    uint64_t input_bytes;
    uint64_t output_packets;
    uint64_t output_bytes;
    uint64_t decoded_frames;
    uint64_t encoded_frames;
    uint64_t dropped_frames;
    double fps_in;
    double fps_out;
    double compression_ratio_avg;
    double latency_ms_avg;
    double latency_ms_p50;
    double latency_ms_p95;
    double latency_ms_p99;
    double decode_ms_avg;
    double encode_ms_avg;
    double fast_analyze_ms_avg;
    double heavy_analyze_ms_avg;
    double preprocess_ms_avg;
    uint64_t scene_cut_count;
    uint64_t force_idr_count;
    uint64_t mbqp_frames;
    uint64_t roi_frames;
    uint64_t global_frames;
    uint64_t fallback_count;
    int32_t mbqp_init_enabled;
    uint64_t mbqp_applied_frames;
    uint64_t mbqp_skipped_frames;
    uint64_t mbqp_fallback_count;
    int32_t qp_avg;
    int32_t qp_min;
    int32_t qp_max;
    int32_t qp_p10;
    int32_t qp_p50;
    int32_t qp_p90;
    int32_t qp_p95;
    double low_importance_block_ratio;
    double high_qp_block_ratio;
    double roi_block_ratio;
    double background_block_ratio;
    double flat_background_block_ratio;
    double foreground_block_ratio;
    double edge_block_ratio;
    double texture_block_ratio;
    double transition_block_ratio;
    double smoothing_changed_qp_avg;
    double avg_ssim;
    double min_ssim;
    double p5_ssim;
    uint32_t queue_depth_input;
    uint32_t queue_depth_output;
    char effective_strategy[64];
    char actual_encode_control[64];
    char last_warning[512];
    double true_roi_block_ratio;
    double edge_texture_roi_block_ratio;
    double high_texture_background_block_ratio;
    double hard_scene_background_block_ratio;
    double normal_background_block_ratio;
    uint8_t reserved[120];
} MFX50RT_RouteStats;

typedef struct MFX50RT_GlobalStats {
    uint32_t size;
    uint32_t version;
    uint32_t route_count;
    uint32_t active_routes;
    double total_fps_in;
    double total_fps_out;
    uint64_t total_input_bytes;
    uint64_t total_output_bytes;
    double compression_ratio_avg;
    double cpu_usage_percent;
    double gpu_usage_percent;
    double memory_usage_mb;
    uint64_t total_fallback_count;
    uint64_t total_overload_count;
    uint8_t reserved[256];
} MFX50RT_GlobalStats;

typedef struct MFX50RT_DecisionTrace {
    uint32_t size;
    uint32_t version;
    uint32_t stream_id;
    int64_t frame_id;
    int64_t pts;
    int32_t frame_type;
    int32_t base_scene_qp;
    int32_t qpi;
    int32_t qpp;
    int32_t qpb;
    int32_t frame_anchor_qp;
    int32_t spatial_min_qp;
    int32_t spatial_max_qp;
    int32_t spatial_avg_qp;
    int32_t qp_p10;
    int32_t qp_p50;
    int32_t qp_p90;
    int32_t qp_p95;
    int32_t roi_count;
    float foreground_ratio;
    float edge_density;
    float motion_score;
    float noise_score;
    float scene_cut_score;
    float low_importance_block_ratio;
    float high_qp_block_ratio;
    float roi_block_ratio;
    float background_block_ratio;
    float flat_background_block_ratio;
    float foreground_block_ratio;
    float edge_block_ratio;
    float texture_block_ratio;
    float transition_block_ratio;
    float smoothing_changed_qp_avg;
    int32_t quality_guard_state;
    int32_t effective_strategy;
    int32_t mbqp_requested;
    int32_t mbqp_attached;
    int32_t mbqp_block_cols;
    int32_t mbqp_block_rows;
    int32_t mbqp_pitch;
    uint32_t mbqp_num_qp_alloc;
    char actual_encode_control[64];
    char encode_ctrl_reason[512];
    char reason[512];
    char detail_json[4096];
    float true_roi_block_ratio;
    float edge_texture_roi_block_ratio;
    float high_texture_background_block_ratio;
    float hard_scene_background_block_ratio;
    float normal_background_block_ratio;
    int32_t hard_scene_like;
    int32_t hard_guard_active;
    int32_t class_qp_table_overwrite_count;
    char scene_mode[32];
    char class_qp_table_name[64];
    int32_t static_reuse_candidate;
    int32_t static_reuse_consecutive_frames;
    float static_reuse_risk_score;
    uint8_t reserved[60];
} MFX50RT_DecisionTrace;

typedef struct MFX50RT_Event {
    uint32_t size;
    uint32_t version;
    MFX50RT_EventType type;
    uint32_t stream_id;
    int64_t timestamp_ms;
    int32_t code;
    char message[512];
    char detail_json[2048];
    uint8_t reserved[128];
} MFX50RT_Event;

typedef void (*MFX50RT_OutputCallback)(
    const MFX50RT_OutputPacket* packet,
    void* user_opaque);

typedef void (*MFX50RT_EventCallback)(
    const MFX50RT_Event* event,
    void* user_opaque);

MFX50RT_API const char* MFX50RT_GetVersion(void);
MFX50RT_API const char* MFX50RT_StatusString(MFX50RT_Status status);
MFX50RT_API MFX50RT_Status MFX50RT_DefaultConfig(MFX50RT_Config* config);
MFX50RT_API MFX50RT_Status MFX50RT_Create(
    const MFX50RT_Config* config,
    MFX50RT_Handle* handle);
MFX50RT_API MFX50RT_Status MFX50RT_CreateFromJson(
    const char* json_path_or_text,
    int32_t is_path,
    MFX50RT_Handle* handle);
MFX50RT_API MFX50RT_Status MFX50RT_QueryCapabilities(
    const MFX50RT_BackendConfig* backend,
    MFX50RT_Capabilities* caps);
MFX50RT_API MFX50RT_Status MFX50RT_GetEffectiveConfig(
    MFX50RT_Handle handle,
    MFX50RT_EffectiveConfig* out_config);
MFX50RT_API MFX50RT_Status MFX50RT_PushPacket(
    MFX50RT_Handle handle,
    const MFX50RT_InputPacket* packet);
MFX50RT_API MFX50RT_Status MFX50RT_PollPacket(
    MFX50RT_Handle handle,
    MFX50RT_OutputPacket* packet,
    int32_t timeout_ms);
MFX50RT_API MFX50RT_Status MFX50RT_ReleasePacket(
    MFX50RT_Handle handle,
    MFX50RT_OutputPacket* packet);
MFX50RT_API MFX50RT_Status MFX50RT_Flush(
    MFX50RT_Handle handle,
    uint32_t stream_id);
MFX50RT_API MFX50RT_Status MFX50RT_Close(MFX50RT_Handle handle);
MFX50RT_API MFX50RT_Status MFX50RT_UpdateQualityMetric(
    MFX50RT_Handle handle,
    const MFX50RT_QualityMetric* metric);
MFX50RT_API MFX50RT_Status MFX50RT_GetRouteStats(
    MFX50RT_Handle handle,
    uint32_t stream_id,
    MFX50RT_RouteStats* stats);
MFX50RT_API MFX50RT_Status MFX50RT_GetGlobalStats(
    MFX50RT_Handle handle,
    MFX50RT_GlobalStats* stats);
MFX50RT_API MFX50RT_Status MFX50RT_GetDecisionTrace(
    MFX50RT_Handle handle,
    uint32_t stream_id,
    MFX50RT_DecisionTrace* traces,
    uint32_t* inout_count);
MFX50RT_API MFX50RT_Status MFX50RT_SetOutputCallback(
    MFX50RT_Handle handle,
    MFX50RT_OutputCallback cb,
    void* user_opaque);
MFX50RT_API MFX50RT_Status MFX50RT_SetEventCallback(
    MFX50RT_Handle handle,
    MFX50RT_EventCallback cb,
    void* user_opaque);

#ifdef __cplusplus
}
#endif

#endif /* MFX50RT_H */
