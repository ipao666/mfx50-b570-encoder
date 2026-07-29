#include <vpl/mfxdispatcher.h>
#include <vpl/mfxvideo.h>
#include <va/va.h>
#include <va/va_drm.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

enum ProbeStatus {
    MBQP_SUPPORTED = 0,
    MBQP_UNSUPPORTED_BY_RUNTIME = 1,
    MBQP_INIT_FAILED = 2,
    MBQP_RUNTIME_FAILED = 3,
    MBQP_OUTPUT_INVALID = 4
};

struct Options {
    std::string device = "auto";
    std::string output = "/tmp/mfx50rt_mbqp_probe_mbqp.h265";
    std::string baselineOutput = "/tmp/mfx50rt_mbqp_probe_baseline.h265";
    std::string ffprobe = "ffprobe";
    int width = 640;
    int height = 360;
    int frames = 8;
    bool runFfprobe = true;
};

struct EncodeResult {
    ProbeStatus status = MBQP_INIT_FAILED;
    std::string error;
    size_t bytes = 0;
    bool ffprobeOk = false;
};

struct MfxException : public std::runtime_error {
    MfxException(const std::string& label, mfxStatus status)
        : std::runtime_error(label), st(status) {}
    mfxStatus st;
};

const char* probeStatusName(ProbeStatus status) {
    switch (status) {
    case MBQP_SUPPORTED: return "MBQP_SUPPORTED";
    case MBQP_UNSUPPORTED_BY_RUNTIME: return "MBQP_UNSUPPORTED_BY_RUNTIME";
    case MBQP_INIT_FAILED: return "MBQP_INIT_FAILED";
    case MBQP_RUNTIME_FAILED: return "MBQP_RUNTIME_FAILED";
    case MBQP_OUTPUT_INVALID: return "MBQP_OUTPUT_INVALID";
    default: return "MBQP_UNKNOWN";
    }
}

std::string mfxStatusName(mfxStatus st) {
    switch (st) {
    case MFX_ERR_NONE: return "MFX_ERR_NONE";
    case MFX_ERR_UNKNOWN: return "MFX_ERR_UNKNOWN";
    case MFX_ERR_NULL_PTR: return "MFX_ERR_NULL_PTR";
    case MFX_ERR_UNSUPPORTED: return "MFX_ERR_UNSUPPORTED";
    case MFX_ERR_MEMORY_ALLOC: return "MFX_ERR_MEMORY_ALLOC";
    case MFX_ERR_NOT_ENOUGH_BUFFER: return "MFX_ERR_NOT_ENOUGH_BUFFER";
    case MFX_ERR_INVALID_HANDLE: return "MFX_ERR_INVALID_HANDLE";
    case MFX_ERR_LOCK_MEMORY: return "MFX_ERR_LOCK_MEMORY";
    case MFX_ERR_NOT_INITIALIZED: return "MFX_ERR_NOT_INITIALIZED";
    case MFX_ERR_NOT_FOUND: return "MFX_ERR_NOT_FOUND";
    case MFX_ERR_MORE_DATA: return "MFX_ERR_MORE_DATA";
    case MFX_ERR_MORE_SURFACE: return "MFX_ERR_MORE_SURFACE";
    case MFX_ERR_ABORTED: return "MFX_ERR_ABORTED";
    case MFX_ERR_DEVICE_LOST: return "MFX_ERR_DEVICE_LOST";
    case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM: return "MFX_ERR_INCOMPATIBLE_VIDEO_PARAM";
    case MFX_ERR_INVALID_VIDEO_PARAM: return "MFX_ERR_INVALID_VIDEO_PARAM";
    case MFX_ERR_UNDEFINED_BEHAVIOR: return "MFX_ERR_UNDEFINED_BEHAVIOR";
    case MFX_ERR_DEVICE_FAILED: return "MFX_ERR_DEVICE_FAILED";
    case MFX_ERR_MORE_BITSTREAM: return "MFX_ERR_MORE_BITSTREAM";
    case MFX_ERR_GPU_HANG: return "MFX_ERR_GPU_HANG";
    case MFX_ERR_NOT_IMPLEMENTED: return "MFX_ERR_NOT_IMPLEMENTED";
    case MFX_WRN_IN_EXECUTION: return "MFX_WRN_IN_EXECUTION";
    case MFX_WRN_DEVICE_BUSY: return "MFX_WRN_DEVICE_BUSY";
    case MFX_WRN_VIDEO_PARAM_CHANGED: return "MFX_WRN_VIDEO_PARAM_CHANGED";
    case MFX_WRN_PARTIAL_ACCELERATION: return "MFX_WRN_PARTIAL_ACCELERATION";
    case MFX_WRN_INCOMPATIBLE_VIDEO_PARAM: return "MFX_WRN_INCOMPATIBLE_VIDEO_PARAM";
    case MFX_WRN_VALUE_NOT_CHANGED: return "MFX_WRN_VALUE_NOT_CHANGED";
    case MFX_WRN_OUT_OF_RANGE: return "MFX_WRN_OUT_OF_RANGE";
    case MFX_WRN_FILTER_SKIPPED: return "MFX_WRN_FILTER_SKIPPED";
    default: return "mfxStatus(" + std::to_string(st) + ")";
    }
}

