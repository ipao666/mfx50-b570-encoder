#include "mfx50_decoder.h"
#include "mfx50_encoder.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::string input;
    std::string output;
    MFX50_Codec input_codec = MFX50_CODEC_HEVC;
    int width = 0;
    int height = 0;
    int fps = 25;
    int gop = 30;
    int qp = 32;
};

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s <in.h265|in.h264> <out.hevc> [h265|h264] [fps] [gop] [qp]\n",
                 argv0);
}

MFX50_Codec parse_codec(const char* text) {
    if (!text) return MFX50_CODEC_HEVC;
    if (std::strcmp(text, "h264") == 0 || std::strcmp(text, "avc") == 0) {
        return MFX50_CODEC_H264;
    }
    return MFX50_CODEC_HEVC;
}

bool parse_args(int argc, char** argv, Options* opt) {
    if (!opt || argc < 3) return false;
    opt->input = argv[1];
    opt->output = argv[2];
    if (argc > 3) opt->input_codec = parse_codec(argv[3]);
    if (argc > 4) opt->fps = std::atoi(argv[4]);
    if (argc > 5) opt->gop = std::atoi(argv[5]);
    if (argc > 6) opt->qp = std::atoi(argv[6]);
    if (opt->fps <= 0) opt->fps = 25;
    if (opt->gop <= 0) opt->gop = 30;
    if (opt->qp <= 0) opt->qp = 32;
    return !opt->input.empty() && !opt->output.empty();
}

bool drain_encoder(MFX50_Encoder* encoder, std::ofstream* output, uint64_t* packets, uint64_t* bytes) {
    for (;;) {
        MFX50_Packet packet;
        std::memset(&packet, 0, sizeof(packet));
        MFX50_Status st = mfx50_encoder_poll_packet(encoder, &packet);
        if (st == MFX50_ERR_AGAIN) return true;
        if (st != MFX50_OK) {
            std::fprintf(stderr, "encoder_poll_packet failed: %s (%s)\n",
                         mfx50_status_string(st),
                         mfx50_encoder_get_last_error(encoder));
            return false;
        }
        output->write(reinterpret_cast<const char*>(packet.data),
                      static_cast<std::streamsize>(packet.data_size));
        if (!*output) {
            std::fprintf(stderr, "write output failed\n");
            mfx50_packet_release(&packet);
            return false;
        }
        if (packets) (*packets)++;
        if (bytes) (*bytes) += packet.data_size;
        mfx50_packet_release(&packet);
    }
}

