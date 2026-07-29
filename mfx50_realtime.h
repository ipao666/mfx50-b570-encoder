#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#  define MFX50RT_EXTERN_C extern "C"
#else
#  define MFX50RT_EXTERN_C
#endif

#ifdef _WIN32
#  ifdef MFX50_REALTIME_DLL_BUILD
#    define MFX50RT_API MFX50RT_EXTERN_C __declspec(dllexport)
#  else
#    define MFX50RT_API MFX50RT_EXTERN_C __declspec(dllimport)
#  endif
#else
#  define MFX50RT_API MFX50RT_EXTERN_C
#endif

#define MFX50RT_API_VERSION 1
#define MFX50RT_VERSION "0.5.0-alpha1"

typedef struct MFX50RT_Context* MFX50RT_Handle;

typedef enum MFX50_Codec {
    MFX50_CODEC_H264 = 1,
    MFX50_CODEC_HEVC = 2
} MFX50_Codec;

typedef enum MFX50_PixelFormat {
    MFX50_PIXFMT_UNKNOWN = 0,
    MFX50_PIXFMT_NV12 = 1,
    MFX50_PIXFMT_P010 = 2
} MFX50_PixelFormat;

typedef enum MFX50_Profile {
    MFX50_PROFILE_THROUGHPUT_ONLY = 0,
    MFX50_PROFILE_QUALITY_90_NEAR = 1,
    MFX50_PROFILE_COMPRESS_85_PROBE = 2,
    MFX50_PROFILE_DEBUG_TRACE = 3,
    MFX50_PROFILE_COMPRESS_90_PROBE_A = 4,
    MFX50_PROFILE_COMPRESS_90_PROBE_B = 5,
    MFX50_PROFILE_COMPRESS_90_PROBE_C = 6,
    MFX50_PROFILE_COMPRESS_90_PROBE_D = 7
} MFX50_Profile;

typedef enum MFX50_InputMode {
    MFX50_INPUT_ENCODED_PACKET = 1,
    MFX50_INPUT_RAW_FRAME = 2
} MFX50_InputMode;

typedef enum MFX50_OutputMode {
    MFX50_OUTPUT_POLL = 1,
    MFX50_OUTPUT_CALLBACK = 2
} MFX50_OutputMode;

typedef enum MFX50RT_DropPolicy {
    MFX50RT_DROP_NONE = 0,
    MFX50RT_DROP_OLDEST = 1,
    MFX50RT_DROP_NON_KEY_UNTIL_IDR = 2
} MFX50RT_DropPolicy;

typedef enum MFX50_Status {
    MFX50_OK = 0,
    MFX50_ERR_INVALID_ARG = -1,
    MFX50_ERR_DEVICE = -2,
    MFX50_ERR_DECODE = -3,
    MFX50_ERR_ENCODE = -4,
    MFX50_ERR_NOT_IMPLEMENTED = -5,
    MFX50_ERR_BACKPRESSURE = -44,
    MFX50_ERR_BUFFER_TOO_SMALL = -43,
    MFX50_ERR_NO_OUTPUT = 1,
    MFX50_ERR_NEED_MORE_INPUT = 2,
    MFX50_ERR_AGAIN = 3
} MFX50_Status;

typedef enum MFX50RT_Status {
    MFX50RT_OK = MFX50_OK,
    MFX50RT_E_INVALID_ARG = MFX50_ERR_INVALID_ARG,
    MFX50RT_E_DEVICE = MFX50_ERR_DEVICE,
    MFX50RT_E_DECODE = MFX50_ERR_DECODE,
    MFX50RT_E_ENCODE = MFX50_ERR_ENCODE,
    MFX50RT_E_NOT_IMPLEMENTED = MFX50_ERR_NOT_IMPLEMENTED,
    MFX50RT_E_BACKPRESSURE = MFX50_ERR_BACKPRESSURE,
    MFX50RT_E_BUFFER_TOO_SMALL = MFX50_ERR_BUFFER_TOO_SMALL,
    MFX50RT_E_NO_OUTPUT = MFX50_ERR_NO_OUTPUT,
    MFX50RT_E_NEED_MORE_INPUT = MFX50_ERR_NEED_MORE_INPUT,
    MFX50RT_E_AGAIN = MFX50_ERR_AGAIN
} MFX50RT_Status;

typedef enum MFX50RT_LogLevel {
    MFX50RT_LOG_ERROR = 1,
    MFX50RT_LOG_WARN = 2,
    MFX50RT_LOG_INFO = 3,
    MFX50RT_LOG_DEBUG = 4,
    MFX50RT_LOG_TRACE = 5
} MFX50RT_LogLevel;

