#include "mfx50_decoder.h"
#include "mfx50_encoder.h"

#include <vpl/mfxdispatcher.h>
#include <vpl/mfxvideo.h>
#include <va/va.h>
#include <va/va_drm.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {

thread_local std::string g_last_error;

template <typename T>
void init_struct(T* value) {
    std::memset(value, 0, sizeof(*value));
    value->struct_size = sizeof(*value);
    value->api_version = MFX50_DEVICE_API_VERSION;
}

template <typename T, typename Defaults>
T copy_config_or_default(const T* input, Defaults defaults) {
    T out{};
    defaults(&out);
    if (!input) return out;
    const size_t n = input->struct_size > 0
        ? std::min<size_t>(input->struct_size, sizeof(T))
        : sizeof(T);
    std::memcpy(&out, input, n);
    out.struct_size = sizeof(T);
    if (out.api_version == 0) out.api_version = MFX50_DEVICE_API_VERSION;
    return out;
}

void set_error(std::string* target, const char* msg) {
    g_last_error = msg ? msg : "";
    if (target) *target = g_last_error;
}

void copy_note(char* dst, size_t cap, const char* msg) {
    if (!dst || cap == 0) return;
    std::snprintf(dst, cap, "%s", msg ? msg : "");
}

int align16(int value) {
    return (value + 15) & ~15;
}

int positive_max(int a, int b) {
    return std::max(a > 0 ? a : 0, b > 0 ? b : 0);
}

std::string mfx_status_name(mfxStatus st) {
    switch (st) {
        case MFX_ERR_NONE: return "MFX_ERR_NONE";
        case MFX_ERR_MORE_DATA: return "MFX_ERR_MORE_DATA";
        case MFX_ERR_MORE_SURFACE: return "MFX_ERR_MORE_SURFACE";
        case MFX_ERR_MORE_BITSTREAM: return "MFX_ERR_MORE_BITSTREAM";
        case MFX_WRN_DEVICE_BUSY: return "MFX_WRN_DEVICE_BUSY";
        case MFX_ERR_NOT_ENOUGH_BUFFER: return "MFX_ERR_NOT_ENOUGH_BUFFER";
        case MFX_ERR_UNSUPPORTED: return "MFX_ERR_UNSUPPORTED";
        case MFX_ERR_NOT_IMPLEMENTED: return "MFX_ERR_NOT_IMPLEMENTED";
        case MFX_ERR_INVALID_VIDEO_PARAM: return "MFX_ERR_INVALID_VIDEO_PARAM";
        case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM: return "MFX_ERR_INCOMPATIBLE_VIDEO_PARAM";
        case MFX_ERR_DEVICE_FAILED: return "MFX_ERR_DEVICE_FAILED";
        case MFX_ERR_NOT_FOUND: return "MFX_ERR_NOT_FOUND";
        default: return "mfxStatus(" + std::to_string(st) + ")";
    }
}

int parse_drm_render_node_num(const char* path) {
    if (!path) return 0;
    const char* key = std::strstr(path, "renderD");
    if (!key) return 0;
    key += 7;
    int value = 0;
    bool has_digit = false;
    while (*key >= '0' && *key <= '9') {
        has_digit = true;
        value = value * 10 + (*key - '0');
        ++key;
    }
    return has_digit ? value : 0;
}

void reset_bitstream(mfxBitstream* bs, std::vector<uint8_t>* storage) {
    std::memset(bs, 0, sizeof(*bs));
    bs->Data = storage->data();
    bs->MaxLength = static_cast<mfxU32>(storage->size());
}

bool hevc_contains_idr(const uint8_t* data, size_t size) {
    if (!data || size < 6) return false;
    for (size_t i = 0; i + 5 < size; ++i) {
        size_t off = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) off = i + 3;
        else if (i + 6 < size && data[i] == 0 && data[i + 1] == 0 &&
                 data[i + 2] == 0 && data[i + 3] == 1) off = i + 4;
        else continue;
        const uint8_t nal_type = static_cast<uint8_t>((data[off] >> 1) & 0x3f);
        if (nal_type == 19 || nal_type == 20) return true;
    }
    return false;
}

} // namespace

struct EncodedBuffer {
    MFX50_Codec codec = MFX50_CODEC_UNKNOWN;
    std::vector<uint8_t> data;
    int64_t pts = 0;
    int64_t dts = 0;
    uint32_t flags = 0;
    void* user_opaque = nullptr;
};

struct DecodedSurfaceRef {
    mfxFrameSurface1* surface = nullptr;
    std::atomic<int> refs{1};
};

struct DecodedSurfaceItem {
    mfxFrameSurface1* surface = nullptr;
    int64_t pts = 0;
    int64_t dts = 0;
    void* user_opaque = nullptr;
};

struct MFX50_Device {
    MFX50_DeviceConfig config{};
    std::string last_error;
    int fd = -1;
    VADisplay va_display = nullptr;
    bool va_initialized = false;
    bool owns_va_display = false;
    mfxLoader loader = nullptr;
    mfxSession session = nullptr;
    bool owns_session = false;
    bool runtime_ready = false;
};

struct MFX50_Decoder {
    MFX50_Device* device = nullptr;
    MFX50_DecoderConfig config{};
    std::string last_error;
    bool decoder_ready = false;
    bool flushing = false;
    bool drained = false;
    MFX50_Codec active_codec = MFX50_CODEC_UNKNOWN;
    mfxVideoParam par{};
    std::vector<uint8_t> bitstream_storage;
    mfxBitstream input_bs{};
    struct DecodeOp {
        bool in_use = false;
        mfxFrameSurface1* surface = nullptr;
        mfxSyncPoint sync = nullptr;
        int64_t pts = 0;
        int64_t dts = 0;
        void* user_opaque = nullptr;
    };
    std::vector<DecodeOp> ops;
    std::deque<DecodedSurfaceItem> output_queue;
    uint64_t input_packets = 0;
    uint64_t output_surfaces = 0;
    int64_t last_pts = -1;
    int64_t last_dts = -1;
    void* last_user_opaque = nullptr;
};

struct MFX50_Encoder {
    MFX50_Device* device = nullptr;
    MFX50_EncoderConfig config{};
    std::string last_error;
    bool encoder_ready = false;
    bool flushed = false;
    mfxVideoParam par{};
    std::vector<uint8_t> bitstream_storage;
    std::deque<EncodedBuffer> output_queue;
    uint64_t input_frames = 0;
    uint64_t output_packets = 0;
    int io_pattern = 0;
    int width = 0;
    int height = 0;
};

