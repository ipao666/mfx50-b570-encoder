#include "mfx50_decoder.h"
#include "mfx50_encoder.h"
#include "mfx50_transcoder.h"

#include <stdio.h>
#include <string.h>

static int require_true(int condition, const char* message) {
    if (condition) return 0;
    fprintf(stderr, "require failed: %s\n", message);
    return 1;
}

int main(void) {
    MFX50_DeviceConfig device_cfg;
    if (require_true(mfx50_device_default_config(&device_cfg) == MFX50_OK, "device default config")) return 1;

    MFX50_Device* device = 0;
    if (require_true(mfx50_device_create(&device_cfg, &device) == MFX50_OK, "device create")) return 1;
    if (require_true(device != 0, "device handle")) return 1;

    MFX50_DecoderConfig decoder_cfg;
    if (require_true(mfx50_decoder_default_config(&decoder_cfg) == MFX50_OK, "decoder default config")) return 1;
    decoder_cfg.input_codec = MFX50_CODEC_H265;

    MFX50_Decoder* decoder = 0;
    if (require_true(mfx50_decoder_create(device, &decoder_cfg, &decoder) == MFX50_OK, "decoder create")) return 1;
    if (require_true(decoder != 0, "decoder handle")) return 1;

    {
        const unsigned char partial_hevc_header[] = {0x00, 0x00, 0x00, 0x01, 0x40, 0x01};
        MFX50_Packet packet;
        memset(&packet, 0, sizeof(packet));
        packet.struct_size = sizeof(packet);
        packet.api_version = MFX50_DEVICE_API_VERSION;
        packet.codec = MFX50_CODEC_HEVC;
        packet.data = partial_hevc_header;
        packet.data_size = sizeof(partial_hevc_header);
        packet.pts = 0;
        packet.dts = 0;
        MFX50_Status push_st = mfx50_decoder_push_packet(decoder, &packet);
        if (require_true(push_st != MFX50_ERR_NOT_IMPLEMENTED,
                         "decoder push packet must be implemented")) return 1;
        if (require_true(push_st == MFX50_OK || push_st == MFX50_ERR_AGAIN,
                         "decoder push partial header accepted as pending input")) return 1;

        packet.codec = MFX50_CODEC_H264;
        push_st = mfx50_decoder_push_packet(decoder, &packet);
        if (require_true(push_st == MFX50_ERR_INVALID_PARAM,
                         "decoder rejects packet codec mismatch")) return 1;

        MFX50_Surface decoded_surface;
        memset(&decoded_surface, 0, sizeof(decoded_surface));
        MFX50_Status poll_st = mfx50_decoder_poll_surface(decoder, &decoded_surface);
        if (poll_st == MFX50_OK) {
            if (require_true(decoded_surface.type == MFX50_SURFACE_ONEVPL,
                             "decoded surface is oneVPL")) return 1;
            mfx50_surface_release(&decoded_surface);
        } else if (require_true(poll_st == MFX50_ERR_AGAIN,
                                "partial header has no decoded surface yet")) {
            return 1;
        }

        if (require_true(mfx50_decoder_flush(decoder) == MFX50_OK,
                         "decoder flush partial input")) return 1;
        memset(&decoded_surface, 0, sizeof(decoded_surface));
        poll_st = mfx50_decoder_poll_surface(decoder, &decoded_surface);
        if (require_true(poll_st == MFX50_ERR_EOS,
                         "decoder poll returns EOS after flush without frames")) return 1;

        packet.codec = MFX50_CODEC_HEVC;
        push_st = mfx50_decoder_push_packet(decoder, &packet);
        if (require_true(push_st == MFX50_ERR_BAD_STATE,
                         "decoder rejects push after flush")) return 1;
    }
    mfx50_decoder_destroy(decoder);
    decoder = 0;

    decoder_cfg.input_codec = MFX50_CODEC_UNKNOWN;
    if (require_true(mfx50_decoder_create(device, &decoder_cfg, &decoder) == MFX50_OK,
                     "auto decoder create")) return 1;
    {
        const unsigned char partial_hevc_header[] = {0x00, 0x00, 0x00, 0x01, 0x40, 0x01};
        MFX50_Packet packet;
        memset(&packet, 0, sizeof(packet));
        packet.struct_size = sizeof(packet);
        packet.api_version = MFX50_DEVICE_API_VERSION;
        packet.codec = MFX50_CODEC_UNKNOWN;
        packet.data = partial_hevc_header;
        packet.data_size = sizeof(partial_hevc_header);
        MFX50_Status push_st = mfx50_decoder_push_packet(decoder, &packet);
        if (require_true(push_st != MFX50_ERR_INVALID_PARAM &&
                         push_st != MFX50_ERR_NOT_IMPLEMENTED,
                         "auto decoder sniffs HEVC Annex-B header")) return 1;
        if (require_true(mfx50_decoder_flush(decoder) == MFX50_OK,
                         "auto decoder flush partial input")) return 1;
        MFX50_Surface decoded_surface;
        memset(&decoded_surface, 0, sizeof(decoded_surface));
        MFX50_Status poll_st = mfx50_decoder_poll_surface(decoder, &decoded_surface);
        if (require_true(poll_st == MFX50_ERR_EOS,
                         "auto decoder poll returns EOS after flush without frames")) return 1;
    }
    mfx50_decoder_destroy(decoder);
    decoder = 0;

    MFX50_EncoderConfig encoder_cfg;
    if (require_true(mfx50_encoder_default_config(&encoder_cfg) == MFX50_OK, "encoder default config")) return 1;
    encoder_cfg.output_codec = MFX50_CODEC_HEVC;
    encoder_cfg.input_format = MFX50_PIXFMT_NV12;

    MFX50_EncoderSurfaceSupport support;
    if (require_true(mfx50_encoder_get_surface_support(device, &support) == MFX50_OK, "surface support")) return 1;
    if (require_true(support.supports_onevpl_surface == 1, "oneVPL surface advertised as supported")) return 1;
    if (require_true(support.supports_vaapi_surface == 0, "vaapi not advertised as supported yet")) return 1;
    if (require_true(support.supports_dmabuf == 0, "dmabuf not advertised as supported")) return 1;

    MFX50_Encoder* encoder = 0;
    if (require_true(mfx50_encoder_create(device, &encoder_cfg, &encoder) == MFX50_OK, "encoder create")) return 1;
    if (require_true(encoder != 0, "encoder handle")) return 1;

    MFX50_Surface surface;
    memset(&surface, 0, sizeof(surface));
    surface.struct_size = sizeof(surface);
    surface.api_version = MFX50_DEVICE_API_VERSION;
    surface.type = MFX50_SURFACE_VAAPI;
    surface.pixel_format = MFX50_PIXFMT_NV12;
    surface.width = 1920;
    surface.height = 1080;

    if (require_true(mfx50_encoder_push_surface(encoder, &surface, 0) == MFX50_ERR_NOT_IMPLEMENTED,
                     "surface encode returns not implemented")) return 1;

    mfx50_encoder_destroy(encoder);
    if (decoder) mfx50_decoder_destroy(decoder);
    mfx50_device_destroy(device);
    return 0;
}