bool isUnsupportedStatus(mfxStatus st) {
    return st == MFX_ERR_UNSUPPORTED ||
           st == MFX_ERR_NOT_IMPLEMENTED ||
           st == MFX_ERR_INVALID_VIDEO_PARAM ||
           st == MFX_ERR_INCOMPATIBLE_VIDEO_PARAM;
}

void requireMfx(mfxStatus st, const char* label) {
    if (st < MFX_ERR_NONE) {
        throw MfxException(label, st);
    }
}

int align16(int value) {
    return (value + 15) & ~15;
}

std::string shellQuote(const std::string& value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') out += "'\\''";
        else out += ch;
    }
    out += "'";
    return out;
}

std::string defaultDevice() {
    if (access("/dev/dri/renderD129", R_OK | W_OK) == 0) return "/dev/dri/renderD129";
    return "/dev/dri/renderD128";
}

bool parseInt(const char* text, int* out) {
    if (!text || !out) return false;
    char* end = nullptr;
    long value = std::strtol(text, &end, 10);
    if (!end || *end != '\0') return false;
    *out = static_cast<int>(value);
    return true;
}

void usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s [--device /dev/dri/renderD129] [--output out.h265] [options]\n"
        "\n"
        "options:\n"
        "  --baseline-output <path>  output without MBQP, default /tmp/mfx50rt_mbqp_probe_baseline.h265\n"
        "  --width <n>               default 640\n"
        "  --height <n>              default 360\n"
        "  --frames <n>              default 8\n"
        "  --ffprobe <path>          default ffprobe\n"
        "  --no-ffprobe              skip ffprobe validation\n",
        argv0);
}

bool parseArgs(int argc, char** argv, Options* opt) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto needValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            std::exit(0);
        }
        else if (arg == "--device") {
            const char* value = needValue("--device");
            if (!value) return false;
            opt->device = value;
        }
        else if (arg == "--output") {
            const char* value = needValue("--output");
            if (!value) return false;
            opt->output = value;
        }
        else if (arg == "--baseline-output") {
            const char* value = needValue("--baseline-output");
            if (!value) return false;
            opt->baselineOutput = value;
        }
        else if (arg == "--width") {
            const char* value = needValue("--width");
            if (!value || !parseInt(value, &opt->width)) return false;
        }
        else if (arg == "--height") {
            const char* value = needValue("--height");
            if (!value || !parseInt(value, &opt->height)) return false;
        }
        else if (arg == "--frames") {
            const char* value = needValue("--frames");
            if (!value || !parseInt(value, &opt->frames)) return false;
        }
        else if (arg == "--ffprobe") {
            const char* value = needValue("--ffprobe");
            if (!value) return false;
            opt->ffprobe = value;
        }
        else if (arg == "--no-ffprobe") {
            opt->runFfprobe = false;
        }
        else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return false;
        }
    }

    if (opt->device == "auto") opt->device = defaultDevice();
    if (opt->width <= 0 || opt->height <= 0 || opt->frames <= 0) {
        std::fprintf(stderr, "width, height, and frames must be positive\n");
        return false;
    }
    return true;
}

struct VaDevice {
    int fd = -1;
    VADisplay display = nullptr;

