#pragma once

#include "mfx50_api.h"
#include "mfx50_device.h"
#include "mfx50_surface.h"
#include "mfx50_types.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MFX50_Encoder MFX50_Encoder;

typedef enum MFX50_RateControl {
    MFX50_RC_AUTO = 0,
    MFX50_RC_CQP = 1,
    MFX50_RC_QVBR = 2,
    MFX50_RC_ICQ = 3,
    MFX50_RC_VBR = 4,
    MFX50_RC_CBR = 5
} MFX50_RateControl;

typedef struct MFX50_EncoderConfig {
    uint32_t struct_size;
    uint32_t api_version;
    MFX50_Codec output_codec;
    MFX50_PixelFormat input_format;
    int32_t width;
    int32_t height;
    int32_t fps_num;
    int32_t fps_den;
    int32_t gop_size;
    int32_t b_frames;
    MFX50_RateControl rc_mode;
    int32_t target_bitrate_kbps;
    int32_t max_bitrate_kbps;
    int32_t qpi;
    int32_t qpp;
    int32_t qpb;
    int32_t async_depth;
    int32_t low_latency;
    int32_t require_zero_copy;
    char options_json[1024];
    uint8_t reserved[256];
} MFX50_EncoderConfig;

typedef struct MFX50_EncoderSurfaceSupport {
    uint32_t struct_size;
    uint32_t api_version;
    int32_t supports_onevpl_surface;
    int32_t supports_vaapi_surface;
    int32_t supports_dmabuf;
    int32_t dmabuf_experimental;
    char implementation_note[256];
    uint8_t reserved[128];
} MFX50_EncoderSurfaceSupport;

/* Fill MFX50_EncoderConfig with ABI version, NV12 input, HEVC output and default QP values. */
MFX50_API MFX50_Status mfx50_encoder_default_config(MFX50_EncoderConfig* config);

/* Query which MFX50_Surface handle types the current encoder implementation accepts. */
MFX50_API MFX50_Status mfx50_encoder_get_surface_support(
    MFX50_Device* device,
    MFX50_EncoderSurfaceSupport* out_support);

/* Create an encode-only encoder under the shared MFX50_Device. */
MFX50_API MFX50_Status mfx50_encoder_create(
    MFX50_Device* device,
    const MFX50_EncoderConfig* config,
    MFX50_Encoder** out_encoder);

/* Submit one NV12 GPU surface and an optional policy decision for encoding. */
MFX50_API MFX50_Status mfx50_encoder_push_surface(
    MFX50_Encoder* encoder,
    const MFX50_Surface* surface,
    const MFX50_EncodeDecision* decision);

/* Poll encoded packets produced by mfx50_encoder_push_surface/flush. Current implementation outputs HEVC/H.265. */
MFX50_API MFX50_Status mfx50_encoder_poll_packet(
    MFX50_Encoder* encoder,
    MFX50_Packet* out_packet);

/* Signal end-of-input and drain delayed packets. */
MFX50_API MFX50_Status mfx50_encoder_flush(MFX50_Encoder* encoder);

/* Return the last detailed encoder error string for diagnostics. */
MFX50_API const char* mfx50_encoder_get_last_error(MFX50_Encoder* encoder);

/* Destroy the encoder object. Packets already polled remain valid until released. */
MFX50_API void mfx50_encoder_destroy(MFX50_Encoder* encoder);

#ifdef __cplusplus
}
#endif
