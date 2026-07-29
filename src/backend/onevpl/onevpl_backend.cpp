#include "onevpl_backend.h"

#include <vpl/mfxdispatcher.h>
#include <vpl/mfxvideo.h>
#include <va/va.h>
#include <va/va_drm.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace mfx50rt::onevpl {

namespace {

void copy_cstr(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) return;
    std::snprintf(dst, dst_size, "%s", src ? src : "");
}

std::string default_device() {
    if (access("/dev/dri/renderD129", R_OK | W_OK) == 0) return "/dev/dri/renderD129";
    return "/dev/dri/renderD128";
}

std::string resolve_device(const MFX50RT_BackendConfig& backend) {
    if (backend.device_name[0]) return backend.device_name;
    if (backend.device_index == 1 && access("/dev/dri/renderD129", R_OK | W_OK) == 0) {
        return "/dev/dri/renderD129";
    }
    return default_device();
}

std::string mfx_status_name(mfxStatus st) {
    switch (st) {
        case MFX_ERR_NONE: return "MFX_ERR_NONE";
        case MFX_ERR_UNSUPPORTED: return "MFX_ERR_UNSUPPORTED";
        case MFX_ERR_NOT_IMPLEMENTED: return "MFX_ERR_NOT_IMPLEMENTED";
        case MFX_ERR_INVALID_VIDEO_PARAM: return "MFX_ERR_INVALID_VIDEO_PARAM";
        case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM: return "MFX_ERR_INCOMPATIBLE_VIDEO_PARAM";
        case MFX_ERR_NOT_FOUND: return "MFX_ERR_NOT_FOUND";
        case MFX_ERR_DEVICE_FAILED: return "MFX_ERR_DEVICE_FAILED";
        default: return "mfxStatus(" + std::to_string(st) + ")";
    }
}

void require_mfx(mfxStatus st, const char* label) {
    if (st < MFX_ERR_NONE) {
        throw std::runtime_error(std::string(label) + " failed: " + mfx_status_name(st));
    }
}

struct VaDevice {
    int fd = -1;
    VADisplay display = nullptr;

    explicit VaDevice(const std::string& path) {
        fd = open(path.c_str(), O_RDWR);
        if (fd < 0) throw std::runtime_error("failed to open VAAPI device: " + path);
        display = vaGetDisplayDRM(fd);
        if (!display) throw std::runtime_error("vaGetDisplayDRM failed: " + path);
        int major = 0;
        int minor = 0;
        VAStatus st = vaInitialize(display, &major, &minor);
        if (st != VA_STATUS_SUCCESS) {
            throw std::runtime_error(std::string("vaInitialize failed: ") + vaErrorStr(st));
        }
    }

    ~VaDevice() {
        if (display) vaTerminate(display);
        if (fd >= 0) close(fd);
    }
};

struct VplSession {
    mfxLoader loader = nullptr;
    mfxSession session = nullptr;

    VplSession() {
        loader = MFXLoad();
        if (!loader) throw std::runtime_error("MFXLoad failed");
        MFX_ADD_PROPERTY_U32(loader, "mfxImplDescription.Impl", MFX_IMPL_TYPE_HARDWARE);
        MFX_ADD_PROPERTY_U32(loader, "mfxImplDescription.AccelerationMode", MFX_ACCEL_MODE_VIA_VAAPI);
        require_mfx(MFXCreateSession(loader, 0, &session), "MFXCreateSession");
        if (!session) throw std::runtime_error("MFXCreateSession returned null");
    }

    ~VplSession() {
        if (session) MFXClose(session);
        if (loader) MFXUnload(loader);
    }
};

bool query_hevc_cqp(mfxSession session, bool mbqp) {
    mfxVideoParam par{};
    par.AsyncDepth = 1;
    par.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
    par.mfx.CodecId = MFX_CODEC_HEVC;
    par.mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED;
    par.mfx.RateControlMethod = MFX_RATECONTROL_CQP;
    par.mfx.QPI = 35;
    par.mfx.QPP = 41;
    par.mfx.QPB = 47;
    par.mfx.GopPicSize = 30;
    par.mfx.GopRefDist = 1;
    par.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    par.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    par.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    par.mfx.FrameInfo.FrameRateExtN = 30;
    par.mfx.FrameInfo.FrameRateExtD = 1;
    par.mfx.FrameInfo.Width = 640;
    par.mfx.FrameInfo.Height = 368;
    par.mfx.FrameInfo.CropW = 640;
    par.mfx.FrameInfo.CropH = 360;

    mfxExtCodingOption3 coding_option3{};
    mfxExtBuffer* ext[1]{};
    if (mbqp) {
        coding_option3.Header.BufferId = MFX_EXTBUFF_CODING_OPTION3;
        coding_option3.Header.BufferSz = sizeof(coding_option3);
        coding_option3.EnableMBQP = MFX_CODINGOPTION_ON;
        ext[0] = reinterpret_cast<mfxExtBuffer*>(&coding_option3);
        par.ExtParam = ext;
        par.NumExtParam = 1;
    }

    mfxVideoParam queried = par;
    mfxExtCodingOption3 queried_option3 = coding_option3;
    mfxExtBuffer* queried_ext[1]{};
    if (mbqp) {
        queried_ext[0] = reinterpret_cast<mfxExtBuffer*>(&queried_option3);
        queried.ExtParam = queried_ext;
        queried.NumExtParam = 1;
    }

    mfxStatus st = MFXVideoENCODE_Query(session, &par, &queried);
    if (st < MFX_ERR_NONE) return false;
    if (mbqp && queried_option3.EnableMBQP == MFX_CODINGOPTION_OFF) return false;

    st = MFXVideoENCODE_Init(session, &queried);
    if (st < MFX_ERR_NONE) return false;
    MFXVideoENCODE_Close(session);
    return true;
}

} // namespace