    explicit VaDevice(const std::string& path) {
        fd = open(path.c_str(), O_RDWR);
        if (fd < 0) {
            throw std::runtime_error("failed to open VAAPI device: " + path);
        }
        display = vaGetDisplayDRM(fd);
        if (!display) {
            throw std::runtime_error("vaGetDisplayDRM failed for " + path);
        }
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
        requireMfx(MFXCreateSession(loader, 0, &session), "MFXCreateSession");
        if (!session) throw std::runtime_error("MFXCreateSession returned null");
    }

    ~VplSession() {
        if (session) MFXClose(session);
        if (loader) MFXUnload(loader);
    }
};

void fillNv12(std::vector<uint8_t>& y,
              std::vector<uint8_t>& uv,
              int width,
              int height,
              int pitch,
              int frameIndex) {
    for (int row = 0; row < height; ++row) {
        uint8_t* dst = y.data() + static_cast<size_t>(row) * pitch;
        for (int col = 0; col < width; ++col) {
            dst[col] = static_cast<uint8_t>((col * 3 + row * 2 + frameIndex * 5) & 0xff);
        }
        std::fill(dst + width, dst + pitch, dst[width - 1]);
    }
    for (int row = 0; row < height / 2; ++row) {
        uint8_t* dst = uv.data() + static_cast<size_t>(row) * pitch;
        for (int col = 0; col < width; col += 2) {
            dst[col] = static_cast<uint8_t>(96 + ((row + frameIndex) & 31));
            dst[col + 1] = static_cast<uint8_t>(128 + ((col / 2) & 31));
        }
        for (int col = width; col < pitch; ++col) {
            dst[col] = 128;
        }
    }
}

void resetBitstream(mfxBitstream* bs, std::vector<uint8_t>* storage) {
    std::memset(bs, 0, sizeof(*bs));
    bs->Data = storage->data();
    bs->MaxLength = static_cast<mfxU32>(storage->size());
}

void appendBitstream(const mfxBitstream& bs, std::vector<uint8_t>* out) {
    if (bs.DataLength == 0) return;
    out->insert(out->end(),
                bs.Data + bs.DataOffset,
                bs.Data + bs.DataOffset + bs.DataLength);
}