namespace {

void cleanup_device_runtime(MFX50_Device* device) {
    if (!device) return;
    if (device->session && device->owns_session) {
        MFXClose(device->session);
    }
    device->session = nullptr;
    if (device->loader) {
        MFXUnload(device->loader);
    }
    device->loader = nullptr;
    if (device->va_display && device->va_initialized && device->owns_va_display) {
        vaTerminate(device->va_display);
    }
    device->va_display = nullptr;
    device->va_initialized = false;
    if (device->fd >= 0) {
        close(device->fd);
    }
    device->fd = -1;
    device->runtime_ready = false;
}

MFX50_Status ensure_device_runtime(MFX50_Device* device) {
    if (!device) return MFX50_ERR_INVALID_PARAM;
    if (device->runtime_ready) return MFX50_OK;

    const char* device_path = device->config.device_path[0]
        ? device->config.device_path
        : "/dev/dri/renderD129";
    const int drm_render_node_num = parse_drm_render_node_num(device_path);

    if (device->config.external_mfx_session) {
        device->session = reinterpret_cast<mfxSession>(device->config.external_mfx_session);
        device->owns_session = false;
    } else {
        device->loader = MFXLoad();
        if (!device->loader) {
            set_error(&device->last_error, "MFXLoad failed");
            return MFX50_ERR_DEVICE;
        }
        MFX_ADD_PROPERTY_U32(device->loader, "mfxImplDescription.Impl", MFX_IMPL_TYPE_HARDWARE);
        MFX_ADD_PROPERTY_U32(device->loader, "mfxImplDescription.AccelerationMode", MFX_ACCEL_MODE_VIA_VAAPI);
        if (drm_render_node_num > 0) {
            MFX_ADD_PROPERTY_U32(device->loader,
                                 "mfxExtendedDeviceId.DRMRenderNodeNum",
                                 static_cast<mfxU32>(drm_render_node_num));
        }
        mfxStatus st = MFXCreateSession(device->loader, 0, &device->session);
        if (st < MFX_ERR_NONE || !device->session) {
            std::string msg = "MFXCreateSession failed for ";
            msg += device_path;
            if (drm_render_node_num > 0) {
                msg += " (DRMRenderNodeNum=" + std::to_string(drm_render_node_num) + ")";
            }
            msg += ": " + mfx_status_name(st);
            set_error(&device->last_error, msg.c_str());
            cleanup_device_runtime(device);
            return MFX50_ERR_DEVICE;
        }
        device->owns_session = true;
    }

    if (device->config.external_va_display) {
        device->va_display = reinterpret_cast<VADisplay>(device->config.external_va_display);
        device->owns_va_display = device->config.take_external_ownership ? true : false;
    } else {
        device->fd = open(device_path, O_RDWR);
        if (device->fd < 0) {
            std::string msg = "failed to open VAAPI device: ";
            msg += device_path;
            set_error(&device->last_error, msg.c_str());
            cleanup_device_runtime(device);
            return MFX50_ERR_DEVICE;
        }
        device->va_display = vaGetDisplayDRM(device->fd);
        if (!device->va_display) {
            set_error(&device->last_error, "vaGetDisplayDRM failed");
            cleanup_device_runtime(device);
            return MFX50_ERR_DEVICE;
        }
        int major = 0;
        int minor = 0;
        VAStatus vst = vaInitialize(device->va_display, &major, &minor);
        if (vst != VA_STATUS_SUCCESS) {
            std::string msg = std::string("vaInitialize failed: ") + vaErrorStr(vst);
            set_error(&device->last_error, msg.c_str());
            cleanup_device_runtime(device);
            return MFX50_ERR_DEVICE;
        }
        device->va_initialized = true;
        device->owns_va_display = true;
    }

    if (device->va_display && device->session) {
        mfxStatus st = MFXVideoCORE_SetHandle(device->session,
                                              MFX_HANDLE_VA_DISPLAY,
                                              device->va_display);
        if (st < MFX_ERR_NONE) {
            set_error(&device->last_error, ("MFXVideoCORE_SetHandle failed: " + mfx_status_name(st)).c_str());
            cleanup_device_runtime(device);
            return MFX50_ERR_DEVICE;
        }
    }

    device->runtime_ready = true;
    return MFX50_OK;
}

MFX50_Status map_mfx_error(mfxStatus st) {
    if (st >= MFX_ERR_NONE) return MFX50_OK;
    if (st == MFX_ERR_UNSUPPORTED || st == MFX_ERR_NOT_IMPLEMENTED ||
        st == MFX_ERR_INVALID_VIDEO_PARAM || st == MFX_ERR_INCOMPATIBLE_VIDEO_PARAM) {
        return MFX50_ERR_UNSUPPORTED;
    }
    return MFX50_ERR_DEVICE;
}

mfxU32 codec_to_mfx(MFX50_Codec codec) {
    switch (codec) {
        case MFX50_CODEC_H264: return MFX_CODEC_AVC;
        case MFX50_CODEC_HEVC: return MFX_CODEC_HEVC;
        default: return 0;
    }
}

MFX50_Codec detect_annexb_codec(const uint8_t* data, size_t size) {
    if (!data || size < 5) return MFX50_CODEC_UNKNOWN;
    MFX50_Codec candidate = MFX50_CODEC_UNKNOWN;
    for (size_t i = 0; i + 4 < size; ++i) {
        size_t off = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            off = i + 3;
        } else if (i + 4 < size && data[i] == 0 && data[i + 1] == 0 &&
                   data[i + 2] == 0 && data[i + 3] == 1) {
            off = i + 4;
        } else {
            continue;
        }
        if (off >= size) continue;

        const uint8_t h264_type = static_cast<uint8_t>(data[off] & 0x1f);
        if (h264_type == 7 || h264_type == 8) return MFX50_CODEC_H264;
        if ((h264_type == 1 || h264_type == 5) && candidate == MFX50_CODEC_UNKNOWN) {
            candidate = MFX50_CODEC_H264;
        }

        if (off + 1 >= size || (data[off + 1] & 0x07) == 0 ||
            (data[off] & 0x01) != 0) {
            continue;
        }
        const uint8_t hevc_type = static_cast<uint8_t>((data[off] >> 1) & 0x3f);
        if (hevc_type == 32 || hevc_type == 33 || hevc_type == 34) return MFX50_CODEC_HEVC;
        if ((hevc_type == 19 || hevc_type == 20) && candidate == MFX50_CODEC_UNKNOWN) {
            candidate = MFX50_CODEC_HEVC;
        }
    }
    return candidate;
}

MFX50_PixelFormat pixfmt_from_fourcc(mfxU32 fourcc) {
    switch (fourcc) {
        case MFX_FOURCC_NV12: return MFX50_PIXFMT_NV12;
        case MFX_FOURCC_P010: return MFX50_PIXFMT_P010;
        default: return MFX50_PIXFMT_UNKNOWN;
    }
}

void reset_decoder_bitstream(MFX50_Decoder* decoder) {
    if (!decoder) return;
    mfxU32 offset = decoder->input_bs.DataOffset;
    mfxU32 length = decoder->input_bs.DataLength;
    std::memset(&decoder->input_bs, 0, sizeof(decoder->input_bs));
    decoder->input_bs.Data = decoder->bitstream_storage.data();
    decoder->input_bs.MaxLength = static_cast<mfxU32>(decoder->bitstream_storage.size());
    decoder->input_bs.DataOffset = offset;
    decoder->input_bs.DataLength = length;
}

void compact_decoder_input(MFX50_Decoder* decoder) {
    if (!decoder) return;
    if (decoder->input_bs.DataOffset == 0) return;
    if (decoder->input_bs.DataLength > 0) {
        std::memmove(decoder->bitstream_storage.data(),
                     decoder->bitstream_storage.data() + decoder->input_bs.DataOffset,
                     decoder->input_bs.DataLength);
    }
    decoder->input_bs.DataOffset = 0;
    reset_decoder_bitstream(decoder);
}