MFX50RT_Status queryConservativeCapabilities(const MFX50RT_BackendConfig& backend,
                                             MFX50RT_Capabilities* caps) {
    if (!caps || caps->size < sizeof(MFX50RT_Capabilities) ||
        caps->version != MFX50RT_API_VERSION) {
        return MFX50RT_ERR_INVALID_ARG;
    }
    MFX50RT_Capabilities out{};
    out.size = sizeof(out);
    out.version = MFX50RT_API_VERSION;
    out.supports_hw_decode = backend.type == MFX50RT_BACKEND_ONEVPL || backend.type == MFX50RT_BACKEND_AUTO;
    out.supports_hw_encode = out.supports_hw_decode;
    out.supports_video_memory = out.supports_hw_decode;
    out.supports_zero_copy = out.supports_hw_decode;
    out.supports_hevc_decode = out.supports_hw_decode;
    out.supports_hevc_encode = out.supports_hw_encode;
    out.supports_h264_decode = out.supports_hw_decode;
    out.supports_h264_encode = out.supports_hw_encode;
    out.supports_av1_decode = 0;
    out.supports_av1_encode = 0;
    out.supports_cqp = 1;
    out.supports_qvbr = 1;
    out.supports_icq = 1;
    out.supports_vbr = 1;
    out.supports_cbr = 1;
    out.supports_ipb_qp = 1;
    out.supports_force_idr = 1;
    out.supports_b_frames = 1;

    /*
     * Do not advertise MBQP/ROI without a successful low-level probe. The
     * v0.5 branch has probes and a native encoder path, but this public facade
     * keeps capability reporting conservative until a backend adapter can bind
     * a real mfxSession/device and verify these ext buffers.
     */
    out.supports_roi_delta_qp = 0;
    out.supports_mbqp = 0;
    out.supports_ctu_qp_map = 0;
    out.max_roi_regions = 0;
    out.max_width = 8192;
    out.max_height = 4320;
    out.max_async_depth = backend.async_depth > 0 ? backend.async_depth : 4;
    copy_cstr(out.backend_name, sizeof(out.backend_name), "oneVPL-conservative");
    copy_cstr(out.device_name,
              sizeof(out.device_name),
              backend.device_name[0] ? backend.device_name : "auto");
    copy_cstr(out.driver_desc,
              sizeof(out.driver_desc),
              "conservative capability set; MBQP/ROI require runtime probe");
    *caps = out;
    return MFX50RT_OK;
}

MFX50RT_Status queryRealCapabilities(const MFX50RT_BackendConfig& backend,
                                     MFX50RT_Capabilities* caps) {
    if (!caps || caps->size < sizeof(MFX50RT_Capabilities) ||
        caps->version != MFX50RT_API_VERSION) {
        return MFX50RT_ERR_INVALID_ARG;
    }

    MFX50RT_Capabilities out{};
    out.size = sizeof(out);
    out.version = MFX50RT_API_VERSION;
    out.max_width = 8192;
    out.max_height = 4320;
    out.max_async_depth = backend.async_depth > 0 ? backend.async_depth : 4;
    copy_cstr(out.backend_name, sizeof(out.backend_name), "oneVPL-VAAPI");

    const std::string device = resolve_device(backend);
    copy_cstr(out.device_name, sizeof(out.device_name), device.c_str());

    try {
        VaDevice va(device);
        VplSession vpl;
        require_mfx(MFXVideoCORE_SetHandle(vpl.session, MFX_HANDLE_VA_DISPLAY, va.display),
                    "MFXVideoCORE_SetHandle");

        const bool hevc_cqp = query_hevc_cqp(vpl.session, false);
        const bool mbqp = hevc_cqp && query_hevc_cqp(vpl.session, true);

        out.supports_hw_decode = 1;
        out.supports_hw_encode = hevc_cqp ? 1 : 0;
        out.supports_video_memory = 1;
        out.supports_zero_copy = 1;
        out.supports_hevc_decode = 1;
        out.supports_hevc_encode = hevc_cqp ? 1 : 0;
        out.supports_h264_decode = 1;
        out.supports_h264_encode = 1;
        out.supports_av1_decode = 0;
        out.supports_av1_encode = 0;
        out.supports_cqp = hevc_cqp ? 1 : 0;
        out.supports_qvbr = hevc_cqp ? 1 : 0;
        out.supports_icq = hevc_cqp ? 1 : 0;
        out.supports_vbr = hevc_cqp ? 1 : 0;
        out.supports_cbr = hevc_cqp ? 1 : 0;
        out.supports_ipb_qp = hevc_cqp ? 1 : 0;
        out.supports_force_idr = hevc_cqp ? 1 : 0;
        out.supports_b_frames = hevc_cqp ? 1 : 0;
        out.supports_roi_delta_qp = 0;
        out.supports_mbqp = mbqp ? 1 : 0;
        out.supports_ctu_qp_map = mbqp ? 1 : 0;
        out.max_roi_regions = 0;
        copy_cstr(out.driver_desc,
                  sizeof(out.driver_desc),
                  mbqp ? "VAAPI hardware probe passed HEVC CQP and MBQP init"
                       : "VAAPI hardware probe passed HEVC CQP; MBQP init unavailable");
    } catch (const std::exception& ex) {
        copy_cstr(out.driver_desc, sizeof(out.driver_desc), ex.what());
    }

    *caps = out;
    return MFX50RT_OK;
}

} // namespace mfx50rt::onevpl