void submitEncode(mfxSession session,
                  mfxEncodeCtrl* ctrl,
                  mfxFrameSurface1* surface,
                  mfxBitstream* bs,
                  std::vector<uint8_t>* bsStorage,
                  std::vector<uint8_t>* out) {
    for (;;) {
        resetBitstream(bs, bsStorage);
        mfxSyncPoint syncp = nullptr;
        mfxStatus st = MFXVideoENCODE_EncodeFrameAsync(session, ctrl, surface, bs, &syncp);
        if (st == MFX_WRN_DEVICE_BUSY) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (st == MFX_ERR_MORE_BITSTREAM) {
            bsStorage->resize(bsStorage->size() * 2);
            continue;
        }
        if (st == MFX_ERR_MORE_DATA) {
            return;
        }
        requireMfx(st, "MFXVideoENCODE_EncodeFrameAsync");
        if (syncp) {
            requireMfx(MFXVideoCORE_SyncOperation(session, syncp, 60000), "MFXVideoCORE_SyncOperation");
            appendBitstream(*bs, out);
        }
        return;
    }
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

bool runFfprobe(const Options& opt, const std::string& path) {
    if (!opt.runFfprobe) return true;
    const std::string cmd = shellQuote(opt.ffprobe) +
        " -v error -select_streams v:0 -show_entries stream=codec_name,width,height "
        "-of csv=p=0 " + shellQuote(path);
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return false;
    char buf[512] = {};
    std::string output;
    while (std::fgets(buf, sizeof(buf), pipe)) {
        output += buf;
    }
    const int rc = pclose(pipe);
    return rc == 0 && output.find("hevc") != std::string::npos;
}

EncodeResult encodeProbe(const Options& opt, bool enableMbqp, const std::string& path) {
    EncodeResult result;
    result.status = enableMbqp ? MBQP_UNSUPPORTED_BY_RUNTIME : MBQP_INIT_FAILED;

    try {
        VaDevice va(opt.device);
        VplSession vpl;
        requireMfx(MFXVideoCORE_SetHandle(vpl.session, MFX_HANDLE_VA_DISPLAY, va.display),
                   "MFXVideoCORE_SetHandle");

        const int alignedWidth = align16(opt.width);
        const int alignedHeight = align16(opt.height);

        mfxVideoParam par = {};
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
        par.mfx.FrameInfo.Width = static_cast<mfxU16>(alignedWidth);
        par.mfx.FrameInfo.Height = static_cast<mfxU16>(alignedHeight);
        par.mfx.FrameInfo.CropW = static_cast<mfxU16>(opt.width);
        par.mfx.FrameInfo.CropH = static_cast<mfxU16>(opt.height);

        mfxExtCodingOption3 codingOption3 = {};
        mfxExtBuffer* initExtParams[1] = {};
        if (enableMbqp) {
            codingOption3.Header.BufferId = MFX_EXTBUFF_CODING_OPTION3;
            codingOption3.Header.BufferSz = sizeof(codingOption3);
            codingOption3.EnableMBQP = MFX_CODINGOPTION_ON;
            initExtParams[0] = reinterpret_cast<mfxExtBuffer*>(&codingOption3);
            par.ExtParam = initExtParams;
            par.NumExtParam = 1;
        }

        mfxVideoParam queried = par;
        mfxExtCodingOption3 queriedOption3 = codingOption3;
        mfxExtBuffer* queriedExtParams[1] = {};
        if (enableMbqp) {
            queriedExtParams[0] = reinterpret_cast<mfxExtBuffer*>(&queriedOption3);
            queried.ExtParam = queriedExtParams;
            queried.NumExtParam = 1;
        }
        mfxStatus st = MFXVideoENCODE_Query(vpl.session, &par, &queried);
        if (st < MFX_ERR_NONE) {
            result.status = enableMbqp && isUnsupportedStatus(st)
                ? MBQP_UNSUPPORTED_BY_RUNTIME
                : MBQP_INIT_FAILED;
            result.error = std::string("MFXVideoENCODE_Query failed: ") + mfxStatusName(st);
            return result;
        }
        par = queried;
        if (enableMbqp) {
            codingOption3 = queriedOption3;
            initExtParams[0] = reinterpret_cast<mfxExtBuffer*>(&codingOption3);
            par.ExtParam = initExtParams;
            par.NumExtParam = 1;
            if (codingOption3.EnableMBQP == MFX_CODINGOPTION_OFF) {
                result.status = MBQP_UNSUPPORTED_BY_RUNTIME;
                result.error = "MFXVideoENCODE_Query disabled EnableMBQP";
                return result;
            }
        }

        st = MFXVideoENCODE_Init(vpl.session, &par);
        if (st < MFX_ERR_NONE) {
            result.status = enableMbqp && isUnsupportedStatus(st)
                ? MBQP_UNSUPPORTED_BY_RUNTIME
                : MBQP_INIT_FAILED;
            result.error = std::string("MFXVideoENCODE_Init failed: ") + mfxStatusName(st);
            return result;
        }

        std::vector<uint8_t> y(static_cast<size_t>(alignedWidth) * alignedHeight);
        std::vector<uint8_t> uv(static_cast<size_t>(alignedWidth) * alignedHeight / 2);
        std::vector<uint8_t> bitstreamStorage(4 * 1024 * 1024);
        std::vector<uint8_t> encoded;
        mfxBitstream bs = {};

        const int blocksW = (opt.width + 15) / 16;
        const int blocksH = (opt.height + 15) / 16;
        std::vector<uint8_t> qpMap(static_cast<size_t>(blocksW) * blocksH, 41);
        for (int by = 0; by < blocksH; ++by) {
            for (int bx = 0; bx < blocksW; ++bx) {
                qpMap[static_cast<size_t>(by) * blocksW + bx] =
                    static_cast<uint8_t>(bx < blocksW / 2 ? 36 : 46);
            }
        }

        for (int i = 0; i < opt.frames; ++i) {
            fillNv12(y, uv, opt.width, opt.height, alignedWidth, i);
            mfxFrameSurface1 surface = {};
            surface.Info = par.mfx.FrameInfo;
            surface.Data.Y = y.data();
            surface.Data.UV = uv.data();
            surface.Data.PitchLow = static_cast<mfxU16>(alignedWidth);
            surface.Data.TimeStamp = static_cast<mfxU64>(i);

            mfxExtMBQP mbqp = {};
            mfxExtBuffer* ctrlExtParams[1] = {};
            mfxEncodeCtrl ctrl = {};
            mfxEncodeCtrl* ctrlPtr = nullptr;
            if (enableMbqp) {
                mbqp.Header.BufferId = MFX_EXTBUFF_MBQP;
                mbqp.Header.BufferSz = sizeof(mbqp);
                mbqp.Pitch = static_cast<mfxU32>(blocksW);
                mbqp.Mode = MFX_MBQP_MODE_QP_VALUE;
                mbqp.BlockSize = 16;
                mbqp.NumQPAlloc = static_cast<mfxU32>(qpMap.size());
                mbqp.QP = qpMap.data();
                ctrlExtParams[0] = reinterpret_cast<mfxExtBuffer*>(&mbqp);
                ctrl.NumExtParam = 1;
                ctrl.ExtParam = ctrlExtParams;
                ctrlPtr = &ctrl;
            }
            submitEncode(vpl.session, ctrlPtr, &surface, &bs, &bitstreamStorage, &encoded);
        }

        for (;;) {
            const size_t before = encoded.size();
            submitEncode(vpl.session, nullptr, nullptr, &bs, &bitstreamStorage, &encoded);
            if (encoded.size() == before) break;
        }

        MFXVideoENCODE_Close(vpl.session);
        if (encoded.empty()) {
            result.status = MBQP_OUTPUT_INVALID;
            result.error = "encoder produced empty bitstream";
            return result;
        }
        if (!writeFile(path, encoded)) {
            result.status = MBQP_OUTPUT_INVALID;
            result.error = "failed to write output file";
            return result;
        }
        result.bytes = encoded.size();
        result.ffprobeOk = runFfprobe(opt, path);
        if (!result.ffprobeOk) {
            result.status = MBQP_OUTPUT_INVALID;
            result.error = "ffprobe did not validate HEVC output";
            return result;
        }
        result.status = MBQP_SUPPORTED;
        return result;
    }
    catch (const MfxException& ex) {
        result.status = enableMbqp && isUnsupportedStatus(ex.st)
            ? MBQP_UNSUPPORTED_BY_RUNTIME
            : MBQP_RUNTIME_FAILED;
        result.error = ex.what() + std::string(" failed: ") + mfxStatusName(ex.st);
        return result;
    }
    catch (const std::exception& ex) {
        result.status = MBQP_INIT_FAILED;
        result.error = ex.what();
        return result;
    }
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, &opt)) {
        usage(argv[0]);
        return 2;
    }

    EncodeResult baseline = encodeProbe(opt, false, opt.baselineOutput);
    EncodeResult mbqp = encodeProbe(opt, true, opt.output);

    std::printf("device=%s\n", opt.device.c_str());
    std::printf("width=%d\n", opt.width);
    std::printf("height=%d\n", opt.height);
    std::printf("frames=%d\n", opt.frames);
    std::printf("baseline_output=%s\n", opt.baselineOutput.c_str());
    std::printf("baseline_ok=%d\n", baseline.status == MBQP_SUPPORTED ? 1 : 0);
    std::printf("baseline_status=%s\n",
                baseline.status == MBQP_SUPPORTED ? "BASELINE_OK" : probeStatusName(baseline.status));
    std::printf("baseline_bytes=%zu\n", baseline.bytes);
    std::printf("baseline_ffprobe_ok=%d\n", baseline.ffprobeOk ? 1 : 0);
    if (!baseline.error.empty()) std::printf("baseline_error=%s\n", baseline.error.c_str());
    std::printf("mbqp_output=%s\n", opt.output.c_str());
    std::printf("mbqp_status=%s\n", probeStatusName(mbqp.status));
    std::printf("mbqp_bytes=%zu\n", mbqp.bytes);
    std::printf("mbqp_ffprobe_ok=%d\n", mbqp.ffprobeOk ? 1 : 0);
    if (!mbqp.error.empty()) std::printf("mbqp_error=%s\n", mbqp.error.c_str());
    if (baseline.bytes > 0 && mbqp.bytes > 0) {
        const double ratio = static_cast<double>(mbqp.bytes) / static_cast<double>(baseline.bytes);
        std::printf("mbqp_to_baseline_ratio=%.6f\n", ratio);
    }

    return mbqp.status == MBQP_SUPPORTED ? 0 : 1;
}