bool ensure_decoder_input_capacity(MFX50_Decoder* decoder, size_t additional) {
    if (!decoder) return false;
    compact_decoder_input(decoder);
    const size_t required = static_cast<size_t>(decoder->input_bs.DataOffset) +
                            static_cast<size_t>(decoder->input_bs.DataLength) +
                            additional;
    if (required <= decoder->bitstream_storage.size()) return true;

    size_t capacity = decoder->bitstream_storage.empty()
        ? 1024 * 1024
        : decoder->bitstream_storage.size();
    while (capacity < required) capacity *= 2;
    decoder->bitstream_storage.resize(capacity);
    reset_decoder_bitstream(decoder);
    return true;
}

void append_decoder_input(MFX50_Decoder* decoder, const uint8_t* data, size_t size) {
    if (!decoder || !data || size == 0) return;
    ensure_decoder_input_capacity(decoder, size);
    uint8_t* dst = decoder->bitstream_storage.data() +
                   decoder->input_bs.DataOffset +
                   decoder->input_bs.DataLength;
    std::memcpy(dst, data, size);
    decoder->input_bs.DataLength += static_cast<mfxU32>(size);
    reset_decoder_bitstream(decoder);
}

MFX50_Status init_decoder_if_needed(MFX50_Decoder* decoder) {
    if (!decoder) return MFX50_ERR_INVALID_PARAM;
    if (decoder->decoder_ready) return MFX50_OK;
    if (decoder->input_bs.DataLength == 0) return MFX50_ERR_AGAIN;

    const mfxU32 codec_id = codec_to_mfx(decoder->active_codec);
    if (!codec_id) {
        set_error(&decoder->last_error, "decoder input codec must be H.264 or H.265");
        return MFX50_ERR_UNSUPPORTED;
    }

    std::memset(&decoder->par, 0, sizeof(decoder->par));
    decoder->par.AsyncDepth = static_cast<mfxU16>(std::max(1, decoder->config.async_depth));
    decoder->par.mfx.CodecId = codec_id;

    const mfxU32 offset_before = decoder->input_bs.DataOffset;
    const mfxU32 length_before = decoder->input_bs.DataLength;
    mfxStatus st = MFXVideoDECODE_DecodeHeader(decoder->device->session,
                                               &decoder->input_bs,
                                               &decoder->par);
    decoder->input_bs.DataOffset = offset_before;
    decoder->input_bs.DataLength = length_before;
    reset_decoder_bitstream(decoder);
    if (st == MFX_ERR_MORE_DATA) {
        set_error(&decoder->last_error, "decoder needs more header data");
        return MFX50_ERR_AGAIN;
    }
    if (st < MFX_ERR_NONE) {
        std::string msg = "MFXVideoDECODE_DecodeHeader failed: " + mfx_status_name(st);
        set_error(&decoder->last_error, msg.c_str());
        return map_mfx_error(st);
    }

    decoder->par.AsyncDepth = static_cast<mfxU16>(std::max(1, decoder->config.async_depth));
    decoder->par.IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;
    if (decoder->config.fps_num > 0 && decoder->config.fps_den > 0) {
        decoder->par.mfx.FrameInfo.FrameRateExtN = static_cast<mfxU32>(decoder->config.fps_num);
        decoder->par.mfx.FrameInfo.FrameRateExtD = static_cast<mfxU32>(decoder->config.fps_den);
    }
    if (decoder->par.mfx.FrameInfo.PicStruct == 0) {
        decoder->par.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    }

    mfxVideoParam queried = decoder->par;
    st = MFXVideoDECODE_Query(decoder->device->session, &decoder->par, &queried);
    if (st < MFX_ERR_NONE) {
        std::string msg = "MFXVideoDECODE_Query failed: " + mfx_status_name(st);
        set_error(&decoder->last_error, msg.c_str());
        return map_mfx_error(st);
    }
    decoder->par = queried;
    decoder->par.IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;

    st = MFXVideoDECODE_Init(decoder->device->session, &decoder->par);
    if (st < MFX_ERR_NONE) {
        std::string msg = "MFXVideoDECODE_Init failed: " + mfx_status_name(st);
        set_error(&decoder->last_error, msg.c_str());
        return map_mfx_error(st);
    }

    decoder->decoder_ready = true;
    return MFX50_OK;
}

MFX50_Status surface_ref_add(void* opaque) {
    DecodedSurfaceRef* ref = reinterpret_cast<DecodedSurfaceRef*>(opaque);
    if (!ref || !ref->surface) return MFX50_ERR_INVALID_PARAM;
    ref->refs.fetch_add(1, std::memory_order_relaxed);
    return MFX50_OK;
}