bool drain_decoder_to_encoder(MFX50_Decoder* decoder,
                              MFX50_Encoder* encoder,
                              std::ofstream* output,
                              uint64_t* surfaces,
                               uint64_t* packets,
                               uint64_t* bytes,
                               bool expect_eos) {
    int idle_polls = 0;
    for (;;) {
        MFX50_Surface surface;
        std::memset(&surface, 0, sizeof(surface));
        MFX50_Status st = mfx50_decoder_poll_surface(decoder, &surface);
        if (st == MFX50_ERR_AGAIN) {
            if (!expect_eos) return true;
            if (++idle_polls > 60000) {
                std::fprintf(stderr, "decoder drain timed out while waiting for EOS\n");
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (st == MFX50_ERR_EOS) return true;
        if (st != MFX50_OK) {
            std::fprintf(stderr, "decoder_poll_surface failed: %s (%s)\n",
                         mfx50_status_string(st),
                         mfx50_decoder_get_last_error(decoder));
            return false;
        }
        idle_polls = 0;

        st = mfx50_encoder_push_surface(encoder, &surface, nullptr);
        if (st != MFX50_OK) {
            std::fprintf(stderr, "encoder_push_surface failed: %s (%s)\n",
                         mfx50_status_string(st),
                         mfx50_encoder_get_last_error(encoder));
            mfx50_surface_release(&surface);
            return false;
        }
        if (surfaces) (*surfaces)++;
        mfx50_surface_release(&surface);
        if (!drain_encoder(encoder, output, packets, bytes)) return false;
    }
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, &opt)) {
        usage(argv[0]);
        return 2;
    }

    std::ifstream input(opt.input, std::ios::binary);
    if (!input) {
        std::fprintf(stderr, "cannot open input: %s\n", opt.input.c_str());
        return 1;
    }
    std::ofstream output(opt.output, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::fprintf(stderr, "cannot open output: %s\n", opt.output.c_str());
        return 1;
    }

    MFX50_DeviceConfig device_cfg;
    MFX50_DecoderConfig decoder_cfg;
    MFX50_EncoderConfig encoder_cfg;
    if (mfx50_device_default_config(&device_cfg) != MFX50_OK ||
        mfx50_decoder_default_config(&decoder_cfg) != MFX50_OK ||
        mfx50_encoder_default_config(&encoder_cfg) != MFX50_OK) {
        std::fprintf(stderr, "default config failed\n");
        return 1;
    }
    decoder_cfg.input_codec = opt.input_codec;
    decoder_cfg.fps_num = opt.fps;
    decoder_cfg.fps_den = 1;
    decoder_cfg.async_depth = 4;
    decoder_cfg.max_output_surfaces = 16;

    encoder_cfg.output_codec = MFX50_CODEC_HEVC;
    encoder_cfg.input_format = MFX50_PIXFMT_NV12;
    encoder_cfg.fps_num = opt.fps;
    encoder_cfg.fps_den = 1;
    encoder_cfg.gop_size = opt.gop;
    encoder_cfg.qpi = opt.qp;
    encoder_cfg.qpp = opt.qp;
    encoder_cfg.qpb = opt.qp;
    encoder_cfg.async_depth = 4;

    MFX50_Device* device = nullptr;
    MFX50_Decoder* decoder = nullptr;
    MFX50_Encoder* encoder = nullptr;
    MFX50_Status st = mfx50_device_create(&device_cfg, &device);
    if (st != MFX50_OK) {
        std::fprintf(stderr, "device_create failed: %s (%s)\n",
                     mfx50_status_string(st),
                     mfx50_device_get_last_error(device));
        return 1;
    }
    st = mfx50_decoder_create(device, &decoder_cfg, &decoder);
    if (st != MFX50_OK) {
        std::fprintf(stderr, "decoder_create failed: %s (%s)\n",
                     mfx50_status_string(st),
                     mfx50_device_get_last_error(device));
        mfx50_device_destroy(device);
        return 1;
    }
    st = mfx50_encoder_create(device, &encoder_cfg, &encoder);
    if (st != MFX50_OK) {
        std::fprintf(stderr, "encoder_create failed: %s (%s)\n",
                     mfx50_status_string(st),
                     mfx50_device_get_last_error(device));
        mfx50_decoder_destroy(decoder);
        mfx50_device_destroy(device);
        return 1;
    }

    std::vector<unsigned char> buffer(256 * 1024);
    uint64_t chunks = 0;
    uint64_t surfaces = 0;
    uint64_t packets = 0;
    uint64_t bytes = 0;
    bool ok = true;
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = input.gcount();
        if (got <= 0) break;
        MFX50_Packet packet;
        std::memset(&packet, 0, sizeof(packet));
        packet.struct_size = sizeof(packet);
        packet.api_version = MFX50_DEVICE_API_VERSION;
        packet.codec = opt.input_codec;
        packet.data = buffer.data();
        packet.data_size = static_cast<size_t>(got);
        packet.pts = static_cast<int64_t>(chunks);
        packet.dts = packet.pts;
        st = mfx50_decoder_push_packet(decoder, &packet);
        if (st != MFX50_OK) {
            std::fprintf(stderr, "decoder_push_packet failed: %s (%s)\n",
                         mfx50_status_string(st),
                         mfx50_decoder_get_last_error(decoder));
            ok = false;
            break;
        }
        chunks++;
        if (!drain_decoder_to_encoder(decoder, encoder, &output, &surfaces, &packets, &bytes, false)) {
            ok = false;
            break;
        }
    }

    if (ok) {
        st = mfx50_decoder_flush(decoder);
        if (st != MFX50_OK) {
            std::fprintf(stderr, "decoder_flush failed: %s (%s)\n",
                         mfx50_status_string(st),
                         mfx50_decoder_get_last_error(decoder));
            ok = false;
        }
    }
    if (ok && !drain_decoder_to_encoder(decoder, encoder, &output, &surfaces, &packets, &bytes, true)) {
        ok = false;
    }
    if (ok) {
        st = mfx50_encoder_flush(encoder);
        if (st != MFX50_OK) {
            std::fprintf(stderr, "encoder_flush failed: %s (%s)\n",
                         mfx50_status_string(st),
                         mfx50_encoder_get_last_error(encoder));
            ok = false;
        }
    }
    if (ok && !drain_encoder(encoder, &output, &packets, &bytes)) ok = false;

    mfx50_encoder_destroy(encoder);
    mfx50_decoder_destroy(decoder);
    mfx50_device_destroy(device);
    output.close();

    std::printf("input_chunks=%llu\n", static_cast<unsigned long long>(chunks));
    std::printf("decoded_surfaces=%llu\n", static_cast<unsigned long long>(surfaces));
    std::printf("encoded_packets=%llu\n", static_cast<unsigned long long>(packets));
    std::printf("output_bytes=%llu\n", static_cast<unsigned long long>(bytes));
    return ok ? 0 : 1;
}