typedef struct MFX50RT_Config {
    uint32_t struct_size;

    MFX50_InputMode input_mode;
    MFX50_OutputMode output_mode;

    MFX50_Codec input_codec;
    MFX50_Codec output_codec;

    int width;
    int height;
    int fps_num;
    int fps_den;

    const char* device_selector;
    MFX50_Profile profile;

    int route_count;
    int async_depth;
    int max_queue_packets;
    int max_queue_surfaces;
    int algo_budget_us;

    int target_usage;
    int gop;
    int gop_ref_dist;
    int num_ref_frame;
    int qpi;
    int qpp;
    int qpb;
    int bref_type; /* 0 auto, 1 off, 2 pyramid */

    int enable_trace;
    const char* trace_path;

    void* user_opaque;

    uint32_t abi_version;

    int async_mode;
    int max_input_queue_packets;
    int max_output_queue_packets;
    int drop_policy;
    int enable_static_reuse;
} MFX50RT_Config;

typedef struct MFX50RT_Packet {
    uint32_t struct_size;

    int stream_id;
    const uint8_t* data;
    size_t size;

    int64_t pts;
    int64_t dts;

    int is_keyframe;
    int end_of_stream;

    void* user_opaque;
} MFX50RT_Packet;

typedef struct MFX50RT_RawFrame {
    uint32_t struct_size;

    int stream_id;
    MFX50_PixelFormat format;

    uint8_t* data[3];
    int stride[3];

    int width;
    int height;

    int64_t pts;

    void* user_opaque;
} MFX50RT_RawFrame;

typedef struct MFX50RT_EncodedPacket {
    uint32_t struct_size;

    int stream_id;
    uint8_t* data;
    size_t size;
    size_t capacity;

    int64_t pts;
    int64_t dts;

    int is_keyframe;
    int frame_type;

    void* user_opaque;
} MFX50RT_EncodedPacket;

typedef void (*MFX50RT_OutputCallback)(
    const MFX50RT_EncodedPacket* pkt,
    void* user_opaque);

typedef void (*MFX50RT_LogCallback)(
    int level,
    const char* message,
    void* user_opaque);

typedef struct MFX50RT_Stats {
    uint32_t struct_size;

    uint64_t input_packets;
    uint64_t decoded_frames;
    uint64_t encoded_frames;
    uint64_t output_packets;

    uint64_t dropped_frames;
    uint64_t fallback_frames;

    double fps_in;
    double fps_out;

    double avg_decode_us;
    double avg_algo_us;
    double avg_encode_submit_us;
    double avg_sync_us;

    int current_queue_packets;
    int current_queue_surfaces;

    int last_error_code;
    char last_error_msg[256];

    uint64_t input_bytes;
    uint64_t output_bytes;
    uint64_t decode_errors;
    uint64_t encode_errors;

    int route_count;
    int abi_version;

    uint64_t preprocess_frames;
    uint64_t smooth_scale_frames;
    uint64_t pre_denoise_frames;
    uint64_t scene_analyzed_frames;
    uint64_t adaptive_profile_switches;
    uint64_t adaptive_qp_frames;
    uint64_t mbqp_frames;
    uint64_t mbqp_fallback_frames;
    uint64_t mbqp_skipped_frames;
    uint64_t mbqp_init_enabled_frames;

    double avg_preprocess_ms;
    double avg_scene_analyze_ms;
    double avg_mbqp_build_ms;

    int mbqp_supported;
    int mbqp_disabled_reason;
    int active_profile;
    int active_algo_flags;

    int current_input_queue_packets;
    int async_mode;
    uint64_t async_enqueued_packets;
    uint64_t async_processed_packets;
    uint64_t backpressure_events;
} MFX50RT_Stats;

MFX50RT_API const char* MFX50RT_GetVersion(void);

MFX50RT_API int MFX50RT_GetAbiVersion(void);

MFX50RT_API const char* MFX50RT_StatusString(int code);

MFX50RT_API int MFX50RT_DefaultConfig(MFX50RT_Config* cfg);

MFX50RT_API int MFX50RT_Create(
    const MFX50RT_Config* cfg,
    MFX50RT_Handle* out_handle);

MFX50RT_API int MFX50RT_SetOutputCallback(
    MFX50RT_Handle h,
    MFX50RT_OutputCallback cb,
    void* user_opaque);

MFX50RT_API int MFX50RT_SetLogCallback(
    MFX50RT_Handle h,
    MFX50RT_LogCallback cb,
    void* user_opaque);

MFX50RT_API int MFX50RT_PushPacket(
    MFX50RT_Handle h,
    const MFX50RT_Packet* packet);

MFX50RT_API int MFX50RT_PushFrame(
    MFX50RT_Handle h,
    const MFX50RT_RawFrame* frame);

MFX50RT_API int MFX50RT_PollPacket(
    MFX50RT_Handle h,
    MFX50RT_EncodedPacket* out_packet);

MFX50RT_API int MFX50RT_Flush(MFX50RT_Handle h);

MFX50RT_API int MFX50RT_GetStats(
    MFX50RT_Handle h,
    MFX50RT_Stats* stats);

MFX50RT_API const char* MFX50RT_GetLastError(MFX50RT_Handle h);

MFX50RT_API int MFX50RT_Close(MFX50RT_Handle h);

#include "mfx50_realtime_algo.h"
