#pragma once

#include "mfx50_api.h"
#include "mfx50_types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MFX50_SURFACE_FLAG_EXTERNAL_OWNER (1u << 0)
#define MFX50_SURFACE_FLAG_SYNC_ONLY      (1u << 1)
#define MFX50_SURFACE_FLAG_READ_ONLY      (1u << 2)
#define MFX50_SURFACE_FLAG_EOS            (1u << 3)

#define MFX50_PACKET_FLAG_KEYFRAME        (1u << 0)
#define MFX50_PACKET_FLAG_EOS             (1u << 1)
#define MFX50_PACKET_FLAG_CONFIG          (1u << 2)
#define MFX50_PACKET_FLAG_DISCONT         (1u << 3)

typedef enum MFX50_SurfaceType {
    MFX50_SURFACE_UNKNOWN = 0,
    MFX50_SURFACE_ONEVPL = 1,
    MFX50_SURFACE_VAAPI = 2,
    MFX50_SURFACE_DMABUF = 3
} MFX50_SurfaceType;

typedef MFX50_Status (*MFX50_SurfaceAddRefCallback)(void* ref_opaque);
typedef void (*MFX50_SurfaceReleaseCallback)(void* ref_opaque);
typedef void (*MFX50_PacketReleaseCallback)(void* release_opaque);

typedef struct MFX50_Surface {
    uint32_t struct_size;
    uint32_t api_version;
    MFX50_SurfaceType type;
    MFX50_PixelFormat pixel_format;
    int32_t width;
    int32_t height;
    int32_t crop_x;
    int32_t crop_y;
    int32_t crop_w;
    int32_t crop_h;
    int64_t pts;
    int64_t dts;
    uint32_t flags;
    union {
        struct {
            void* mfx_surface;
        } onevpl;
        struct {
            void* va_display;
            uint32_t va_surface_id;
        } vaapi;
        struct {
            int fd;
            int32_t stride[4];
            int32_t offset[4];
            uint64_t modifier;
        } dmabuf;
    } handle;
    void* ref_opaque;
    MFX50_SurfaceAddRefCallback add_ref;
    MFX50_SurfaceReleaseCallback release;
    void* user_opaque;
    uint8_t reserved[128];
} MFX50_Surface;

typedef struct MFX50_Packet {
    uint32_t struct_size;
    uint32_t api_version;
    MFX50_Codec codec;
    const uint8_t* data;
    size_t data_size;
    int64_t pts;
    int64_t dts;
    uint32_t flags;
    void* packet_handle;
    void* release_opaque;
    MFX50_PacketReleaseCallback release;
    void* user_opaque;
    uint8_t reserved[128];
} MFX50_Packet;

MFX50_API MFX50_Status mfx50_surface_add_ref(const MFX50_Surface* surface);
MFX50_API void mfx50_surface_release(MFX50_Surface* surface);
MFX50_API void mfx50_packet_release(MFX50_Packet* packet);

#ifdef __cplusplus
}
#endif