void surface_ref_release(void* opaque) {
    DecodedSurfaceRef* ref = reinterpret_cast<DecodedSurfaceRef*>(opaque);
    if (!ref) return;
    if (ref->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    if (ref->surface && ref->surface->FrameInterface) {
        ref->surface->FrameInterface->Release(ref->surface);
    }
    delete ref;
}

void release_decode_surface(mfxFrameSurface1* surface) {
    if (surface && surface->FrameInterface) {
        surface->FrameInterface->Release(surface);
    }
}

bool decoder_has_active_ops(const MFX50_Decoder* decoder) {
    if (!decoder) return false;
    for (const auto& op : decoder->ops) {
        if (op.in_use) return true;
    }
    return false;
}

MFX50_Decoder::DecodeOp* find_free_decode_op(MFX50_Decoder* decoder) {
    if (!decoder) return nullptr;
    for (auto& op : decoder->ops) {
        if (!op.in_use) return &op;
    }
    return nullptr;
}

void queue_decoded_surface(MFX50_Decoder* decoder, MFX50_Decoder::DecodeOp& op) {
    if (!decoder) return;
    if (!op.surface) {
        op.sync = nullptr;
        op.in_use = false;
        op.pts = 0;
        op.dts = 0;
        op.user_opaque = nullptr;
        return;
    }
    DecodedSurfaceItem item;
    item.surface = op.surface;
    item.pts = op.pts;
    item.dts = op.dts;
    item.user_opaque = op.user_opaque;
    decoder->output_queue.push_back(item);
    decoder->output_surfaces++;
    op.surface = nullptr;
    op.sync = nullptr;
    op.in_use = false;
    op.pts = 0;
    op.dts = 0;
    op.user_opaque = nullptr;
}

MFX50_Status sync_decoder_ops(MFX50_Decoder* decoder, bool blocking, bool* progressed) {
    if (progressed) *progressed = false;
    if (!decoder || !decoder->device || !decoder->device->session) return MFX50_ERR_INVALID_PARAM;

    for (auto& op : decoder->ops) {
        if (!op.in_use) continue;
        if (!op.sync) {
            queue_decoded_surface(decoder, op);
            if (progressed) *progressed = true;
            continue;
        }
        mfxStatus st = MFXVideoCORE_SyncOperation(decoder->device->session,
                                                  op.sync,
                                                  blocking ? 1000U : 0U);
        if (st == MFX_ERR_NONE) {
            op.sync = nullptr;
            queue_decoded_surface(decoder, op);
            if (progressed) *progressed = true;
            continue;
        }
        if (st == MFX_WRN_IN_EXECUTION) {
            continue;
        }
        release_decode_surface(op.surface);
        op.surface = nullptr;
        op.sync = nullptr;
        op.in_use = false;
        std::string msg = "MFXVideoCORE_SyncOperation(decode) failed: " + mfx_status_name(st);
        set_error(&decoder->last_error, msg.c_str());
        return map_mfx_error(st);
    }
    return MFX50_OK;
}

MFX50_Status submit_decoder_step(MFX50_Decoder* decoder,
                                 bool draining,
                                 int64_t pts,
                                 int64_t dts,
                                 void* user_opaque,
                                 bool* need_more_data,
                                 bool* submitted) {
    if (need_more_data) *need_more_data = false;
    if (submitted) *submitted = false;
    MFX50_Decoder::DecodeOp* op = find_free_decode_op(decoder);
    if (!op) return MFX50_OK;

    op->in_use = true;
    op->surface = nullptr;
    op->sync = nullptr;
    op->pts = pts;
    op->dts = dts;
    op->user_opaque = user_opaque;

    mfxBitstream* bs = draining ? nullptr : &decoder->input_bs;
    for (;;) {
        mfxStatus st = MFXVideoDECODE_DecodeFrameAsync(decoder->device->session,
                                                       bs,
                                                       nullptr,
                                                       &op->surface,
                                                       &op->sync);
        if (st == MFX_WRN_DEVICE_BUSY) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (st == MFX_ERR_MORE_SURFACE || st == MFX_WRN_ALLOC_TIMEOUT_EXPIRED) {
            op->in_use = false;
            op->surface = nullptr;
            op->sync = nullptr;
            return MFX50_OK;
        }
        if (st == MFX_ERR_MORE_DATA) {
            op->in_use = false;
            op->surface = nullptr;
            op->sync = nullptr;
            if (!draining) compact_decoder_input(decoder);
            if (need_more_data) *need_more_data = true;
            return MFX50_OK;
        }
        if (st < MFX_ERR_NONE) {
            op->in_use = false;
            release_decode_surface(op->surface);
            op->surface = nullptr;
            op->sync = nullptr;
            std::string msg = "MFXVideoDECODE_DecodeFrameAsync failed: " + mfx_status_name(st);
            set_error(&decoder->last_error, msg.c_str());
            return map_mfx_error(st);
        }
        if (!op->surface) {
            op->in_use = false;
            if (need_more_data) *need_more_data = true;
            return MFX50_OK;
        }
        if (submitted) *submitted = true;
        return MFX50_OK;
    }
}

MFX50_Status drive_decoder(MFX50_Decoder* decoder,
                           bool draining,
                           int64_t pts,
                           int64_t dts,
                           void* user_opaque,
                           bool blocking) {
    if (!decoder) return MFX50_ERR_INVALID_PARAM;
    for (;;) {
        bool progressed = false;
        MFX50_Status st = sync_decoder_ops(decoder, blocking, &progressed);
        if (st != MFX50_OK) return st;
        if (decoder->output_queue.size() >= static_cast<size_t>(decoder->config.max_output_surfaces)) {
            return MFX50_OK;
        }

        bool need_more_data = false;
        bool submitted = false;
        st = submit_decoder_step(decoder,
                                 draining,
                                 pts,
                                 dts,
                                 user_opaque,
                                 &need_more_data,
                                 &submitted);
        if (st != MFX50_OK) return st;
        if (need_more_data) {
            if (draining) decoder->drained = true;
            return MFX50_OK;
        }
        if (!submitted) {
            if (progressed) continue;
            return MFX50_OK;
        }
        if (!draining && decoder->input_bs.DataLength == 0) {
            compact_decoder_input(decoder);
            return MFX50_OK;
        }
        if (!blocking && !find_free_decode_op(decoder)) return MFX50_OK;
    }
}

MFX50_Status advance_decoder_flush(MFX50_Decoder* decoder, bool blocking) {
    if (!decoder) return MFX50_ERR_INVALID_PARAM;
    if (decoder->drained) return MFX50_OK;
    if (!decoder->decoder_ready) {
        decoder->drained = true;
        return MFX50_OK;
    }
    if (decoder->input_bs.DataLength > 0) {
        return drive_decoder(decoder,
                             false,
                             decoder->last_pts,
                             decoder->last_dts,
                             decoder->last_user_opaque,
                             blocking);
    }
    return drive_decoder(decoder, true, -1, -1, nullptr, blocking);
}

MFX50_Status init_encoder_if_needed(MFX50_Encoder* encoder,
                                    int visible_width_hint,
                                    int visible_height_hint,
                                    int backing_width_hint,
                                    int backing_height_hint,
                                    int io_pattern,
                                    const mfxFrameInfo* source_info) {
    if (!encoder) return MFX50_ERR_INVALID_PARAM;
    if (encoder->encoder_ready) {
        if (encoder->io_pattern != io_pattern) {
            set_error(&encoder->last_error, "encoder input memory type changed after init");
            return MFX50_ERR_BAD_STATE;
        }
        return MFX50_OK;
    }

    if (encoder->config.output_codec != MFX50_CODEC_HEVC &&
        encoder->config.output_codec != MFX50_CODEC_H265) {
        set_error(&encoder->last_error, "only HEVC/H.265 output is implemented");
        return MFX50_ERR_UNSUPPORTED;
    }
    if (encoder->config.input_format != MFX50_PIXFMT_NV12) {
        set_error(&encoder->last_error, "only NV12 input is implemented");
        return MFX50_ERR_UNSUPPORTED;
    }

    int width = encoder->config.width > 0 ? encoder->config.width : visible_width_hint;
    int height = encoder->config.height > 0 ? encoder->config.height : visible_height_hint;
    if ((width <= 0 || height <= 0) && source_info) {
        width = source_info->CropW > 0 ? source_info->CropW : source_info->Width;
        height = source_info->CropH > 0 ? source_info->CropH : source_info->Height;
    }
    if (width <= 0 || height <= 0) {
        set_error(&encoder->last_error, "encoder width/height must be > 0");
        return MFX50_ERR_INVALID_PARAM;
    }
    const int aligned_width = align16(width);
    const int aligned_height = align16(height);
    const int source_width = source_info ? source_info->Width : 0;
    const int source_height = source_info ? source_info->Height : 0;
    const int external_backing_width = positive_max(backing_width_hint, source_width);
    const int external_backing_height = positive_max(backing_height_hint, source_height);
    const int backing_width = external_backing_width > 0 ? external_backing_width : aligned_width;
    const int backing_height = external_backing_height > 0 ? external_backing_height : aligned_height;
    if (backing_width < width || backing_height < height) {
        std::string msg = "encoder surface backing is smaller than visible crop"
            " (visible=" + std::to_string(width) + "x" + std::to_string(height) +
            ", backing=" + std::to_string(backing_width) + "x" +
            std::to_string(backing_height) + ")";
        set_error(&encoder->last_error, msg.c_str());
        return MFX50_ERR_INVALID_PARAM;
    }

    MFX50_Status rt = ensure_device_runtime(encoder->device);
    if (rt != MFX50_OK) {
        encoder->last_error = encoder->device ? encoder->device->last_error : "device runtime unavailable";
        return rt;
    }

    std::memset(&encoder->par, 0, sizeof(encoder->par));
    encoder->par.AsyncDepth = static_cast<mfxU16>(std::max(1, encoder->config.async_depth));
    encoder->par.IOPattern = static_cast<mfxU16>(io_pattern);
    encoder->par.mfx.CodecId = MFX_CODEC_HEVC;
    encoder->par.mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED;
    encoder->par.mfx.RateControlMethod = MFX_RATECONTROL_CQP;
    encoder->par.mfx.QPI = static_cast<mfxU16>(encoder->config.qpi > 0 ? encoder->config.qpi : 32);
    encoder->par.mfx.QPP = static_cast<mfxU16>(encoder->config.qpp > 0 ? encoder->config.qpp : encoder->par.mfx.QPI);
    encoder->par.mfx.QPB = static_cast<mfxU16>(encoder->config.qpb > 0 ? encoder->config.qpb : encoder->par.mfx.QPP);
    encoder->par.mfx.GopPicSize = static_cast<mfxU16>(encoder->config.gop_size > 0 ? encoder->config.gop_size : 60);
    encoder->par.mfx.GopRefDist = static_cast<mfxU16>(encoder->config.b_frames > 0 ? encoder->config.b_frames + 1 : 1);
    if (source_info) {
        encoder->par.mfx.FrameInfo = *source_info;
    }
    encoder->par.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    encoder->par.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    encoder->par.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    encoder->par.mfx.FrameInfo.FrameRateExtN = static_cast<mfxU32>(encoder->config.fps_num > 0 ? encoder->config.fps_num : 30);
    encoder->par.mfx.FrameInfo.FrameRateExtD = static_cast<mfxU32>(encoder->config.fps_den > 0 ? encoder->config.fps_den : 1);
    encoder->par.mfx.FrameInfo.Width = static_cast<mfxU16>(backing_width);
    encoder->par.mfx.FrameInfo.Height = static_cast<mfxU16>(backing_height);
    encoder->par.mfx.FrameInfo.CropW = static_cast<mfxU16>(width);
    encoder->par.mfx.FrameInfo.CropH = static_cast<mfxU16>(height);

    mfxVideoParam queried = encoder->par;
    mfxStatus st = MFXVideoENCODE_Query(encoder->device->session, &encoder->par, &queried);
    if (st < MFX_ERR_NONE) {
        std::string msg = "MFXVideoENCODE_Query failed: " + mfx_status_name(st) +
            " (visible=" + std::to_string(width) + "x" + std::to_string(height) +
            ", backing=" + std::to_string(backing_width) + "x" +
            std::to_string(backing_height) + ", source_info=" +
            std::to_string(source_width) + "x" + std::to_string(source_height) + ")";
        set_error(&encoder->last_error, msg.c_str());
        return map_mfx_error(st);
    }
    encoder->par = queried;
    encoder->par.IOPattern = static_cast<mfxU16>(io_pattern);
    encoder->par.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    encoder->par.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    encoder->par.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    encoder->par.mfx.FrameInfo.Width = static_cast<mfxU16>(backing_width);
    encoder->par.mfx.FrameInfo.Height = static_cast<mfxU16>(backing_height);
    encoder->par.mfx.FrameInfo.CropX = static_cast<mfxU16>(source_info ? source_info->CropX : 0);
    encoder->par.mfx.FrameInfo.CropY = static_cast<mfxU16>(source_info ? source_info->CropY : 0);
    encoder->par.mfx.FrameInfo.CropW = static_cast<mfxU16>(width);
    encoder->par.mfx.FrameInfo.CropH = static_cast<mfxU16>(height);
    st = MFXVideoENCODE_Init(encoder->device->session, &encoder->par);
    if (st < MFX_ERR_NONE) {
        std::string msg = "MFXVideoENCODE_Init failed: " + mfx_status_name(st) +
            " (visible=" + std::to_string(width) + "x" + std::to_string(height) +
            ", backing=" + std::to_string(backing_width) + "x" +
            std::to_string(backing_height) + ", source_info=" +
            std::to_string(source_width) + "x" + std::to_string(source_height) + ")";
        set_error(&encoder->last_error, msg.c_str());
        return map_mfx_error(st);
    }

    encoder->bitstream_storage.assign(4 * 1024 * 1024, 0);
    encoder->encoder_ready = true;
    encoder->io_pattern = io_pattern;
    encoder->width = width;
    encoder->height = height;
    return MFX50_OK;
}

void enqueue_bitstream(MFX50_Encoder* encoder,
                       const mfxBitstream& bs,
                       int64_t pts,
                       void* user_opaque) {
    if (!encoder || bs.DataLength == 0) return;
    EncodedBuffer out;
    out.codec = MFX50_CODEC_HEVC;
    out.data.assign(bs.Data + bs.DataOffset, bs.Data + bs.DataOffset + bs.DataLength);
    out.pts = pts;
    out.dts = static_cast<int64_t>(encoder->output_packets);
    out.flags = hevc_contains_idr(out.data.data(), out.data.size()) ? MFX50_PACKET_FLAG_KEYFRAME : 0;
    out.user_opaque = user_opaque;
    encoder->output_packets++;
    encoder->output_queue.push_back(std::move(out));
}

MFX50_Status submit_encoder_surface(MFX50_Encoder* encoder,
                                    mfxFrameSurface1* surface,
                                    int64_t pts,
                                    void* user_opaque,
                                    bool* more_data) {
    if (more_data) *more_data = false;
    for (;;) {
        mfxBitstream bs{};
        reset_bitstream(&bs, &encoder->bitstream_storage);
        mfxSyncPoint syncp = nullptr;
        mfxStatus st = MFXVideoENCODE_EncodeFrameAsync(
            encoder->device->session,
            nullptr,
            surface,
            &bs,
            &syncp);
        if (st == MFX_WRN_DEVICE_BUSY) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (st == MFX_ERR_MORE_BITSTREAM || st == MFX_ERR_NOT_ENOUGH_BUFFER) {
            if (encoder->bitstream_storage.size() >= 256u * 1024u * 1024u) {
                std::string msg = "MFXVideoENCODE_EncodeFrameAsync output bitstream buffer exceeded 256MB";
                set_error(&encoder->last_error, msg.c_str());
                return MFX50_ERR_NO_MEMORY;
            }
            encoder->bitstream_storage.resize(encoder->bitstream_storage.size() * 2);
            continue;
        }
        if (st == MFX_ERR_MORE_DATA) {
            if (more_data) *more_data = true;
            return MFX50_OK;
        }
        if (st < MFX_ERR_NONE) {
            std::string msg = "MFXVideoENCODE_EncodeFrameAsync failed: " + mfx_status_name(st);
            set_error(&encoder->last_error, msg.c_str());
            return map_mfx_error(st);
        }
        if (syncp) {
            st = MFXVideoCORE_SyncOperation(encoder->device->session, syncp, 60000);
            if (st < MFX_ERR_NONE) {
                std::string msg = "MFXVideoCORE_SyncOperation failed: " + mfx_status_name(st);
                set_error(&encoder->last_error, msg.c_str());
                return map_mfx_error(st);
            }
            enqueue_bitstream(encoder, bs, pts, user_opaque);
        }
        return MFX50_OK;
    }
}

void release_packet_buffer(void* opaque) {
    delete reinterpret_cast<EncodedBuffer*>(opaque);
}

} // namespace

