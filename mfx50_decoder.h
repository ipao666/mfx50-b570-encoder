#pragma once

#include "mfx50_api.h"
#include "mfx50_device.h"
#include "mfx50_surface.h"
#include "mfx50_types.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MFX50_Decoder MFX50_Decoder;

typedef struct MFX50_DecoderConfig {
    uint32_t struct_size;
    uint32_t api_version;
    MFX50_Codec input_codec;
    int32_t width;
    int32_t height;
    int32_t fps_num;
    int32_t fps_den;
    int32_t annexb_input;
    int32_t low_latency;
    int32_t async_depth;
    int32_t max_output_surfaces;
    char options_json[1024];
    uint8_t reserved[256];
} MFX50_DecoderConfig;

/* Fill MFX50_DecoderConfig with ABI version and low-latency packet input defaults. */
MFX50_API MFX50_Status mfx50_decoder_default_config(MFX50_DecoderConfig* config);

/* Create a decoder under the shared MFX50_Device used by the matching encoder. */
MFX50_API MFX50_Status mfx50_decoder_create(
    MFX50_Device* device,
    const MFX50_DecoderConfig* config,
    MFX50_Decoder** out_decoder);

/* Push one encoded H.264/H.265 access unit or packet into the decoder. */
MFX50_API MFX50_Status mfx50_decoder_push_packet(
    MFX50_Decoder* decoder,
    const MFX50_Packet* packet);

/* Poll decoded NV12 GPU surfaces that can be passed to MFX50 Encoder. */
MFX50_API MFX50_Status mfx50_decoder_poll_surface(
    MFX50_Decoder* decoder,
    MFX50_Surface* out_surface);

/* Signal end-of-input to the decoder. */
MFX50_API MFX50_Status mfx50_decoder_flush(MFX50_Decoder* decoder);

/* Return the last detailed decoder error string for diagnostics. */
MFX50_API const char* mfx50_decoder_get_last_error(MFX50_Decoder* decoder);

/* Destroy the decoder object. Surfaces already polled follow MFX50_Surface lifetime rules. */
MFX50_API void mfx50_decoder_destroy(MFX50_Decoder* decoder);

#ifdef __cplusplus
}
#endif
