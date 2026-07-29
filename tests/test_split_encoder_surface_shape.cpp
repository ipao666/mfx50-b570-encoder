#include "mfx50_device.h"
#include "mfx50_encoder.h"

#include <vpl/mfxvideo.h>

#include <cstdio>
#include <cstring>

static int require_true(bool condition, const char* message) {
    if (condition) return 0;
    std::fprintf(stderr, "require failed: %s\n", message);
    return 1;
}

int main() {
    MFX50_DeviceConfig device_cfg;
    if (require_true(mfx50_device_default_config(&device_cfg) == MFX50_OK,
                     "device default config")) return 1;

    MFX50_Device* device = nullptr;
    if (require_true(mfx50_device_create(&device_cfg, &device) == MFX50_OK,
                     "device create")) return 1;

    MFX50_EncoderConfig encoder_cfg;
    if (require_true(mfx50_encoder_default_config(&encoder_cfg) == MFX50_OK,
                     "encoder default config")) return 1;
    encoder_cfg.output_codec = MFX50_CODEC_HEVC;
    encoder_cfg.input_format = MFX50_PIXFMT_NV12;
    encoder_cfg.width = 0;
    encoder_cfg.height = 0;

    MFX50_Encoder* encoder = nullptr;
    if (require_true(mfx50_encoder_create(device, &encoder_cfg, &encoder) == MFX50_OK,
                     "encoder create")) return 1;

    mfxFrameSurface1 mfx_surface;
    std::memset(&mfx_surface, 0, sizeof(mfx_surface));
    mfx_surface.Info.FourCC = MFX_FOURCC_NV12;
    mfx_surface.Info.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    mfx_surface.Info.Width = 2304;
    mfx_surface.Info.Height = 1280;
    mfx_surface.Info.CropW = 2304;
    mfx_surface.Info.CropH = 1296;
    mfx_surface.Data.MemId = reinterpret_cast<mfxMemId>(0x1);

    MFX50_Surface surface;
    std::memset(&surface, 0, sizeof(surface));
    surface.struct_size = sizeof(surface);
    surface.api_version = MFX50_DEVICE_API_VERSION;
    surface.type = MFX50_SURFACE_ONEVPL;
    surface.pixel_format = MFX50_PIXFMT_NV12;
    surface.width = 2304;
    surface.height = 1280;
    surface.crop_w = 2304;
    surface.crop_h = 1296;
    surface.handle.onevpl.mfx_surface = &mfx_surface;

    MFX50_Status st = mfx50_encoder_push_surface(encoder, &surface, nullptr);
    if (require_true(st == MFX50_ERR_INVALID_PARAM,
                     "undersized external backing rejected before oneVPL init")) return 1;

    const char* err = mfx50_encoder_get_last_error(encoder);
    if (require_true(err && std::strstr(err, "backing is smaller") != nullptr,
                     "diagnostic mentions backing/crop mismatch")) return 1;

    mfx50_encoder_destroy(encoder);
    mfx50_device_destroy(device);
    return 0;
}