extern "C" const char* mfx50_status_string(MFX50_Status status) {
    switch (status) {
        case MFX50_OK: return "MFX50_OK";
        case MFX50_ERR_INVALID_PARAM: return "MFX50_ERR_INVALID_PARAM";
        case MFX50_ERR_UNSUPPORTED: return "MFX50_ERR_UNSUPPORTED";
        case MFX50_ERR_NOT_READY: return "MFX50_ERR_NOT_READY";
        case MFX50_ERR_NO_MEMORY: return "MFX50_ERR_NO_MEMORY";
        case MFX50_ERR_BAD_STATE: return "MFX50_ERR_BAD_STATE";
        case MFX50_ERR_VERSION_MISMATCH: return "MFX50_ERR_VERSION_MISMATCH";
        case MFX50_ERR_BUFFER_TOO_SMALL: return "MFX50_ERR_BUFFER_TOO_SMALL";
        case MFX50_ERR_AGAIN: return "MFX50_ERR_AGAIN";
        case MFX50_ERR_DEVICE: return "MFX50_ERR_DEVICE";
        case MFX50_ERR_EOS: return "MFX50_ERR_EOS";
        case MFX50_ERR_NOT_IMPLEMENTED: return "MFX50_ERR_NOT_IMPLEMENTED";
        default: return "MFX50_ERR_UNKNOWN";
    }
}

extern "C" MFX50_Status mfx50_device_default_config(MFX50_DeviceConfig* config) {
    if (!config) return MFX50_ERR_INVALID_PARAM;
    init_struct(config);
    config->device_index = 1;
    std::snprintf(config->device_path, sizeof(config->device_path), "%s", "/dev/dri/renderD129");
    config->interop_type = MFX50_DEVICE_INTEROP_AUTO;
    config->require_zero_copy = 1;
    return MFX50_OK;
}

extern "C" MFX50_Status mfx50_device_create(const MFX50_DeviceConfig* config,
                                            MFX50_Device** out_device) {
    if (!out_device) {
        set_error(nullptr, "out_device is null");
        return MFX50_ERR_INVALID_PARAM;
    }
    *out_device = nullptr;

    auto defaults = [](MFX50_DeviceConfig* c) { mfx50_device_default_config(c); };
    MFX50_DeviceConfig cfg = copy_config_or_default(config, defaults);
    if (cfg.api_version != MFX50_DEVICE_API_VERSION) {
        set_error(nullptr, "device config api_version mismatch");
        return MFX50_ERR_VERSION_MISMATCH;
    }

    MFX50_Device* device = new (std::nothrow) MFX50_Device();
    if (!device) {
        set_error(nullptr, "failed to allocate MFX50_Device");
        return MFX50_ERR_NO_MEMORY;
    }
    device->config = cfg;
    *out_device = device;
    return MFX50_OK;
}

extern "C" const char* mfx50_device_get_last_error(MFX50_Device* device) {
    return device ? device->last_error.c_str() : g_last_error.c_str();
}

extern "C" void mfx50_device_destroy(MFX50_Device* device) {
    cleanup_device_runtime(device);
    delete device;
}

extern "C" MFX50_Status mfx50_surface_add_ref(const MFX50_Surface* surface) {
    if (!surface) return MFX50_ERR_INVALID_PARAM;
    if (!surface->add_ref) return MFX50_ERR_UNSUPPORTED;
    return surface->add_ref(surface->ref_opaque);
}

extern "C" void mfx50_surface_release(MFX50_Surface* surface) {
    if (!surface) return;
    if (surface->release) surface->release(surface->ref_opaque);
    std::memset(surface, 0, sizeof(*surface));
}

extern "C" void mfx50_packet_release(MFX50_Packet* packet) {
    if (!packet) return;
    if (packet->release) packet->release(packet->release_opaque);
    std::memset(packet, 0, sizeof(*packet));
}

extern "C" MFX50_Status mfx50_decoder_default_config(MFX50_DecoderConfig* config) {
    if (!config) return MFX50_ERR_INVALID_PARAM;
    init_struct(config);
    config->input_codec = MFX50_CODEC_UNKNOWN;
    config->fps_num = 30;
    config->fps_den = 1;
    config->annexb_input = 1;
    config->low_latency = 1;
    config->async_depth = 2;
    config->max_output_surfaces = 16;
    return MFX50_OK;
}

extern "C" MFX50_Status mfx50_decoder_create(MFX50_Device* device,
                                             const MFX50_DecoderConfig* config,
                                             MFX50_Decoder** out_decoder) {
    if (!device || !out_decoder) {
        set_error(device ? &device->last_error : nullptr, "device or out_decoder is null");
        return MFX50_ERR_INVALID_PARAM;
    }
    *out_decoder = nullptr;
    auto defaults = [](MFX50_DecoderConfig* c) { mfx50_decoder_default_config(c); };
    MFX50_DecoderConfig cfg = copy_config_or_default(config, defaults);
    if (cfg.input_codec != MFX50_CODEC_UNKNOWN &&
        cfg.input_codec != MFX50_CODEC_H264 &&
        cfg.input_codec != MFX50_CODEC_HEVC) {
        set_error(&device->last_error, "decoder input codec must be H.264, H.265, or UNKNOWN/AUTO");
        return MFX50_ERR_UNSUPPORTED;
    }
    if (cfg.async_depth <= 0) cfg.async_depth = 2;
    if (cfg.max_output_surfaces <= 0) cfg.max_output_surfaces = 16;
    if (cfg.max_output_surfaces < cfg.async_depth) cfg.max_output_surfaces = cfg.async_depth;

    MFX50_Status rt = ensure_device_runtime(device);
    if (rt != MFX50_OK) return rt;

    MFX50_Decoder* decoder = new (std::nothrow) MFX50_Decoder();
    if (!decoder) {
        set_error(&device->last_error, "failed to allocate MFX50_Decoder");
        return MFX50_ERR_NO_MEMORY;
    }
    decoder->device = device;
    decoder->config = cfg;
    decoder->active_codec = cfg.input_codec;
    decoder->bitstream_storage.assign(4 * 1024 * 1024, 0);
    reset_decoder_bitstream(decoder);
    decoder->ops.resize(static_cast<size_t>(cfg.async_depth));
    *out_decoder = decoder;
    return MFX50_OK;
}

extern "C" MFX50_Status mfx50_decoder_push_packet(MFX50_Decoder* decoder,
                                                  const MFX50_Packet* packet) {
    if (!decoder || !packet) return MFX50_ERR_INVALID_PARAM;
    if (decoder->flushing) {
        set_error(&decoder->last_error, "cannot push packet after decoder flush");
        return MFX50_ERR_BAD_STATE;
    }
    if (decoder->drained) {
        set_error(&decoder->last_error, "cannot push packet after decoder drain");
        return MFX50_ERR_BAD_STATE;
    }
    if (packet->flags & MFX50_PACKET_FLAG_EOS) {
        return mfx50_decoder_flush(decoder);
    }
    if (!packet->data || packet->data_size == 0) {
        set_error(&decoder->last_error, "decoder packet data is empty");
        return MFX50_ERR_INVALID_PARAM;
    }
    MFX50_Codec packet_codec = packet->codec;
    if (packet_codec != MFX50_CODEC_H264 && packet_codec != MFX50_CODEC_HEVC) {
        packet_codec = detect_annexb_codec(packet->data, packet->data_size);
    }
    if (decoder->config.input_codec == MFX50_CODEC_UNKNOWN) {
        if (packet_codec == MFX50_CODEC_H264 || packet_codec == MFX50_CODEC_HEVC) {
            decoder->active_codec = packet_codec;
        }
    }
    if ((packet_codec == MFX50_CODEC_H264 || packet_codec == MFX50_CODEC_HEVC) &&
        decoder->active_codec != MFX50_CODEC_UNKNOWN &&
        packet_codec != decoder->active_codec) {
        set_error(&decoder->last_error, "packet codec does not match decoder input codec");
        return MFX50_ERR_INVALID_PARAM;
    }
    if (decoder->active_codec == MFX50_CODEC_UNKNOWN) {
        set_error(&decoder->last_error, "decoder input codec is unknown");
        return MFX50_ERR_INVALID_PARAM;
    }

    append_decoder_input(decoder, packet->data, packet->data_size);
    decoder->input_packets++;
    decoder->last_pts = packet->pts;
    decoder->last_dts = packet->dts;
    decoder->last_user_opaque = packet->user_opaque;

    MFX50_Status st = init_decoder_if_needed(decoder);
    if (st == MFX50_ERR_AGAIN) return MFX50_OK;
    if (st != MFX50_OK) return st;

    return drive_decoder(decoder,
                         false,
                         packet->pts,
                         packet->dts,
                         packet->user_opaque,
                         false);
}

extern "C" MFX50_Status mfx50_decoder_poll_surface(MFX50_Decoder* decoder,
                                                   MFX50_Surface* out_surface) {
    if (!decoder || !out_surface) return MFX50_ERR_INVALID_PARAM;
    if (decoder->decoder_ready) {
        MFX50_Status st = sync_decoder_ops(decoder, false, nullptr);
        if (st != MFX50_OK) return st;
        if (!decoder->drained &&
            decoder->output_queue.empty() &&
            decoder->input_bs.DataLength > 0) {
            st = drive_decoder(decoder,
                               false,
                               decoder->last_pts,
                               decoder->last_dts,
                               decoder->last_user_opaque,
                               false);
            if (st != MFX50_OK) return st;
            st = sync_decoder_ops(decoder, false, nullptr);
            if (st != MFX50_OK) return st;
        }
        if (decoder->flushing && !decoder->drained && decoder->output_queue.empty()) {
            st = advance_decoder_flush(decoder, false);
            if (st != MFX50_OK) return st;
            st = sync_decoder_ops(decoder, false, nullptr);
            if (st != MFX50_OK) return st;
        }
    }
    if (decoder->output_queue.empty()) {
        return (decoder->drained && !decoder_has_active_ops(decoder))
            ? MFX50_ERR_EOS
            : MFX50_ERR_AGAIN;
    }

    DecodedSurfaceItem item = decoder->output_queue.front();
    decoder->output_queue.pop_front();
    if (!item.surface) return MFX50_ERR_AGAIN;

    DecodedSurfaceRef* ref = new (std::nothrow) DecodedSurfaceRef();
    if (!ref) {
        release_decode_surface(item.surface);
        return MFX50_ERR_NO_MEMORY;
    }
    ref->surface = item.surface;

    const mfxFrameInfo& info = item.surface->Info;
    std::memset(out_surface, 0, sizeof(*out_surface));
    out_surface->struct_size = sizeof(*out_surface);
    out_surface->api_version = MFX50_DEVICE_API_VERSION;
    out_surface->type = MFX50_SURFACE_ONEVPL;
    out_surface->pixel_format = pixfmt_from_fourcc(info.FourCC);
    out_surface->width = info.CropW > 0 ? info.CropW : info.Width;
    out_surface->height = info.CropH > 0 ? info.CropH : info.Height;
    out_surface->crop_x = info.CropX;
    out_surface->crop_y = info.CropY;
    out_surface->crop_w = info.CropW > 0 ? info.CropW : info.Width;
    out_surface->crop_h = info.CropH > 0 ? info.CropH : info.Height;
    out_surface->pts = item.pts;
    out_surface->dts = item.dts;
    out_surface->handle.onevpl.mfx_surface = item.surface;
    out_surface->ref_opaque = ref;
    out_surface->add_ref = surface_ref_add;
    out_surface->release = surface_ref_release;
    out_surface->user_opaque = item.user_opaque;
    return MFX50_OK;
}

extern "C" MFX50_Status mfx50_decoder_flush(MFX50_Decoder* decoder) {
    if (!decoder) return MFX50_ERR_INVALID_PARAM;
    if (decoder->drained) return MFX50_OK;
    decoder->flushing = true;
    if (!decoder->decoder_ready) {
        decoder->drained = true;
        return MFX50_OK;
    }
    return advance_decoder_flush(decoder, true);
}

extern "C" const char* mfx50_decoder_get_last_error(MFX50_Decoder* decoder) {
    return decoder ? decoder->last_error.c_str() : g_last_error.c_str();
}

extern "C" void mfx50_decoder_destroy(MFX50_Decoder* decoder) {
    if (decoder) {
        for (auto& op : decoder->ops) {
            release_decode_surface(op.surface);
            op.surface = nullptr;
            op.sync = nullptr;
            op.in_use = false;
        }
        while (!decoder->output_queue.empty()) {
            release_decode_surface(decoder->output_queue.front().surface);
            decoder->output_queue.pop_front();
        }
        if (decoder->decoder_ready && decoder->device && decoder->device->session) {
            MFXVideoDECODE_Close(decoder->device->session);
        }
    }
    delete decoder;
}

extern "C" MFX50_Status mfx50_encoder_default_config(MFX50_EncoderConfig* config) {
    if (!config) return MFX50_ERR_INVALID_PARAM;
    init_struct(config);
    config->output_codec = MFX50_CODEC_HEVC;
    config->input_format = MFX50_PIXFMT_NV12;
    config->fps_num = 30;
    config->fps_den = 1;
    config->gop_size = 60;
    config->rc_mode = MFX50_RC_CQP;
    config->qpi = 32;
    config->qpp = 32;
    config->qpb = 32;
    config->async_depth = 2;
    config->low_latency = 1;
    config->require_zero_copy = 1;
    return MFX50_OK;
}

extern "C" MFX50_Status mfx50_encoder_get_surface_support(
    MFX50_Device* device,
    MFX50_EncoderSurfaceSupport* out_support) {
    if (!device || !out_support) return MFX50_ERR_INVALID_PARAM;
    std::memset(out_support, 0, sizeof(*out_support));
    out_support->struct_size = sizeof(*out_support);
    out_support->api_version = MFX50_DEVICE_API_VERSION;
    out_support->supports_onevpl_surface = 1;
    out_support->dmabuf_experimental = 1;
    copy_note(out_support->implementation_note,
              sizeof(out_support->implementation_note),
              "oneVPL mfxFrameSurface1* GPU surface encode is implemented; VAAPI VASurfaceID adapter is planned; DMA-BUF is reserved for cross-module interop");
    return MFX50_OK;
}

extern "C" MFX50_Status mfx50_encoder_create(MFX50_Device* device,
                                             const MFX50_EncoderConfig* config,
                                             MFX50_Encoder** out_encoder) {
    if (!device || !out_encoder) {
        set_error(device ? &device->last_error : nullptr, "device or out_encoder is null");
        return MFX50_ERR_INVALID_PARAM;
    }
    *out_encoder = nullptr;
    auto defaults = [](MFX50_EncoderConfig* c) { mfx50_encoder_default_config(c); };
    MFX50_EncoderConfig cfg = copy_config_or_default(config, defaults);

    MFX50_Encoder* encoder = new (std::nothrow) MFX50_Encoder();
    if (!encoder) {
        set_error(&device->last_error, "failed to allocate MFX50_Encoder");
        return MFX50_ERR_NO_MEMORY;
    }
    encoder->device = device;
    encoder->config = cfg;
    *out_encoder = encoder;
    return MFX50_OK;
}

extern "C" MFX50_Status mfx50_encoder_push_surface(MFX50_Encoder* encoder,
                                                   const MFX50_Surface* surface,
                                                   const MFX50_EncodeDecision* decision) {
    (void)decision;
    if (!encoder || !surface) return MFX50_ERR_INVALID_PARAM;
    if (encoder->flushed) {
        set_error(&encoder->last_error, "cannot push surface after encoder flush");
        return MFX50_ERR_BAD_STATE;
    }
    if (surface->type != MFX50_SURFACE_ONEVPL) {
        set_error(&encoder->last_error,
                  "only MFX50_SURFACE_ONEVPL is implemented for zero-copy surface encode");
        return MFX50_ERR_NOT_IMPLEMENTED;
    }
    if (surface->pixel_format != MFX50_PIXFMT_NV12) {
        set_error(&encoder->last_error, "only NV12 oneVPL surfaces are implemented");
        return MFX50_ERR_UNSUPPORTED;
    }

    mfxFrameSurface1* mfx_surface =
        reinterpret_cast<mfxFrameSurface1*>(surface->handle.onevpl.mfx_surface);
    if (!mfx_surface) {
        set_error(&encoder->last_error, "oneVPL surface pointer is null");
        return MFX50_ERR_INVALID_PARAM;
    }

    const bool video_memory =
        mfx_surface->Data.MemId != nullptr || mfx_surface->FrameInterface != nullptr;
    if (!video_memory) {
        set_error(&encoder->last_error,
                  "MFX50_SURFACE_ONEVPL must be a GPU/video-memory surface");
        return MFX50_ERR_UNSUPPORTED;
    }
    const int io_pattern = MFX_IOPATTERN_IN_VIDEO_MEMORY;
    int visible_width_hint = surface->crop_w > 0 ? surface->crop_w : 0;
    int visible_height_hint = surface->crop_h > 0 ? surface->crop_h : 0;
    if (visible_width_hint <= 0) {
        visible_width_hint = mfx_surface->Info.CropW > 0 ? mfx_surface->Info.CropW : surface->width;
    }
    if (visible_height_hint <= 0) {
        visible_height_hint = mfx_surface->Info.CropH > 0 ? mfx_surface->Info.CropH : surface->height;
    }
    if (visible_width_hint <= 0) {
        visible_width_hint = mfx_surface->Info.Width;
    }
    if (visible_height_hint <= 0) {
        visible_height_hint = mfx_surface->Info.Height;
    }
    const int backing_width_hint = surface->width > 0 ? surface->width : mfx_surface->Info.Width;
    const int backing_height_hint = surface->height > 0 ? surface->height : mfx_surface->Info.Height;

    MFX50_Status st = init_encoder_if_needed(encoder,
                                             visible_width_hint,
                                             visible_height_hint,
                                             backing_width_hint,
                                             backing_height_hint,
                                             io_pattern,
                                             &mfx_surface->Info);
    if (st != MFX50_OK) return st;

    if (surface->pts >= 0) {
        mfx_surface->Data.TimeStamp = static_cast<mfxU64>(surface->pts);
    }

    st = submit_encoder_surface(encoder,
                                mfx_surface,
                                surface->pts,
                                surface->user_opaque,
                                nullptr);
    if (st == MFX50_OK) encoder->input_frames++;
    return st;
}

extern "C" MFX50_Status mfx50_encoder_poll_packet(MFX50_Encoder* encoder,
                                                  MFX50_Packet* out_packet) {
    if (!encoder || !out_packet) return MFX50_ERR_INVALID_PARAM;
    if (encoder->output_queue.empty()) return MFX50_ERR_AGAIN;

    EncodedBuffer* holder = new (std::nothrow) EncodedBuffer(std::move(encoder->output_queue.front()));
    encoder->output_queue.pop_front();
    if (!holder) return MFX50_ERR_NO_MEMORY;

    std::memset(out_packet, 0, sizeof(*out_packet));
    out_packet->struct_size = sizeof(*out_packet);
    out_packet->api_version = MFX50_DEVICE_API_VERSION;
    out_packet->codec = holder->codec;
    out_packet->data = holder->data.data();
    out_packet->data_size = holder->data.size();
    out_packet->pts = holder->pts;
    out_packet->dts = holder->dts;
    out_packet->flags = holder->flags;
    out_packet->packet_handle = holder;
    out_packet->release_opaque = holder;
    out_packet->release = release_packet_buffer;
    out_packet->user_opaque = holder->user_opaque;
    return MFX50_OK;
}

extern "C" MFX50_Status mfx50_encoder_flush(MFX50_Encoder* encoder) {
    if (!encoder) return MFX50_ERR_INVALID_PARAM;
    if (!encoder->encoder_ready || encoder->flushed) {
        encoder->flushed = true;
        return MFX50_OK;
    }

    for (;;) {
        bool more_data = false;
        MFX50_Status st = submit_encoder_surface(encoder,
                                                 nullptr,
                                                 static_cast<int64_t>(encoder->input_frames),
                                                 nullptr,
                                                 &more_data);
        if (st != MFX50_OK) return st;
        if (more_data) break;
    }
    encoder->flushed = true;
    return MFX50_OK;
}

extern "C" const char* mfx50_encoder_get_last_error(MFX50_Encoder* encoder) {
    return encoder ? encoder->last_error.c_str() : g_last_error.c_str();
}

extern "C" void mfx50_encoder_destroy(MFX50_Encoder* encoder) {
    if (encoder && encoder->encoder_ready && encoder->device && encoder->device->session) {
        MFXVideoENCODE_Close(encoder->device->session);
    }
    delete encoder;
}
