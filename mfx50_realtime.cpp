#include "mfx50_realtime.h"

#include "mfx50_algo_config.h"
#include "mfx50_output_queue.h"
#include "mfx50_preprocess.h"
#include "mfx50_scene_analyzer.h"
#include "src/backend/onevpl/onevpl_realtime_internal.h"

#include <vpl/mfxdispatcher.h>
#include <vpl/mfxvideo.h>
#include <va/va.h>
#include <va/va_drm.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kMaxRoutes = 256;
constexpr size_t kInitialBitstreamBytes = 16 * 1024 * 1024;
constexpr size_t kOutputBitstreamBytes = 8 * 1024 * 1024;
constexpr int kMbqpBlockSize = 16;

thread_local std::string g_last_error;
thread_local std::string g_status_string;

uint64_t nowUs() {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            clock::now() - start).count());
}

uint64_t elapsedUs(uint64_t startUs) {
    return nowUs() - startUs;
}

int ceilDiv(int value, int denom) {
    return denom > 0 ? (value + denom - 1) / denom : 0;
}

void copyString(char* dst, size_t cap, const std::string& src) {
    if (!dst || cap == 0) return;
    std::snprintf(dst, cap, "%s", src.c_str());
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
    case MFX_ERR_REALLOC_SURFACE: return "MFX_ERR_REALLOC_SURFACE";
    case MFX_ERR_RESOURCE_MAPPED: return "MFX_ERR_RESOURCE_MAPPED";
    case MFX_ERR_NOT_IMPLEMENTED: return "MFX_ERR_NOT_IMPLEMENTED";
    case MFX_ERR_MORE_EXTBUFFER: return "MFX_ERR_MORE_EXTBUFFER";
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

const char* statusString(int code) {
    switch (code) {
    case MFX50_OK:
        return "MFX50_OK";
    case MFX50_ERR_INVALID_ARG:
        return "MFX50_ERR_INVALID_ARG";
    case MFX50_ERR_DEVICE:
        return "MFX50_ERR_DEVICE";
    case MFX50_ERR_DECODE:
        return "MFX50_ERR_DECODE";
    case MFX50_ERR_ENCODE:
        return "MFX50_ERR_ENCODE";
    case MFX50_ERR_NOT_IMPLEMENTED:
        return "MFX50_ERR_NOT_IMPLEMENTED";
    case MFX50_ERR_BACKPRESSURE:
        return "MFX50_ERR_BACKPRESSURE";
    case MFX50_ERR_BUFFER_TOO_SMALL:
        return "MFX50_ERR_BUFFER_TOO_SMALL";
    case MFX50_ERR_NO_OUTPUT:
        return "MFX50_ERR_NO_OUTPUT";
    case MFX50_ERR_NEED_MORE_INPUT:
        return "MFX50_ERR_NEED_MORE_INPUT";
    case MFX50_ERR_AGAIN:
        return "MFX50_ERR_AGAIN";
    default:
        g_status_string = "MFX50_STATUS_UNKNOWN(" + std::to_string(code) + ")";
        return g_status_string.c_str();
    }
}

void setGlobalError(const std::string& msg) {
    g_last_error = msg;
}

void requireMfx(mfxStatus st, const char* label) {
    if (st < MFX_ERR_NONE) {
        throw std::runtime_error(std::string(label) + " failed: " + mfxStatusName(st));
    }
}

void requireVa(VAStatus st, const char* label) {
    if (st != VA_STATUS_SUCCESS) {
        throw std::runtime_error(std::string(label) + " failed: " + vaErrorStr(st));
    }
}

bool startCodeAt(const uint8_t* data, size_t size, size_t pos, size_t* prefix) {
    if (pos + 3 <= size && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1) {
        *prefix = 3;
        return true;
    }
    if (pos + 4 <= size && data[pos] == 0 && data[pos + 1] == 0 &&
        data[pos + 2] == 0 && data[pos + 3] == 1) {
        *prefix = 4;
        return true;
    }
    return false;
}

bool containsHevcKeyframe(const uint8_t* data, size_t size) {
    if (!data || size < 5) return false;
    for (size_t pos = 0; pos + 5 < size; ++pos) {
        size_t prefix = 0;
        if (!startCodeAt(data, size, pos, &prefix)) continue;
        const size_t payload = pos + prefix;
        if (payload >= size) continue;
        const int nalType = (data[payload] >> 1) & 0x3f;
        if (nalType >= 16 && nalType <= 21) return true;
    }
    return false;
}

mfxU32 codecId(MFX50_Codec codec) {
    if (codec == MFX50_CODEC_H264) return MFX_CODEC_AVC;
    if (codec == MFX50_CODEC_HEVC) return MFX_CODEC_HEVC;
    throw std::runtime_error("unsupported codec");
}

std::string defaultDeviceForRoute(const MFX50RT_Config& cfg, int routeId) {
    std::string selector = (cfg.device_selector && cfg.device_selector[0])
        ? cfg.device_selector
        : "auto";
    if (selector.rfind("render:", 0) == 0) {
        return selector.substr(7);
    }
    if (selector.rfind("/dev/dri/", 0) == 0) {
        return selector;
    }
    if (selector == "intel:iGPU:0") {
        return "/dev/dri/renderD128";
    }
    if (selector == "intel:dGPU:0") {
        return "/dev/dri/renderD129";
    }
    if (selector == "auto") {
        if (cfg.route_count >= 50 && routeId >= 44 && access("/dev/dri/renderD128", R_OK | W_OK) == 0) {
            return "/dev/dri/renderD128";
        }
        if (access("/dev/dri/renderD129", R_OK | W_OK) == 0) return "/dev/dri/renderD129";
        return "/dev/dri/renderD128";
    }
    return selector;
}

struct ResolvedEncodeProfile {
    int targetUsage = MFX_TARGETUSAGE_BALANCED;
    int gop = 300;
    int gopRefDist = 4;
    int numRefFrame = 4;
    int qpi = 33;
    int qpp = 38;
    int qpb = 45;
    int brefType = 2;
};

ResolvedEncodeProfile resolveProfile(const MFX50RT_Config& cfg) {
    ResolvedEncodeProfile p;
    if (cfg.profile == MFX50_PROFILE_THROUGHPUT_ONLY) {
        p.targetUsage = MFX_TARGETUSAGE_BEST_SPEED;
        p.gop = 60;
        p.gopRefDist = 1;
        p.numRefFrame = 0;
        p.qpi = 32;
        p.qpp = 32;
        p.qpb = 32;
        p.brefType = 0;
    }
    else if (cfg.profile == MFX50_PROFILE_COMPRESS_85_PROBE) {
        p.targetUsage = MFX_TARGETUSAGE_BALANCED;
        p.gop = 300;
        p.gopRefDist = 4;
        p.numRefFrame = 4;
        p.qpi = 33;
        p.qpp = 39;
        p.qpb = 44;
        p.brefType = 2;
    }
    else if (cfg.profile == MFX50_PROFILE_COMPRESS_90_PROBE_A) {
        p.targetUsage = MFX_TARGETUSAGE_BALANCED;
        p.gop = 300;
        p.gopRefDist = 4;
        p.numRefFrame = 4;
        p.qpi = 34;
        p.qpp = 40;
        p.qpb = 46;
        p.brefType = 2;
    }
    else if (cfg.profile == MFX50_PROFILE_COMPRESS_90_PROBE_B) {
        p.targetUsage = MFX_TARGETUSAGE_BALANCED;
        p.gop = 300;
        p.gopRefDist = 4;
        p.numRefFrame = 4;
        p.qpi = 35;
        p.qpp = 41;
        p.qpb = 47;
        p.brefType = 2;
    }
    else if (cfg.profile == MFX50_PROFILE_COMPRESS_90_PROBE_C) {
        p.targetUsage = MFX_TARGETUSAGE_BALANCED;
        p.gop = 300;
        p.gopRefDist = 4;
        p.numRefFrame = 4;
        p.qpi = 36;
        p.qpp = 42;
        p.qpb = 48;
        p.brefType = 2;
    }
    else if (cfg.profile == MFX50_PROFILE_COMPRESS_90_PROBE_D) {
        p.targetUsage = MFX_TARGETUSAGE_BALANCED;
        p.gop = 300;
        p.gopRefDist = 4;
        p.numRefFrame = 4;
        p.qpi = 36;
        p.qpp = 43;
        p.qpb = 49;
        p.brefType = 2;
    }
    else {
        p.targetUsage = MFX_TARGETUSAGE_BALANCED;
        p.gop = 300;
        p.gopRefDist = 4;
        p.numRefFrame = 4;
        p.qpi = 33;
        p.qpp = 38;
        p.qpb = 45;
        p.brefType = 2;
    }

    if (cfg.target_usage > 0) p.targetUsage = cfg.target_usage;
    if (cfg.gop > 0) p.gop = cfg.gop;
    if (cfg.gop_ref_dist > 0) p.gopRefDist = cfg.gop_ref_dist;
    if (cfg.num_ref_frame >= 0) p.numRefFrame = cfg.num_ref_frame;
    if (cfg.qpi > 0) p.qpi = cfg.qpi;
    if (cfg.qpp > 0) p.qpp = cfg.qpp;
    if (cfg.qpb > 0) p.qpb = cfg.qpb;
    if (cfg.bref_type > 0) p.brefType = cfg.bref_type;
    return p;
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
        requireVa(vaInitialize(display, &major, &minor), "vaInitialize");
    }

    ~VaDevice() {
        if (display) vaTerminate(display);
        if (fd >= 0) close(fd);
    }

    VaDevice(const VaDevice&) = delete;
    VaDevice& operator=(const VaDevice&) = delete;
};

struct VplSession {
    mfxLoader loader = nullptr;
    mfxSession session = nullptr;

    VplSession() {
        loader = MFXLoad();
        if (!loader) {
            throw std::runtime_error("MFXLoad failed");
        }
        MFX_ADD_PROPERTY_U32(loader, "mfxImplDescription.Impl", MFX_IMPL_TYPE_HARDWARE);
        MFX_ADD_PROPERTY_U32(loader, "mfxImplDescription.AccelerationMode", MFX_ACCEL_MODE_VIA_VAAPI);
        requireMfx(MFXCreateSession(loader, 0, &session), "MFXCreateSession");
        if (!session) {
            throw std::runtime_error("MFXCreateSession returned null");
        }
    }

    ~VplSession() {
        if (session) MFXClose(session);
        if (loader) MFXUnload(loader);
    }

    VplSession(const VplSession&) = delete;
    VplSession& operator=(const VplSession&) = delete;
};

struct RouteContext {
    int id = 0;
    std::string devicePath;
    std::unique_ptr<VaDevice> va;
    std::unique_ptr<VplSession> vpl;

    bool decoderReady = false;
    bool encoderReady = false;
    bool decoderDrained = false;
    bool encoderDrained = false;

    mfxVideoParam decPar = {};
    mfxVideoParam encPar = {};
    mfxExtCodingOption2 codingOption2 = {};
    mfxExtCodingOption3 codingOption3 = {};
    mfxExtBuffer* encExtParams[2] = {};

    struct PendingEncodeControl {
        bool inUse = false;
        bool hasMbqp = false;
        bool fallback = false;
        mfxEncodeCtrl ctrl = {};
        mfxExtMBQP mbqp = {};
        std::array<mfxExtBuffer*, 4> extBuffers = {};
        std::vector<mfxU8> qpBuffer;
        uint64_t frameId = 0;
        uint32_t streamId = 0;
        int64_t pts = 0;
        int qpMin = 0;
        int qpMax = 0;
        int qpAvg = 0;
        char reason[512] = {};
        MFX50RT_InternalEncodeControlEvent attachedEvent = {};
        bool staticReuseCandidate = false;
        int staticReuseConsecutiveFrames = 0;
        float staticReuseRiskScore = 0.0f;
    };

    struct PipelineOp {
        enum State { FREE, DECODING, DECODED, ENCODING } state = FREE;
        mfxFrameSurface1* surface = nullptr;
        mfxSyncPoint decodeSync = nullptr;
        mfxSyncPoint encodeSync = nullptr;
        std::vector<uint8_t> outputBuffer;
        mfxBitstream outputBs = {};
        PendingEncodeControl* ctrlSlot = nullptr;
        uint64_t decodeOrder = 0;
        uint64_t decodedOrder = 0;
        uint64_t encodeOrder = 0;
        int64_t pts = 0;
        void* userOpaque = nullptr;
        bool preprocessApplied = false;
    };
    std::vector<PendingEncodeControl> encodeCtrlSlots;
    std::vector<PipelineOp> pipelineOps;
    bool mbqpInitEnabled = false;

    std::vector<uint8_t> bitstreamBuffer;
    mfxBitstream inputBs = {};

    std::vector<uint8_t> outputBuffer;
    mfxBitstream outputBs = {};

    struct InputPacket {
        std::vector<uint8_t> data;
        int64_t pts = 0;
        int64_t dts = 0;
        int isKeyframe = 0;
        int endOfStream = 0;
        void* userOpaque = nullptr;
    };

    mutable std::mutex stateMu;
    mutable std::mutex inputMu;
    std::condition_variable inputCv;
    std::condition_variable idleCv;
    std::deque<InputPacket> inputQueue;
    std::thread worker;
    bool workerStarted = false;
    bool workerStop = false;
    bool asyncProcessing = false;

    uint64_t inputPackets = 0;
    uint64_t decodedFrames = 0;
    uint64_t encodedFrames = 0;
    uint64_t outputPackets = 0;
    uint64_t nextDecodeSubmitOrder = 0;
    uint64_t nextDecodeOutputOrder = 0;
    uint64_t nextDecodedReadyOrder = 0;
    uint64_t nextEncodeSubmitOrder = 0;
    uint64_t nextEncodeOutputOrder = 0;
    uint64_t bytesIn = 0;
    uint64_t bytesOut = 0;
    uint64_t decodeErrors = 0;
    uint64_t encodeErrors = 0;
    uint64_t droppedFrames = 0;

    uint64_t decodeUs = 0;
    uint64_t encodeSubmitUs = 0;
    uint64_t syncUs = 0;
    uint64_t preprocessUs = 0;
    uint64_t sceneAnalyzeUs = 0;

    mfx50rt::SceneAnalyzer sceneAnalyzer;
    uint64_t fallbackFrames = 0;
    uint64_t preprocessFrames = 0;
    uint64_t smoothScaleFrames = 0;
    uint64_t preDenoiseFrames = 0;
    uint64_t sceneAnalyzedFrames = 0;
    uint64_t adaptiveProfileSwitches = 0;
    uint64_t adaptiveQpFrames = 0;
    uint64_t mbqpFrames = 0;
    uint64_t mbqpFallbackFrames = 0;
    uint64_t mbqpSkippedFrames = 0;
    uint64_t mbqpInitEnabledFrames = 0;
    uint64_t staticReuseAppliedFrames = 0;
    uint64_t staticReuseReferenceFrames = 0;
    uint64_t asyncEnqueuedPackets = 0;
    uint64_t asyncProcessedPackets = 0;
    uint64_t backpressureEvents = 0;
    bool preprocessFallbackLogged = false;
    bool staticReuseReferenceValid = false;
    int staticReuseWidth = 0;
    int staticReuseHeight = 0;
    std::vector<uint8_t> staticReuseY;
    std::vector<uint8_t> staticReuseUV;

    std::string lastError;
};

void resetBitstreamPointers(RouteContext& r) {
    r.inputBs.Data = r.bitstreamBuffer.data();
    r.inputBs.MaxLength = static_cast<mfxU32>(r.bitstreamBuffer.size());
}

void compactInput(RouteContext& r) {
    if (r.inputBs.DataOffset == 0) return;
    if (r.inputBs.DataLength > 0) {
        std::memmove(r.bitstreamBuffer.data(),
                     r.bitstreamBuffer.data() + r.inputBs.DataOffset,
                     r.inputBs.DataLength);
    }
    r.inputBs.DataOffset = 0;
    resetBitstreamPointers(r);
}

void appendInput(RouteContext& r, const uint8_t* data, size_t size) {
    compactInput(r);
    if (r.inputBs.DataLength + size > r.bitstreamBuffer.size()) {
        size_t nextSize = std::max(r.bitstreamBuffer.size() * 2, static_cast<size_t>(r.inputBs.DataLength + size));
        r.bitstreamBuffer.resize(nextSize);
        resetBitstreamPointers(r);
    }
    std::memcpy(r.bitstreamBuffer.data() + r.inputBs.DataLength, data, size);
    r.inputBs.DataLength += static_cast<mfxU32>(size);
    resetBitstreamPointers(r);
}

void resetOutput(RouteContext& r) {
    r.outputBs = {};
    r.outputBs.Data = r.outputBuffer.data();
    r.outputBs.MaxLength = static_cast<mfxU32>(r.outputBuffer.size());
    r.outputBs.TimeStamp = static_cast<mfxU64>(MFX_TIMESTAMP_UNKNOWN);
    r.outputBs.DecodeTimeStamp = MFX_TIMESTAMP_UNKNOWN;
}

void resetOutput(RouteContext::PipelineOp& op) {
    op.outputBs = {};
    op.outputBs.Data = op.outputBuffer.data();
    op.outputBs.MaxLength = static_cast<mfxU32>(op.outputBuffer.size());
    op.outputBs.TimeStamp = static_cast<mfxU64>(MFX_TIMESTAMP_UNKNOWN);
    op.outputBs.DecodeTimeStamp = MFX_TIMESTAMP_UNKNOWN;
}

void initializePipelineOps(RouteContext& r) {
    const int depth = std::max<int>(1, r.encPar.AsyncDepth > 0 ? r.encPar.AsyncDepth : 1);
    r.pipelineOps.resize(static_cast<size_t>(depth));
    for (auto& op : r.pipelineOps) {
        op.outputBuffer.assign(kOutputBitstreamBytes, 0);
        resetOutput(op);
        op.state = RouteContext::PipelineOp::FREE;
    }
}

} // namespace

struct MFX50RT_Context {
    MFX50RT_Config cfg = {};
    MFX50RT_AlgoConfig algoCfg = {};
    std::vector<std::unique_ptr<RouteContext>> routes;
    mfx50rt::OutputQueue outputQueue;
    MFX50RT_OutputCallback callback = nullptr;
    void* callbackOpaque = nullptr;
    MFX50RT_LogCallback logCallback = nullptr;
    void* logOpaque = nullptr;
    MFX50RT_InternalFrameDecisionCallback frameDecisionCallback = nullptr;
    void* frameDecisionOpaque = nullptr;
    MFX50RT_InternalEncodeControlEventCallback encodeControlEventCallback = nullptr;
    void* encodeControlEventOpaque = nullptr;
    std::mutex mu;
    std::mutex schedulerMu;
    std::condition_variable schedulerCv;
    std::vector<std::thread> schedulerThreads;
    bool schedulerStarted = false;
    bool schedulerStop = false;
    uint64_t startUs = 0;
    int lastErrorCode = MFX50_OK;
    std::string lastError;
};

namespace {

struct CallbackBatch {
    MFX50RT_OutputCallback callback = nullptr;
    void* opaque = nullptr;
    std::deque<mfx50rt::OutputPacket> packets;
};

void emitLog(MFX50RT_Context* ctx, int level, const std::string& msg) {
    if (!ctx || msg.empty()) return;
    MFX50RT_LogCallback cb = nullptr;
    void* opaque = nullptr;
    {
        std::lock_guard<std::mutex> lock(ctx->mu);
        cb = ctx->logCallback;
        opaque = ctx->logOpaque;
    }
    if (cb) cb(level, msg.c_str(), opaque);
}

void warnPreprocessFallback(MFX50RT_Context* ctx, RouteContext& r, const std::string& msg) {
    r.fallbackFrames++;
    if (r.preprocessFallbackLogged) return;
    r.preprocessFallbackLogged = true;
    emitLog(ctx, MFX50RT_LOG_WARN, msg);
}

void setError(MFX50RT_Context* ctx, int code, const std::string& msg) {
    MFX50RT_LogCallback cb = nullptr;
    void* opaque = nullptr;
    if (ctx) {
        std::lock_guard<std::mutex> lock(ctx->mu);
        ctx->lastErrorCode = code;
        ctx->lastError = msg;
        cb = ctx->logCallback;
        opaque = ctx->logOpaque;
    }
    setGlobalError(msg);
    if (cb && !msg.empty()) cb(MFX50RT_LOG_ERROR, msg.c_str(), opaque);
}

void deliverOutput(MFX50RT_Context* ctx, const mfx50rt::OutputPacket& pkt) {
    ctx->outputQueue.push(pkt);
}

CallbackBatch prepareCallbacks(MFX50RT_Context* ctx) {
    CallbackBatch batch;
    {
        std::lock_guard<std::mutex> lock(ctx->mu);
        if (ctx->cfg.output_mode != MFX50_OUTPUT_CALLBACK || !ctx->callback) {
            return batch;
        }
        batch.callback = ctx->callback;
        batch.opaque = ctx->callbackOpaque;
    }
    batch.packets = ctx->outputQueue.drainAll();
    return batch;
}

void invokeCallbacks(const CallbackBatch& batch) {
    if (!batch.callback) return;
    for (const mfx50rt::OutputPacket& pkt : batch.packets) {
        MFX50RT_EncodedPacket out = {};
        out.struct_size = sizeof(out);
        out.stream_id = pkt.streamId;
        out.data = const_cast<uint8_t*>(pkt.data.data());
        out.size = pkt.data.size();
        out.capacity = pkt.data.size();
        out.pts = pkt.pts;
        out.dts = pkt.dts;
        out.is_keyframe = pkt.isKeyframe;
        out.frame_type = pkt.frameType;
        out.user_opaque = pkt.userOpaque;
        batch.callback(&out, batch.opaque);
    }
}

int framePitch(const mfxFrameData& data) {
    return (static_cast<int>(data.PitchHigh) << 16) | static_cast<int>(data.PitchLow);
}

uint8_t* nv12UvPtr(mfxFrameSurface1* surface) {
    if (!surface) return nullptr;
    return surface->Data.UV ? surface->Data.UV : surface->Data.U;
}

void copySurfaceToStaticReuseReference(RouteContext& r,
                                       const uint8_t* y,
                                       const uint8_t* uv,
                                       int width,
                                       int height,
                                       int pitch) {
    const int uvRows = (height + 1) / 2;
    r.staticReuseY.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
    r.staticReuseUV.assign(static_cast<size_t>(width) * static_cast<size_t>(uvRows), 0);
    for (int row = 0; row < height; ++row) {
        std::memcpy(r.staticReuseY.data() + static_cast<size_t>(row) * width,
                    y + static_cast<size_t>(row) * pitch,
                    static_cast<size_t>(width));
    }
    for (int row = 0; row < uvRows; ++row) {
        std::memcpy(r.staticReuseUV.data() + static_cast<size_t>(row) * width,
                    uv + static_cast<size_t>(row) * pitch,
                    static_cast<size_t>(width));
    }
    r.staticReuseWidth = width;
    r.staticReuseHeight = height;
    r.staticReuseReferenceValid = true;
    r.staticReuseReferenceFrames++;
}

void copyStaticReuseReferenceToSurface(const RouteContext& r,
                                       uint8_t* y,
                                       uint8_t* uv,
                                       int width,
                                       int height,
                                       int pitch) {
    const int uvRows = (height + 1) / 2;
    for (int row = 0; row < height; ++row) {
        std::memcpy(y + static_cast<size_t>(row) * pitch,
                    r.staticReuseY.data() + static_cast<size_t>(row) * width,
                    static_cast<size_t>(width));
    }
    for (int row = 0; row < uvRows; ++row) {
        std::memcpy(uv + static_cast<size_t>(row) * pitch,
                    r.staticReuseUV.data() + static_cast<size_t>(row) * width,
                    static_cast<size_t>(width));
    }
}

void applyStaticReuseIfEnabled(MFX50RT_Context* ctx,
                               RouteContext& r,
                               mfxFrameSurface1* surface,
                               const RouteContext::PendingEncodeControl* slot) {
    if (!ctx || !ctx->cfg.enable_static_reuse || !surface || !slot) return;
    if (slot->staticReuseConsecutiveFrames <= 0) {
        r.staticReuseReferenceValid = false;
        return;
    }
    if (surface->Info.FourCC != MFX_FOURCC_NV12) {
        r.staticReuseReferenceValid = false;
        return;
    }

    bool mapped = false;
    if ((!surface->Data.Y || !nv12UvPtr(surface)) &&
        surface->FrameInterface && surface->FrameInterface->Map) {
        mfxStatus st = surface->FrameInterface->Map(surface, MFX_MAP_READ_WRITE);
        if (st < MFX_ERR_NONE) {
            warnPreprocessFallback(ctx, r, "static reuse skipped: surface Map failed: " + mfxStatusName(st));
            r.staticReuseReferenceValid = false;
            return;
        }
        mapped = true;
    }

    uint8_t* y = surface->Data.Y;
    uint8_t* uv = nv12UvPtr(surface);
    const int pitch = framePitch(surface->Data);
    const int width = surface->Info.CropW > 0 ? surface->Info.CropW : surface->Info.Width;
    const int height = surface->Info.CropH > 0 ? surface->Info.CropH : surface->Info.Height;
    if (y && uv && pitch >= width && width > 0 && height > 0) {
        y += static_cast<size_t>(surface->Info.CropY) * pitch + surface->Info.CropX;
        uv += static_cast<size_t>(surface->Info.CropY / 2) * pitch +
              static_cast<size_t>(surface->Info.CropX / 2) * 2;
        const bool same_shape =
            r.staticReuseReferenceValid &&
            r.staticReuseWidth == width &&
            r.staticReuseHeight == height;
        if (slot->staticReuseCandidate && same_shape) {
            copyStaticReuseReferenceToSurface(r, y, uv, width, height, pitch);
            r.staticReuseAppliedFrames++;
        } else {
            copySurfaceToStaticReuseReference(r, y, uv, width, height, pitch);
        }
    } else {
        r.staticReuseReferenceValid = false;
    }

    if (mapped && surface->FrameInterface && surface->FrameInterface->Unmap) {
        requireMfx(surface->FrameInterface->Unmap(surface), "mfxFrameSurfaceInterface::Unmap(static_reuse)");
    }
}

void applyPreprocessIfEnabled(MFX50RT_Context* ctx, RouteContext& r, mfxFrameSurface1* surface) {
    MFX50RT_AlgoConfig cfg = {};
    {
        std::lock_guard<std::mutex> lock(ctx->mu);
        cfg = ctx->algoCfg;
    }
    if (!cfg.enable_preprocess || (!cfg.enable_smooth_scale && !cfg.enable_pre_denoise)) {
        return;
    }
    if (!surface) {
        warnPreprocessFallback(ctx, r, "preprocess skipped: decoded surface is null");
        return;
    }
    if (surface->Info.FourCC != MFX_FOURCC_NV12) {
        warnPreprocessFallback(ctx, r, "preprocess skipped: decoded surface is not NV12");
        return;
    }

    const uint64_t start = nowUs();
    bool mapped = false;
    if ((!surface->Data.Y || !surface->Data.UV) && surface->FrameInterface && surface->FrameInterface->Map) {
        mfxStatus st = surface->FrameInterface->Map(surface, MFX_MAP_READ_WRITE);
        if (st < MFX_ERR_NONE) {
            warnPreprocessFallback(ctx, r, "preprocess skipped: surface Map failed: " + mfxStatusName(st));
            return;
        }
        mapped = true;
    }

    int rc = MFX50_OK;
    uint8_t* y = surface->Data.Y;
    uint8_t* uv = surface->Data.UV ? surface->Data.UV : surface->Data.U;
    const int pitch = framePitch(surface->Data);
    const int width = surface->Info.CropW > 0 ? surface->Info.CropW : surface->Info.Width;
    const int height = surface->Info.CropH > 0 ? surface->Info.CropH : surface->Info.Height;
    if (y && uv && pitch > 0) {
        y += static_cast<size_t>(surface->Info.CropY) * pitch + surface->Info.CropX;
        uv += static_cast<size_t>(surface->Info.CropY / 2) * pitch +
              static_cast<size_t>(surface->Info.CropX / 2) * 2;
        if (cfg.enable_pre_denoise) {
            rc = mfx50_preprocess_denoise_nv12(
                y, uv, width, height, pitch, pitch, cfg.pre_denoise_strength);
        }
        if (rc == MFX50_OK && cfg.enable_smooth_scale) {
            rc = mfx50_preprocess_semantic_smooth_nv12(
                y, uv, width, height, pitch, pitch, cfg.smooth_scale_factor);
        }
    }
    else {
        rc = MFX50_ERR_INVALID_ARG;
    }

    if (mapped && surface->FrameInterface && surface->FrameInterface->Unmap) {
        mfxStatus st = surface->FrameInterface->Unmap(surface);
        requireMfx(st, "mfxFrameSurfaceInterface::Unmap(preprocess)");
    }

    if (rc != MFX50_OK) {
        warnPreprocessFallback(ctx, r, "preprocess skipped: invalid mapped NV12 surface");
        return;
    }

    r.preprocessUs += elapsedUs(start);
    r.preprocessFrames++;
    if (cfg.enable_smooth_scale) r.smoothScaleFrames++;
    if (cfg.enable_pre_denoise) r.preDenoiseFrames++;
}

void analyzeSceneIfEnabled(MFX50RT_Context* ctx, RouteContext& r, mfxFrameSurface1* surface, int64_t pts) {
    MFX50RT_AlgoConfig cfg = {};
    {
        std::lock_guard<std::mutex> lock(ctx->mu);
        cfg = ctx->algoCfg;
    }
    if (!cfg.enable_scene_analyzer) return;
    if (!surface || surface->Info.FourCC != MFX_FOURCC_NV12) {
        return;
    }

    const uint64_t start = nowUs();
    bool mapped = false;
    if (!surface->Data.Y && surface->FrameInterface && surface->FrameInterface->Map) {
        mfxStatus st = surface->FrameInterface->Map(surface, MFX_MAP_READ);
        if (st < MFX_ERR_NONE) {
            warnPreprocessFallback(ctx, r, "scene analyzer skipped: surface Map failed: " + mfxStatusName(st));
            return;
        }
        mapped = true;
    }

    const int pitch = framePitch(surface->Data);
    const int width = surface->Info.CropW > 0 ? surface->Info.CropW : surface->Info.Width;
    const int height = surface->Info.CropH > 0 ? surface->Info.CropH : surface->Info.Height;
    const uint8_t* y = surface->Data.Y;
    if (y && pitch > 0) {
        y += static_cast<size_t>(surface->Info.CropY) * pitch + surface->Info.CropX;
        const mfx50rt::FrameAnalysis analysis =
            r.sceneAnalyzer.analyzeNv12YPlane(y, width, height, pitch, pts);
        r.sceneAnalyzedFrames++;
        emitLog(ctx,
                MFX50RT_LOG_DEBUG,
                "scene frame=" + std::to_string(analysis.frameIndex) +
                    " pts=" + std::to_string(analysis.pts) +
                    " flat=" + std::to_string(analysis.flatScore) +
                    " motion=" + std::to_string(analysis.motionScore) +
                    " noise=" + std::to_string(analysis.noiseScore) +
                    " edge=" + std::to_string(analysis.edgeScore) +
                    " scene_cut=" + std::to_string(analysis.sceneCutScore) +
                    " hard=" + std::to_string(analysis.hardScore) +
                    " suggest=" + std::to_string(analysis.suggestedProfile));
    }

    if (mapped && surface->FrameInterface && surface->FrameInterface->Unmap) {
        mfxStatus st = surface->FrameInterface->Unmap(surface);
        requireMfx(st, "mfxFrameSurfaceInterface::Unmap(scene)");
    }
    r.sceneAnalyzeUs += elapsedUs(start);
}

void initializeRoute(MFX50RT_Context* ctx, RouteContext& r) {
    r.devicePath = defaultDeviceForRoute(ctx->cfg, r.id);
    r.va.reset(new VaDevice(r.devicePath));
    r.vpl.reset(new VplSession());
    requireMfx(MFXVideoCORE_SetHandle(r.vpl->session, MFX_HANDLE_VA_DISPLAY, r.va->display),
               "MFXVideoCORE_SetHandle");

    r.bitstreamBuffer.assign(kInitialBitstreamBytes, 0);
    r.inputBs = {};
    resetBitstreamPointers(r);

    r.outputBuffer.assign(kOutputBitstreamBytes, 0);
    resetOutput(r);
}

void bindEncoderExtParams(RouteContext& r, bool useCodingOption2, bool useCodingOption3) {
    r.encPar.NumExtParam = 0;
    if (useCodingOption2) {
        r.encExtParams[r.encPar.NumExtParam++] =
            reinterpret_cast<mfxExtBuffer*>(&r.codingOption2);
    }
    if (useCodingOption3) {
        r.encExtParams[r.encPar.NumExtParam++] =
            reinterpret_cast<mfxExtBuffer*>(&r.codingOption3);
    }
    r.encPar.ExtParam = r.encPar.NumExtParam > 0 ? r.encExtParams : nullptr;
}

int initDecoderEncoderIfNeeded(MFX50RT_Context* ctx, RouteContext& r) {
    if (r.decoderReady && r.encoderReady) return MFX50_OK;
    if (r.inputBs.DataLength == 0) return MFX50_ERR_NEED_MORE_INPUT;

    r.decPar = {};
    r.decPar.AsyncDepth = static_cast<mfxU16>(ctx->cfg.async_depth > 0 ? ctx->cfg.async_depth : 4);
    r.decPar.mfx.CodecId = codecId(ctx->cfg.input_codec);

    mfxU32 lengthBeforeHeader = r.inputBs.DataLength;
    mfxStatus st = MFXVideoDECODE_DecodeHeader(r.vpl->session, &r.inputBs, &r.decPar);
    if (st == MFX_ERR_MORE_DATA) {
        r.inputBs.DataOffset = 0;
        r.inputBs.DataLength = lengthBeforeHeader;
        return MFX50_ERR_NEED_MORE_INPUT;
    }
    requireMfx(st, "MFXVideoDECODE_DecodeHeader");
    r.inputBs.DataOffset = 0;
    r.inputBs.DataLength = lengthBeforeHeader;

    r.decPar.AsyncDepth = static_cast<mfxU16>(ctx->cfg.async_depth > 0 ? ctx->cfg.async_depth : 4);
    r.decPar.IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;
    if (ctx->cfg.fps_num > 0 && ctx->cfg.fps_den > 0) {
        r.decPar.mfx.FrameInfo.FrameRateExtN = static_cast<mfxU32>(ctx->cfg.fps_num);
        r.decPar.mfx.FrameInfo.FrameRateExtD = static_cast<mfxU32>(ctx->cfg.fps_den);
    }
    if (r.decPar.mfx.FrameInfo.PicStruct == 0) {
        r.decPar.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    }
    {
        mfxVideoParam queried = r.decPar;
        requireMfx(MFXVideoDECODE_Query(r.vpl->session, &r.decPar, &queried),
                   "MFXVideoDECODE_Query");
        r.decPar = queried;
        r.decPar.IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;
    }
    requireMfx(MFXVideoDECODE_Init(r.vpl->session, &r.decPar), "MFXVideoDECODE_Init");
    r.decoderReady = true;

    const ResolvedEncodeProfile profile = resolveProfile(ctx->cfg);
    const int fpsNum = ctx->cfg.fps_num > 0 ? ctx->cfg.fps_num : 30;
    const int fpsDen = ctx->cfg.fps_den > 0 ? ctx->cfg.fps_den : 1;

    r.encPar = {};
    r.encPar.AsyncDepth = static_cast<mfxU16>(ctx->cfg.async_depth > 0 ? ctx->cfg.async_depth : 4);
    r.encPar.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY;
    r.encPar.mfx.CodecId = MFX_CODEC_HEVC;
    r.encPar.mfx.TargetUsage = static_cast<mfxU16>(profile.targetUsage);
    r.encPar.mfx.RateControlMethod = MFX_RATECONTROL_CQP;
    r.encPar.mfx.QPI = static_cast<mfxU16>(profile.qpi);
    r.encPar.mfx.QPP = static_cast<mfxU16>(profile.qpp);
    r.encPar.mfx.QPB = static_cast<mfxU16>(profile.qpb);
    r.encPar.mfx.GopPicSize = static_cast<mfxU16>(profile.gop);
    r.encPar.mfx.GopRefDist = static_cast<mfxU16>(profile.gopRefDist);
    if (profile.numRefFrame > 0) {
        r.encPar.mfx.NumRefFrame = static_cast<mfxU16>(profile.numRefFrame);
    }
    r.encPar.mfx.FrameInfo = r.decPar.mfx.FrameInfo;
    r.encPar.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    r.encPar.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    r.encPar.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    r.encPar.mfx.FrameInfo.FrameRateExtN = static_cast<mfxU32>(fpsNum);
    r.encPar.mfx.FrameInfo.FrameRateExtD = static_cast<mfxU32>(fpsDen);

    r.codingOption2 = {};
    r.codingOption3 = {};
    const bool useCodingOption2 = profile.brefType == 1 || profile.brefType == 2;
    if (useCodingOption2) {
        r.codingOption2.Header.BufferId = MFX_EXTBUFF_CODING_OPTION2;
        r.codingOption2.Header.BufferSz = sizeof(r.codingOption2);
        r.codingOption2.BRefType = profile.brefType == 2 ? MFX_B_REF_PYRAMID : MFX_B_REF_OFF;
    }

    const bool wantMbqp = ctx->frameDecisionCallback != nullptr;
    if (wantMbqp) {
        r.codingOption3.Header.BufferId = MFX_EXTBUFF_CODING_OPTION3;
        r.codingOption3.Header.BufferSz = sizeof(r.codingOption3);
        r.codingOption3.EnableMBQP = MFX_CODINGOPTION_ON;
    }
    bindEncoderExtParams(r, useCodingOption2, wantMbqp);

    auto queryEncoder = [&](bool withMbqp, std::string* failure) -> mfxStatus {
        bindEncoderExtParams(r, useCodingOption2, withMbqp);
        mfxVideoParam queried = r.encPar;
        mfxExtCodingOption2 queriedOption2 = r.codingOption2;
        mfxExtCodingOption3 queriedOption3 = r.codingOption3;
        mfxExtBuffer* queriedExtParams[2] = {};
        mfxU16 numExt = 0;
        if (useCodingOption2) {
            queriedExtParams[numExt++] = reinterpret_cast<mfxExtBuffer*>(&queriedOption2);
        }
        if (withMbqp) {
            queriedExtParams[numExt++] = reinterpret_cast<mfxExtBuffer*>(&queriedOption3);
        }
        queried.ExtParam = numExt > 0 ? queriedExtParams : nullptr;
        queried.NumExtParam = numExt;

        mfxStatus querySt = MFXVideoENCODE_Query(r.vpl->session, &r.encPar, &queried);
        if (querySt < MFX_ERR_NONE) {
            if (failure) *failure = "MFXVideoENCODE_Query failed: " + mfxStatusName(querySt);
            return querySt;
        }
        if (withMbqp && queriedOption3.EnableMBQP == MFX_CODINGOPTION_OFF) {
            if (failure) *failure = "MFXVideoENCODE_Query disabled EnableMBQP";
            return MFX_ERR_UNSUPPORTED;
        }

        r.encPar = queried;
        if (useCodingOption2) r.codingOption2 = queriedOption2;
        if (withMbqp) r.codingOption3 = queriedOption3;
        bindEncoderExtParams(r, useCodingOption2, withMbqp);
        return MFX_ERR_NONE;
    };

    r.mbqpInitEnabled = false;
    std::string mbqpFailure;
    if (wantMbqp && queryEncoder(true, &mbqpFailure) >= MFX_ERR_NONE) {
        mfxStatus initSt = MFXVideoENCODE_Init(r.vpl->session, &r.encPar);
        if (initSt >= MFX_ERR_NONE) {
            r.mbqpInitEnabled = true;
        } else {
            mbqpFailure = "MFXVideoENCODE_Init with MBQP failed: " + mfxStatusName(initSt);
        }
    }
    if (!r.mbqpInitEnabled) {
        if (wantMbqp && !mbqpFailure.empty()) {
            r.mbqpFallbackFrames++;
            emitLog(ctx, MFX50RT_LOG_WARN, "MBQP init disabled for route " +
                std::to_string(r.id) + ": " + mbqpFailure);
        }
        requireMfx(queryEncoder(false, nullptr), "MFXVideoENCODE_Query");
        requireMfx(MFXVideoENCODE_Init(r.vpl->session, &r.encPar), "MFXVideoENCODE_Init");
    }
    r.encodeCtrlSlots.resize(static_cast<size_t>(r.encPar.AsyncDepth) + 4);
    initializePipelineOps(r);
    r.encoderReady = true;
    return MFX50_OK;
}

void pushEncodedFromBitstream(MFX50RT_Context* ctx,
                              RouteContext& r,
                              mfxBitstream& bitstream,
                              int64_t pts,
                              void* userOpaque) {
    if (bitstream.DataLength == 0) {
        bitstream.DataOffset = 0;
        return;
    }
    int64_t outPts = pts;
    if (bitstream.TimeStamp != static_cast<mfxU64>(MFX_TIMESTAMP_UNKNOWN)) {
        outPts = static_cast<int64_t>(bitstream.TimeStamp);
    }
    mfx50rt::OutputPacket out;
    out.streamId = r.id;
    out.data.assign(bitstream.Data + bitstream.DataOffset,
                    bitstream.Data + bitstream.DataOffset + bitstream.DataLength);
    out.pts = outPts;
    out.dts = static_cast<int64_t>(r.outputPackets);
    out.isKeyframe = containsHevcKeyframe(out.data.data(), out.data.size()) ? 1 : 0;
    out.frameType = out.isKeyframe ? 1 : 0;
    out.userOpaque = userOpaque;
    r.bytesOut += out.data.size();
    r.outputPackets++;
    deliverOutput(ctx, out);
    bitstream.DataLength = 0;
    bitstream.DataOffset = 0;
}

RouteContext::PendingEncodeControl* acquireEncodeControlSlot(RouteContext& r) {
    if (r.encodeCtrlSlots.empty()) r.encodeCtrlSlots.resize(8);
    for (auto& slot : r.encodeCtrlSlots) {
        if (!slot.inUse) {
            slot.inUse = true;
            slot.hasMbqp = false;
            slot.fallback = false;
            slot.ctrl = {};
            slot.mbqp = {};
            slot.extBuffers.fill(nullptr);
            slot.frameId = 0;
            slot.streamId = static_cast<uint32_t>(r.id);
            slot.pts = 0;
            slot.qpMin = 0;
            slot.qpMax = 0;
            slot.qpAvg = 0;
            slot.reason[0] = '\0';
            slot.attachedEvent = {};
            slot.staticReuseCandidate = false;
            slot.staticReuseConsecutiveFrames = 0;
            slot.staticReuseRiskScore = 0.0f;
            return &slot;
        }
    }
    r.encodeCtrlSlots.emplace_back();
    auto& slot = r.encodeCtrlSlots.back();
    slot.inUse = true;
    slot.streamId = static_cast<uint32_t>(r.id);
    return &slot;
}

void releaseEncodeControlSlot(RouteContext::PendingEncodeControl* slot) {
    if (!slot) return;
    slot->inUse = false;
    slot->hasMbqp = false;
    slot->fallback = false;
}

void emitEncodeControlEvent(MFX50RT_Context* ctx,
                            const MFX50RT_InternalEncodeControlEvent& event) {
    MFX50RT_InternalEncodeControlEventCallback cb = nullptr;
    void* opaque = nullptr;
    {
        std::lock_guard<std::mutex> lock(ctx->mu);
        cb = ctx->encodeControlEventCallback;
        opaque = ctx->encodeControlEventOpaque;
    }
    if (cb) cb(opaque, &event);
}

void fillEncodeControlEvent(MFX50RT_InternalEncodeControlEvent* event,
                            const RouteContext& r,
                            const RouteContext::PendingEncodeControl* slot,
                            const MFX50RT_InternalEncodeDecision& decision,
                            bool attached,
                            const char* reason) {
    if (!event) return;
    std::memset(event, 0, sizeof(*event));
    event->size = sizeof(*event);
    event->version = MFX50RT_INTERNAL_API_VERSION;
    event->stream_id = static_cast<uint32_t>(r.id);
    event->frame_id = slot ? slot->frameId : decision.frame_anchor_qp;
    event->pts = slot ? slot->pts : 0;
    event->requested_strategy = decision.strategy;
    event->actual_strategy = attached ? MFX50RT_INTERNAL_CONTROL_MBQP : MFX50RT_INTERNAL_CONTROL_GLOBAL;
    event->mbqp_init_enabled = r.mbqpInitEnabled ? 1 : 0;
    event->mbqp_requested = decision.has_mbqp ? 1 : 0;
    event->mbqp_attached = attached ? 1 : 0;
    event->mbqp_block_size = decision.mbqp_block_size;
    event->mbqp_pitch = decision.mbqp_pitch;
    event->mbqp_block_cols = decision.mbqp_block_cols;
    event->mbqp_block_rows = decision.mbqp_block_rows;
    event->mbqp_num_qp_alloc = decision.mbqp_num_qp_alloc;
    event->qp_min = slot ? slot->qpMin : decision.spatial_min_qp;
    event->qp_max = slot ? slot->qpMax : decision.spatial_max_qp;
    event->qp_avg = slot ? slot->qpAvg : decision.spatial_avg_qp;
    event->fallback = attached ? 0 : 1;
    copyString(event->reason,
               sizeof(event->reason),
               reason && reason[0] ? reason : decision.reason);
}

bool buildMbqpControlFromDecision(const MFX50RT_InternalEncodeDecision& decision,
                                  RouteContext::PendingEncodeControl* slot,
                                  std::string* reason) {
    if (!slot) return false;
    if (!decision.has_mbqp ||
        decision.strategy != MFX50RT_INTERNAL_CONTROL_MBQP) {
        if (reason) *reason = decision.reason[0] ? decision.reason : "callback did not request MBQP";
        return false;
    }
    if (decision.mbqp_block_cols <= 0 || decision.mbqp_block_rows <= 0 ||
        decision.mbqp_pitch < decision.mbqp_block_cols ||
        decision.mbqp_block_size <= 0 ||
        !decision.mbqp_qp_buffer) {
        if (reason) *reason = "invalid MBQP geometry";
        return false;
    }
    const uint64_t expected =
        static_cast<uint64_t>(decision.mbqp_pitch) *
        static_cast<uint64_t>(decision.mbqp_block_rows);
    if (expected == 0 ||
        expected > static_cast<uint64_t>(decision.mbqp_qp_capacity) ||
        decision.mbqp_num_qp_alloc < expected) {
        if (reason) *reason = "invalid MBQP allocation size";
        return false;
    }
    slot->qpBuffer.assign(decision.mbqp_qp_buffer,
                          decision.mbqp_qp_buffer + static_cast<size_t>(expected));

    int qpMin = 51;
    int qpMax = 1;
    uint64_t qpSum = 0;
    uint64_t qpCount = 0;
    for (int row = 0; row < decision.mbqp_block_rows; ++row) {
        const int rowStart = row * decision.mbqp_pitch;
        for (int col = 0; col < decision.mbqp_block_cols; ++col) {
            const size_t idx = static_cast<size_t>(rowStart + col);
            int qp = slot->qpBuffer[idx];
            if (qp < 1) qp = 1;
            if (qp > 51) qp = 51;
            slot->qpBuffer[idx] = static_cast<mfxU8>(qp);
            qpMin = std::min(qpMin, qp);
            qpMax = std::max(qpMax, qp);
            qpSum += static_cast<uint64_t>(qp);
            qpCount++;
        }
    }
    if (qpCount == 0) {
        if (reason) *reason = "empty MBQP map";
        return false;
    }

    slot->mbqp = {};
    slot->mbqp.Header.BufferId = MFX_EXTBUFF_MBQP;
    slot->mbqp.Header.BufferSz = sizeof(slot->mbqp);
    slot->mbqp.Pitch = static_cast<mfxU32>(decision.mbqp_pitch);
    slot->mbqp.Mode = MFX_MBQP_MODE_QP_VALUE;
    slot->mbqp.BlockSize = static_cast<mfxU16>(decision.mbqp_block_size);
    slot->mbqp.NumQPAlloc = static_cast<mfxU32>(slot->qpBuffer.size());
    slot->mbqp.QP = slot->qpBuffer.data();

    slot->extBuffers[0] = reinterpret_cast<mfxExtBuffer*>(&slot->mbqp);
    slot->ctrl = {};
    slot->ctrl.NumExtParam = 1;
    slot->ctrl.ExtParam = slot->extBuffers.data();
    if (decision.force_idr) {
        slot->ctrl.FrameType = MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR | MFX_FRAMETYPE_REF;
    }
    slot->hasMbqp = true;
    slot->qpMin = qpMin;
    slot->qpMax = qpMax;
    slot->qpAvg = static_cast<int>(qpSum / qpCount);
    copyString(slot->reason,
               sizeof(slot->reason),
               decision.reason[0] ? decision.reason : "MBQP attached");
    return true;
}

RouteContext::PendingEncodeControl* prepareEncodeControl(MFX50RT_Context* ctx,
                                                         RouteContext& r,
                                                         mfxFrameSurface1* surface,
                                                         int64_t pts) {
    MFX50RT_InternalFrameDecisionCallback cb = nullptr;
    void* opaque = nullptr;
    {
        std::lock_guard<std::mutex> lock(ctx->mu);
        cb = ctx->frameDecisionCallback;
        opaque = ctx->frameDecisionOpaque;
    }
    if (!cb || !surface || !r.mbqpInitEnabled) return nullptr;

    RouteContext::PendingEncodeControl* slot = acquireEncodeControlSlot(r);
    const int width = surface->Info.CropW > 0 ? surface->Info.CropW : surface->Info.Width;
    const int height = surface->Info.CropH > 0 ? surface->Info.CropH : surface->Info.Height;
    const int blockCols = ceilDiv(width, kMbqpBlockSize);
    const int blockRows = ceilDiv(height, kMbqpBlockSize);
    const int pitch = blockCols;
    const uint64_t capacity64 = static_cast<uint64_t>(pitch) * static_cast<uint64_t>(blockRows);
    if (blockCols <= 0 || blockRows <= 0 || capacity64 > UINT32_MAX) {
        r.mbqpFallbackFrames++;
        releaseEncodeControlSlot(slot);
        return nullptr;
    }
    slot->qpBuffer.assign(static_cast<size_t>(capacity64), 0);
    slot->frameId = r.decodedFrames > 0 ? r.decodedFrames - 1 : r.encodedFrames;
    slot->streamId = static_cast<uint32_t>(r.id);
    slot->pts = pts;

    bool mapped = false;
    if (!surface->Data.Y && surface->FrameInterface && surface->FrameInterface->Map) {
        mfxStatus mapSt = surface->FrameInterface->Map(surface, MFX_MAP_READ);
        if (mapSt >= MFX_ERR_NONE) {
            mapped = true;
        }
    }

    const int yPitch = framePitch(surface->Data);
    const uint8_t* y = surface->Data.Y;
    if (y && yPitch > 0) {
        y += static_cast<size_t>(surface->Info.CropY) * yPitch + surface->Info.CropX;
    }

    MFX50RT_InternalSurfaceView view{};
    view.size = sizeof(view);
    view.version = MFX50RT_INTERNAL_API_VERSION;
    view.stream_id = static_cast<uint32_t>(r.id);
    view.frame_id = slot->frameId;
    view.pts = pts;
    view.dts = pts;
    view.width = width;
    view.height = height;
    view.fourcc = static_cast<int32_t>(surface->Info.FourCC);
    view.y_ptr = y;
    view.y_pitch = yPitch;

    MFX50RT_InternalEncodeDecision decision{};
    decision.size = sizeof(decision);
    decision.version = MFX50RT_INTERNAL_API_VERSION;
    decision.strategy = MFX50RT_INTERNAL_CONTROL_GLOBAL;
    decision.mbqp_block_size = kMbqpBlockSize;
    decision.mbqp_pitch = pitch;
    decision.mbqp_block_cols = blockCols;
    decision.mbqp_block_rows = blockRows;
    decision.mbqp_num_qp_alloc = static_cast<uint32_t>(capacity64);
    decision.mbqp_qp_buffer = slot->qpBuffer.data();
    decision.mbqp_qp_capacity = static_cast<uint32_t>(slot->qpBuffer.size());

    const int cbRc = cb(opaque, &view, &decision);
    slot->staticReuseCandidate = decision.static_reuse_candidate != 0;
    slot->staticReuseConsecutiveFrames = decision.static_reuse_consecutive_frames;
    slot->staticReuseRiskScore = decision.static_reuse_risk_score;
    if (mapped && surface->FrameInterface && surface->FrameInterface->Unmap) {
        requireMfx(surface->FrameInterface->Unmap(surface), "mfxFrameSurfaceInterface::Unmap(mbqp)");
    }

    std::string reason;
    const bool attached = cbRc == 0 && buildMbqpControlFromDecision(decision, slot, &reason);
    const bool keepForStaticReuse = ctx->cfg.enable_static_reuse && cbRc == 0;
    if (!attached) {
        if (!keepForStaticReuse) {
            r.mbqpFallbackFrames++;
            r.mbqpSkippedFrames++;
            MFX50RT_InternalEncodeControlEvent event{};
            fillEncodeControlEvent(&event, r, slot, decision, false, reason.c_str());
            emitEncodeControlEvent(ctx, event);
            releaseEncodeControlSlot(slot);
            return nullptr;
        }
        if (decision.has_mbqp) {
            r.mbqpFallbackFrames++;
            r.mbqpSkippedFrames++;
            MFX50RT_InternalEncodeControlEvent event{};
            fillEncodeControlEvent(&event, r, slot, decision, false, reason.c_str());
            emitEncodeControlEvent(ctx, event);
        }
        slot->hasMbqp = false;
        return slot;
    }

    fillEncodeControlEvent(&slot->attachedEvent, r, slot, decision, true, slot->reason);
    return slot;
}

void releasePipelineSurface(RouteContext::PipelineOp& op) {
    if (op.surface && op.surface->FrameInterface) {
        op.surface->FrameInterface->Release(op.surface);
    }
    op.surface = nullptr;
    op.decodeSync = nullptr;
}

void resetPipelineOp(RouteContext::PipelineOp& op) {
    releasePipelineSurface(op);
    releaseEncodeControlSlot(op.ctrlSlot);
    op.ctrlSlot = nullptr;
    op.encodeSync = nullptr;
    op.decodeOrder = 0;
    op.decodedOrder = 0;
    op.encodeOrder = 0;
    op.pts = 0;
    op.userOpaque = nullptr;
    op.preprocessApplied = false;
    op.state = RouteContext::PipelineOp::FREE;
    resetOutput(op);
}

bool hasActivePipelineOps(const RouteContext& r) {
    for (const auto& op : r.pipelineOps) {
        if (op.state != RouteContext::PipelineOp::FREE) return true;
    }
    return false;
}

RouteContext::PipelineOp* findFreePipelineOp(RouteContext& r) {
    for (auto& op : r.pipelineOps) {
        if (op.state == RouteContext::PipelineOp::FREE) return &op;
    }
    return nullptr;
}

void markDecodedReady(MFX50RT_Context* ctx, RouteContext& r, RouteContext::PipelineOp& op) {
    if (!op.surface) {
        op.state = RouteContext::PipelineOp::FREE;
        return;
    }
    r.decodedFrames++;
    op.decodedOrder = r.nextDecodedReadyOrder++;
    op.surface->Data.TimeStamp = op.pts >= 0
        ? static_cast<mfxU64>(op.pts)
        : static_cast<mfxU64>(MFX_TIMESTAMP_UNKNOWN);
    analyzeSceneIfEnabled(ctx, r, op.surface, op.pts);
    op.state = RouteContext::PipelineOp::DECODED;
}

bool trySyncOneDecode(MFX50RT_Context* ctx,
                      RouteContext& r,
                      bool blocking) {
    for (auto& op : r.pipelineOps) {
        if (op.state != RouteContext::PipelineOp::DECODING ||
            op.decodeOrder != r.nextDecodeOutputOrder) continue;
        if (!op.decodeSync) {
            markDecodedReady(ctx, r, op);
            r.nextDecodeOutputOrder++;
            return true;
        }
        const uint64_t syncStart = nowUs();
        const mfxStatus st = MFXVideoCORE_SyncOperation(
            r.vpl->session, op.decodeSync, blocking ? 1000U : 0U);
        if (st == MFX_ERR_NONE) {
            r.syncUs += elapsedUs(syncStart);
            op.decodeSync = nullptr;
            markDecodedReady(ctx, r, op);
            r.nextDecodeOutputOrder++;
            return true;
        }
        if (st == MFX_WRN_IN_EXECUTION) return false;
        resetPipelineOp(op);
        requireMfx(st, "MFXVideoCORE_SyncOperation(decode)");
    }
    return false;
}

bool trySubmitOneDecode(MFX50RT_Context* ctx,
                        RouteContext& r,
                        bool draining,
                        int64_t pts,
                        void* userOpaque,
                        bool* needMoreData) {
    (void)ctx;
    RouteContext::PipelineOp* op = findFreePipelineOp(r);
    if (!op) return false;

    op->surface = nullptr;
    op->decodeSync = nullptr;
    op->encodeSync = nullptr;
    op->ctrlSlot = nullptr;
    op->pts = pts;
    op->userOpaque = userOpaque;
    resetOutput(*op);

    mfxBitstream* bs = draining ? nullptr : &r.inputBs;
    const uint64_t decodeStart = nowUs();
    const mfxStatus st = MFXVideoDECODE_DecodeFrameAsync(
        r.vpl->session, bs, nullptr, &op->surface, &op->decodeSync);
    r.decodeUs += elapsedUs(decodeStart);

    if (st == MFX_WRN_DEVICE_BUSY) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        return false;
    }
    if (st == MFX_ERR_MORE_SURFACE || st == MFX_WRN_ALLOC_TIMEOUT_EXPIRED) {
        return false;
    }
    if (st == MFX_ERR_MORE_DATA) {
        if (!draining) compactInput(r);
        if (needMoreData) *needMoreData = true;
        return false;
    }
    requireMfx(st, "MFXVideoDECODE_DecodeFrameAsync");
    if (!op->surface) return false;

    op->decodeOrder = r.nextDecodeSubmitOrder++;
    op->state = RouteContext::PipelineOp::DECODING;
    return true;
}

bool trySubmitOneEncode(MFX50RT_Context* ctx, RouteContext& r) {
    RouteContext::PipelineOp* selected = nullptr;
    for (auto& candidate : r.pipelineOps) {
        if (candidate.state != RouteContext::PipelineOp::DECODED) continue;
        if (!selected || candidate.decodedOrder < selected->decodedOrder) selected = &candidate;
    }
    if (!selected) return false;

    auto& op = *selected;
    if (!op.ctrlSlot) {
        op.ctrlSlot = prepareEncodeControl(ctx, r, op.surface, op.pts);
        if (!op.preprocessApplied) {
            applyPreprocessIfEnabled(ctx, r, op.surface);
            op.preprocessApplied = true;
        }
        applyStaticReuseIfEnabled(ctx, r, op.surface, op.ctrlSlot);
    }
        mfxEncodeCtrl* ctrl = op.ctrlSlot && op.ctrlSlot->hasMbqp ? &op.ctrlSlot->ctrl : nullptr;
        resetOutput(op);
        op.encodeSync = nullptr;
        const uint64_t submitStart = nowUs();
        const mfxStatus st = MFXVideoENCODE_EncodeFrameAsync(
            r.vpl->session, ctrl, op.surface, &op.outputBs, &op.encodeSync);
        r.encodeSubmitUs += elapsedUs(submitStart);

        if (st == MFX_WRN_DEVICE_BUSY) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            return false;
        }
        if (st == MFX_ERR_MORE_DATA) {
            resetPipelineOp(op);
            return true;
        }
        if (st < MFX_ERR_NONE) {
            resetPipelineOp(op);
            requireMfx(st, "MFXVideoENCODE_EncodeFrameAsync");
        }

        releasePipelineSurface(op);
        op.encodeOrder = r.nextEncodeSubmitOrder++;
        op.state = RouteContext::PipelineOp::ENCODING;
        return true;
}

bool trySyncOneEncode(MFX50RT_Context* ctx,
                      RouteContext& r,
                      bool blocking) {
    for (auto& op : r.pipelineOps) {
        if (op.state != RouteContext::PipelineOp::ENCODING ||
            op.encodeOrder != r.nextEncodeOutputOrder) continue;
        if (!op.encodeSync) {
            r.encodedFrames++;
            if (op.ctrlSlot && op.ctrlSlot->hasMbqp) {
                r.mbqpFrames++;
                r.mbqpInitEnabledFrames++;
                emitEncodeControlEvent(ctx, op.ctrlSlot->attachedEvent);
            }
            pushEncodedFromBitstream(ctx, r, op.outputBs, op.pts, op.userOpaque);
            releaseEncodeControlSlot(op.ctrlSlot);
            op.ctrlSlot = nullptr;
            op.state = RouteContext::PipelineOp::FREE;
            r.nextEncodeOutputOrder++;
            return true;
        }
        const uint64_t syncStart = nowUs();
        const mfxStatus st = MFXVideoCORE_SyncOperation(
            r.vpl->session, op.encodeSync, blocking ? 1000U : 0U);
        if (st == MFX_ERR_NONE) {
            r.syncUs += elapsedUs(syncStart);
            op.encodeSync = nullptr;
            r.encodedFrames++;
            if (op.ctrlSlot && op.ctrlSlot->hasMbqp) {
                r.mbqpFrames++;
                r.mbqpInitEnabledFrames++;
                emitEncodeControlEvent(ctx, op.ctrlSlot->attachedEvent);
            }
            pushEncodedFromBitstream(ctx, r, op.outputBs, op.pts, op.userOpaque);
            releaseEncodeControlSlot(op.ctrlSlot);
            op.ctrlSlot = nullptr;
            op.state = RouteContext::PipelineOp::FREE;
            r.nextEncodeOutputOrder++;
            return true;
        }
        if (st == MFX_WRN_IN_EXECUTION) return false;
        resetPipelineOp(op);
        requireMfx(st, "MFXVideoCORE_SyncOperation(encode)");
    }
    return false;
}

bool processRoutePipeline(MFX50RT_Context* ctx,
                          RouteContext& r,
                          bool draining,
                          int64_t pts,
                          void* userOpaque,
                          bool* decoderNeedsMoreData) {
    bool anyProgress = false;
    for (int guard = 0; guard < 256; ++guard) {
        bool progressed = false;
        while (trySyncOneEncode(ctx, r, false)) progressed = true;
        while (trySyncOneDecode(ctx, r, false)) progressed = true;
        while (trySubmitOneEncode(ctx, r)) progressed = true;

        bool needMoreData = false;
        while (findFreePipelineOp(r)) {
            if (!trySubmitOneDecode(ctx, r, draining, pts, userOpaque, &needMoreData)) break;
            progressed = true;
        }
        if (needMoreData) {
            if (decoderNeedsMoreData) *decoderNeedsMoreData = true;
            anyProgress = anyProgress || progressed;
            break;
        }
        if (!progressed) break;
        anyProgress = true;
    }
    return anyProgress;
}

bool processRoutePipelineSlice(MFX50RT_Context* ctx,
                               RouteContext& r,
                               bool draining,
                               int64_t pts,
                               void* userOpaque,
                               bool* decoderNeedsMoreData) {
    bool progressed = false;
    if (trySyncOneEncode(ctx, r, false)) progressed = true;
    if (trySyncOneDecode(ctx, r, false)) progressed = true;
    if (trySubmitOneEncode(ctx, r)) progressed = true;

    bool needMoreData = false;
    if (findFreePipelineOp(r) &&
        trySubmitOneDecode(ctx, r, draining, pts, userOpaque, &needMoreData)) {
        progressed = true;
    }
    if (needMoreData && decoderNeedsMoreData) {
        *decoderNeedsMoreData = true;
    }
    return progressed;
}

bool processRoutePipelineBudget(MFX50RT_Context* ctx,
                                RouteContext& r,
                                bool draining,
                                int64_t pts,
                                void* userOpaque,
                                bool* decoderNeedsMoreData,
                                int budget) {
    bool anyProgress = false;
    for (int i = 0; i < budget; ++i) {
        bool needMoreData = false;
        const bool progressed =
            processRoutePipelineSlice(ctx, r, draining, pts, userOpaque, &needMoreData);
        if (needMoreData) {
            if (decoderNeedsMoreData) *decoderNeedsMoreData = true;
            break;
        }
        if (!progressed) break;
        anyProgress = true;
    }
    return anyProgress;
}

void waitForOnePipelineOp(MFX50RT_Context* ctx, RouteContext& r) {
    if (trySyncOneEncode(ctx, r, true)) return;
    if (trySyncOneDecode(ctx, r, true)) return;
    if (trySubmitOneEncode(ctx, r)) return;
}

void drainPipeline(MFX50RT_Context* ctx, RouteContext& r, int64_t pts, void* userOpaque) {
    bool inputDone = (r.inputBs.DataLength == 0);
    bool decoderDone = false;
    for (int guard = 0; guard < 100000; ++guard) {
        bool needMoreData = false;
        const bool progressed =
            processRoutePipeline(ctx, r, inputDone && !decoderDone, pts, userOpaque, &needMoreData);
        if (!inputDone) {
            if (needMoreData || r.inputBs.DataLength == 0) inputDone = true;
        } else if (needMoreData) {
            decoderDone = true;
        }
        if (decoderDone && !hasActivePipelineOps(r)) return;
        if (!progressed) {
            waitForOnePipelineOp(ctx, r);
        }
    }
    throw std::runtime_error("pipeline drain guard exhausted");
}

void drainEncoder(MFX50RT_Context* ctx, RouteContext& r) {
    if (!r.encoderReady || r.encoderDrained) return;
    for (;;) {
        resetOutput(r);
        mfxSyncPoint syncp = nullptr;
        mfxStatus st = MFXVideoENCODE_EncodeFrameAsync(r.vpl->session, nullptr, nullptr, &r.outputBs, &syncp);
        if (st == MFX_WRN_DEVICE_BUSY) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (st == MFX_ERR_MORE_DATA) {
            r.encoderDrained = true;
            return;
        }
        requireMfx(st, "MFXVideoENCODE_EncodeFrameAsync(drain)");
        if (syncp) {
            uint64_t syncStart = nowUs();
            requireMfx(MFXVideoCORE_SyncOperation(r.vpl->session, syncp, 60000), "MFXVideoCORE_SyncOperation(drain)");
            r.syncUs += elapsedUs(syncStart);
            r.encodedFrames++;
            pushEncodedFromBitstream(ctx, r, r.outputBs, 0, nullptr);
        } else if (r.outputBs.DataLength > 0) {
            r.encodedFrames++;
            pushEncodedFromBitstream(ctx, r, r.outputBs, 0, nullptr);
        }
    }
}

void closeRoute(RouteContext& r) {
    for (auto& op : r.pipelineOps) {
        resetPipelineOp(op);
    }
    if (r.encoderReady) {
        MFXVideoENCODE_Close(r.vpl->session);
        r.encoderReady = false;
    }
    if (r.decoderReady) {
        MFXVideoDECODE_Close(r.vpl->session);
        r.decoderReady = false;
    }
    r.vpl.reset();
    r.va.reset();
}

int maxInputQueuePackets(const MFX50RT_Config& cfg) {
    if (cfg.max_input_queue_packets > 0) return cfg.max_input_queue_packets;
    if (cfg.max_queue_packets > 0) return cfg.max_queue_packets;
    return 128;
}

RouteContext::InputPacket packetFromApi(const MFX50RT_Packet* packet) {
    RouteContext::InputPacket in;
    if (!packet) return in;
    if (packet->data && packet->size > 0) {
        in.data.assign(packet->data, packet->data + packet->size);
    }
    in.pts = packet->pts;
    in.dts = packet->dts;
    in.isKeyframe = packet->is_keyframe;
    in.endOfStream = packet->end_of_stream;
    in.userOpaque = packet->user_opaque;
    return in;
}

int processInputPacketLocked(MFX50RT_Context* ctx,
                             RouteContext& route,
                             const RouteContext::InputPacket& packet,
                             bool countInput) {
    try {
        if (!packet.data.empty()) {
            appendInput(route, packet.data.data(), packet.data.size());
            if (countInput) {
                route.inputPackets++;
                route.bytesIn += packet.data.size();
            }
        }
        int initRc = initDecoderEncoderIfNeeded(ctx, route);
        if (initRc == MFX50_ERR_NEED_MORE_INPUT) {
            return packet.endOfStream ? MFX50_ERR_DECODE : MFX50_ERR_NEED_MORE_INPUT;
        }

        bool needMoreData = false;
        processRoutePipeline(ctx, route, false, packet.pts, packet.userOpaque, &needMoreData);

        if (packet.endOfStream) {
            drainPipeline(ctx, route, packet.pts, packet.userOpaque);
            drainEncoder(ctx, route);
            route.decoderDrained = true;
        }
    }
    catch (const std::exception& ex) {
        route.lastError = ex.what();
        route.decodeErrors++;
        setError(ctx, MFX50_ERR_DECODE, ex.what());
        return MFX50_ERR_DECODE;
    }
    return MFX50_OK;
}

bool hasSchedulerPipelineWorkLocked(const RouteContext& route) {
    return route.decoderReady &&
           !route.decoderDrained &&
           (route.inputBs.DataLength > 0 || hasActivePipelineOps(route));
}

int processInputPacketSliceLocked(MFX50RT_Context* ctx,
                                  RouteContext& route,
                                  const RouteContext::InputPacket& packet,
                                  bool countInput) {
    try {
        if (!packet.data.empty()) {
            appendInput(route, packet.data.data(), packet.data.size());
            if (countInput) {
                route.inputPackets++;
                route.bytesIn += packet.data.size();
            }
        }
        int initRc = initDecoderEncoderIfNeeded(ctx, route);
        if (initRc == MFX50_ERR_NEED_MORE_INPUT) {
            return packet.endOfStream ? MFX50_ERR_DECODE : MFX50_ERR_NEED_MORE_INPUT;
        }

        bool needMoreData = false;
        processRoutePipelineBudget(ctx, route, false, packet.pts, packet.userOpaque, &needMoreData, 16);

        if (packet.endOfStream) {
            drainPipeline(ctx, route, packet.pts, packet.userOpaque);
            drainEncoder(ctx, route);
            route.decoderDrained = true;
        }
    }
    catch (const std::exception& ex) {
        route.lastError = ex.what();
        route.decodeErrors++;
        setError(ctx, MFX50_ERR_DECODE, ex.what());
        return MFX50_ERR_DECODE;
    }
    return MFX50_OK;
}

bool hasQueuedAsyncInput(MFX50RT_Context* ctx) {
    for (auto& route : ctx->routes) {
        std::lock_guard<std::mutex> lock(route->inputMu);
        if (!route->inputQueue.empty()) return true;
    }
    return false;
}

bool hasSchedulerWorkForShard(MFX50RT_Context* ctx, int workerIndex, int workerCount) {
    if (!ctx || workerCount <= 0) return false;
    for (size_t i = static_cast<size_t>(workerIndex);
         i < ctx->routes.size();
         i += static_cast<size_t>(workerCount)) {
        auto& route = ctx->routes[i];
        {
            std::lock_guard<std::mutex> lock(route->inputMu);
            if (!route->inputQueue.empty()) return true;
        }
        {
            std::lock_guard<std::mutex> stateLock(route->stateMu);
            if (hasSchedulerPipelineWorkLocked(*route)) return true;
        }
    }
    return false;
}

bool hasSchedulerWork(MFX50RT_Context* ctx) {
    if (!ctx) return false;
    return hasSchedulerWorkForShard(ctx, 0, 1);
}

bool processSchedulerRouteStep(MFX50RT_Context* ctx, RouteContext& route) {
    RouteContext::InputPacket packet;
    bool havePacket = false;
    {
        std::lock_guard<std::mutex> lock(route.inputMu);
        if (!route.inputQueue.empty()) {
            packet = std::move(route.inputQueue.front());
            route.inputQueue.pop_front();
            route.asyncProcessing = true;
            havePacket = true;
        }
    }

    bool shouldPollPipeline = false;
    if (!havePacket) {
        std::lock_guard<std::mutex> stateLock(route.stateMu);
        shouldPollPipeline = hasSchedulerPipelineWorkLocked(route);
    }
    if (!havePacket && !shouldPollPipeline) return false;
    if (!havePacket) {
        std::lock_guard<std::mutex> lock(route.inputMu);
        route.asyncProcessing = true;
    }

    bool progressed = false;
    {
        std::lock_guard<std::mutex> stateLock(route.stateMu);
        if (havePacket) {
            processInputPacketSliceLocked(ctx, route, packet, false);
            route.asyncProcessedPackets++;
            progressed = true;
        } else {
            bool needMoreData = false;
            progressed = processRoutePipelineBudget(ctx, route, false, 0, nullptr, &needMoreData, 16);
        }
    }

    {
        std::lock_guard<std::mutex> lock(route.inputMu);
        route.asyncProcessing = false;
    }
    route.idleCv.notify_all();
    return progressed;
}

void globalAsyncSchedulerLoop(MFX50RT_Context* ctx, int workerIndex, int workerCount) {
    for (;;) {
        bool didWork = false;
        for (size_t i = static_cast<size_t>(workerIndex);
             i < ctx->routes.size();
             i += static_cast<size_t>(workerCount)) {
            if (processSchedulerRouteStep(ctx, *ctx->routes[i])) didWork = true;
        }

        CallbackBatch callbacks = prepareCallbacks(ctx);
        invokeCallbacks(callbacks);

        if (didWork) continue;

        {
            std::lock_guard<std::mutex> lock(ctx->schedulerMu);
            if (ctx->schedulerStop &&
                !hasSchedulerWorkForShard(ctx, workerIndex, workerCount)) {
                return;
            }
        }

        if (hasSchedulerWorkForShard(ctx, workerIndex, workerCount)) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }

        std::unique_lock<std::mutex> lock(ctx->schedulerMu);
        ctx->schedulerCv.wait(lock, [&]() {
            return ctx->schedulerStop ||
                   hasSchedulerWorkForShard(ctx, workerIndex, workerCount);
        });
        if (ctx->schedulerStop &&
            !hasSchedulerWorkForShard(ctx, workerIndex, workerCount)) {
            return;
        }
    }
}

void startAsyncWorkers(MFX50RT_Context* ctx) {
    if (!ctx || !ctx->cfg.async_mode) return;
    ctx->schedulerStarted = true;
    const int routes = static_cast<int>(ctx->routes.size());
    int workerCount = ctx->cfg.max_queue_surfaces > 0 ? ctx->cfg.max_queue_surfaces : 0;
    if (workerCount <= 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        workerCount = static_cast<int>(hw > 0 ? std::min<unsigned>(hw, 8U) : 4U);
    }
    workerCount = std::max(1, std::min(workerCount, std::max(1, routes)));
    ctx->schedulerThreads.reserve(static_cast<size_t>(workerCount));
    for (int i = 0; i < workerCount; ++i) {
        ctx->schedulerThreads.emplace_back(globalAsyncSchedulerLoop, ctx, i, workerCount);
    }
}

void stopAsyncWorkers(MFX50RT_Context* ctx) {
    if (!ctx || !ctx->cfg.async_mode) return;
    {
        std::lock_guard<std::mutex> lock(ctx->schedulerMu);
        ctx->schedulerStop = true;
    }
    ctx->schedulerCv.notify_all();
    if (ctx->schedulerStarted) {
        for (auto& thread : ctx->schedulerThreads) {
            if (thread.joinable()) thread.join();
        }
    }
    ctx->schedulerThreads.clear();
    ctx->schedulerStarted = false;
}

void waitAsyncRouteIdle(RouteContext& route) {
    std::unique_lock<std::mutex> lock(route.inputMu);
    route.idleCv.wait(lock, [&]() {
        return route.inputQueue.empty() && !route.asyncProcessing;
    });
}

int enqueueAsyncPacket(MFX50RT_Context* ctx, RouteContext& route, const MFX50RT_Packet* packet) {
    RouteContext::InputPacket in = packetFromApi(packet);
    const int maxInput = maxInputQueuePackets(ctx->cfg);
    bool droppedOldest = false;

    {
        std::unique_lock<std::mutex> lock(route.inputMu);
        if (maxInput > 0 && static_cast<int>(route.inputQueue.size()) >= maxInput) {
            if (ctx->cfg.drop_policy == MFX50RT_DROP_OLDEST && !route.inputQueue.empty()) {
                route.inputQueue.pop_front();
                droppedOldest = true;
            }
            else {
                {
                    std::lock_guard<std::mutex> stateLock(route.stateMu);
                    route.backpressureEvents++;
                }
                setError(ctx, MFX50_ERR_BACKPRESSURE, "async input queue is full");
                return MFX50_ERR_BACKPRESSURE;
            }
        }
        route.inputQueue.push_back(std::move(in));
    }

    {
        std::lock_guard<std::mutex> stateLock(route.stateMu);
        if (droppedOldest) {
            route.droppedFrames++;
        }
        if (packet->data && packet->size > 0) {
            route.inputPackets++;
            route.bytesIn += packet->size;
        }
        route.asyncEnqueuedPackets++;
    }

    route.inputCv.notify_one();
    ctx->schedulerCv.notify_one();
    return MFX50_OK;
}

} // namespace

extern "C" const char* MFX50RT_GetVersion(void) {
    return MFX50RT_VERSION;
}

extern "C" int MFX50RT_GetAbiVersion(void) {
    return MFX50RT_API_VERSION;
}

extern "C" const char* MFX50RT_StatusString(int code) {
    return statusString(code);
}

extern "C" int MFX50RT_DefaultConfig(MFX50RT_Config* cfg) {
    if (!cfg) {
        setGlobalError("config is null");
        return MFX50_ERR_INVALID_ARG;
    }
    std::memset(cfg, 0, sizeof(*cfg));
    cfg->struct_size = sizeof(*cfg);
    cfg->input_mode = MFX50_INPUT_ENCODED_PACKET;
    cfg->output_mode = MFX50_OUTPUT_POLL;
    cfg->input_codec = MFX50_CODEC_H264;
    cfg->output_codec = MFX50_CODEC_HEVC;
    cfg->fps_num = 30;
    cfg->fps_den = 1;
    cfg->device_selector = "auto";
    cfg->profile = MFX50_PROFILE_QUALITY_90_NEAR;
    cfg->route_count = 1;
    cfg->async_depth = 4;
    cfg->max_queue_packets = 128;
    cfg->max_queue_surfaces = 16;
    cfg->algo_budget_us = 1000;
    cfg->target_usage = -1;
    cfg->gop = -1;
    cfg->gop_ref_dist = -1;
    cfg->num_ref_frame = -1;
    cfg->qpi = -1;
    cfg->qpp = -1;
    cfg->qpb = -1;
    cfg->bref_type = 0;
    cfg->abi_version = MFX50RT_API_VERSION;
    cfg->async_mode = 0;
    cfg->max_input_queue_packets = 128;
    cfg->max_output_queue_packets = 128;
    cfg->drop_policy = MFX50RT_DROP_NONE;
    cfg->enable_static_reuse = 0;
    return MFX50_OK;
}

extern "C" int MFX50RT_DefaultAlgoConfig(MFX50RT_AlgoConfig* cfg) {
    if (!cfg) {
        setGlobalError("algo config is null");
        return MFX50_ERR_INVALID_ARG;
    }
    mfx50rt::defaultAlgoConfig(cfg);
    return MFX50_OK;
}

extern "C" int MFX50RT_Create(const MFX50RT_Config* cfg, MFX50RT_Handle* out_handle) {
    if (!out_handle) {
        setGlobalError("out_handle is null");
        return MFX50_ERR_INVALID_ARG;
    }
    *out_handle = nullptr;

    MFX50RT_Config normalized;
    MFX50RT_DefaultConfig(&normalized);
    if (cfg) {
        const size_t copySize = cfg->struct_size > 0
            ? std::min<size_t>(cfg->struct_size, sizeof(normalized))
            : sizeof(normalized);
        std::memcpy(&normalized, cfg, copySize);
        if (normalized.input_mode == 0) normalized.input_mode = MFX50_INPUT_ENCODED_PACKET;
        if (normalized.output_mode == 0) normalized.output_mode = MFX50_OUTPUT_POLL;
        if (normalized.input_codec == 0) normalized.input_codec = MFX50_CODEC_H264;
        if (normalized.output_codec == 0) normalized.output_codec = MFX50_CODEC_HEVC;
        if (normalized.fps_num <= 0) normalized.fps_num = 30;
        if (normalized.fps_den <= 0) normalized.fps_den = 1;
        if (normalized.route_count <= 0) normalized.route_count = 1;
        if (normalized.async_depth <= 0) normalized.async_depth = 4;
        if (!normalized.device_selector) normalized.device_selector = "auto";
        normalized.async_mode = normalized.async_mode ? 1 : 0;
        if (normalized.max_input_queue_packets <= 0) normalized.max_input_queue_packets = 128;
        if (normalized.max_output_queue_packets <= 0) normalized.max_output_queue_packets = 128;
        if (normalized.drop_policy < MFX50RT_DROP_NONE ||
            normalized.drop_policy > MFX50RT_DROP_NON_KEY_UNTIL_IDR) {
            normalized.drop_policy = MFX50RT_DROP_NONE;
        }
        normalized.enable_static_reuse = normalized.enable_static_reuse ? 1 : 0;
    }

    if (normalized.input_mode != MFX50_INPUT_ENCODED_PACKET) {
        setGlobalError("only encoded-packet input is implemented in realtime v0.4");
        return MFX50_ERR_NOT_IMPLEMENTED;
    }
    if (normalized.output_codec != MFX50_CODEC_HEVC) {
        setGlobalError("only HEVC output is implemented");
        return MFX50_ERR_INVALID_ARG;
    }
    if (normalized.route_count <= 0 || normalized.route_count > kMaxRoutes) {
        setGlobalError("route_count is out of range");
        return MFX50_ERR_INVALID_ARG;
    }

    std::unique_ptr<MFX50RT_Context> ctx(new MFX50RT_Context());
    ctx->cfg = normalized;
    mfx50rt::defaultAlgoConfig(&ctx->algoCfg);
    ctx->startUs = nowUs();

    try {
        ctx->routes.reserve(static_cast<size_t>(normalized.route_count));
        for (int i = 0; i < normalized.route_count; ++i) {
            std::unique_ptr<RouteContext> route(new RouteContext());
            route->id = i;
            initializeRoute(ctx.get(), *route);
            ctx->routes.push_back(std::move(route));
        }
    }
    catch (const std::exception& ex) {
        setError(ctx.get(), MFX50_ERR_DEVICE, ex.what());
        return MFX50_ERR_DEVICE;
    }

    startAsyncWorkers(ctx.get());

    *out_handle = ctx.release();
    return MFX50_OK;
}

extern "C" int MFX50RT_SetAlgoConfig(MFX50RT_Handle h, const MFX50RT_AlgoConfig* cfg) {
    if (!h || !cfg) {
        setGlobalError("handle or algo config is null");
        return MFX50_ERR_INVALID_ARG;
    }
    MFX50RT_AlgoConfig normalized = mfx50rt::normalizedAlgoConfig(cfg);
    std::lock_guard<std::mutex> lock(h->mu);
    h->algoCfg = normalized;
    return MFX50_OK;
}

extern "C" int MFX50RT_GetAlgoConfig(MFX50RT_Handle h, MFX50RT_AlgoConfig* cfg) {
    if (!h || !cfg) {
        setGlobalError("handle or algo config is null");
        return MFX50_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(h->mu);
    const size_t copySize = cfg->struct_size > 0
        ? std::min<size_t>(cfg->struct_size, sizeof(h->algoCfg))
        : sizeof(h->algoCfg);
    std::memcpy(cfg, &h->algoCfg, copySize);
    return MFX50_OK;
}

extern "C" int MFX50RT_GetAlgoCaps(MFX50RT_Handle h, MFX50RT_AlgoCaps* caps) {
    (void)h;
    if (!caps) {
        setGlobalError("algo caps is null");
        return MFX50_ERR_INVALID_ARG;
    }
    MFX50RT_AlgoCaps built = mfx50rt::buildAlgoCaps();
    const size_t copySize = caps->struct_size > 0
        ? std::min<size_t>(caps->struct_size, sizeof(built))
        : sizeof(built);
    std::memcpy(caps, &built, copySize);
    return MFX50_OK;
}

extern "C" int MFX50RT_SetOutputCallback(MFX50RT_Handle h,
                                          MFX50RT_OutputCallback cb,
                                          void* user_opaque) {
    if (!h) {
        setGlobalError("handle is null");
        return MFX50_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(h->mu);
    h->callback = cb;
    h->callbackOpaque = user_opaque;
    return MFX50_OK;
}

extern "C" int MFX50RT_SetLogCallback(MFX50RT_Handle h,
                                       MFX50RT_LogCallback cb,
                                       void* user_opaque) {
    if (!h) {
        setGlobalError("handle is null");
        return MFX50_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(h->mu);
    h->logCallback = cb;
    h->logOpaque = user_opaque;
    return MFX50_OK;
}

extern "C" int MFX50RT_PushPacket(MFX50RT_Handle h, const MFX50RT_Packet* packet) {
    if (!h || !packet) {
        setGlobalError("handle or packet is null");
        return MFX50_ERR_INVALID_ARG;
    }
    if (packet->stream_id < 0 || packet->stream_id >= static_cast<int>(h->routes.size())) {
        setError(h, MFX50_ERR_INVALID_ARG, "packet stream_id is out of range");
        return MFX50_ERR_INVALID_ARG;
    }

    RouteContext& route = *h->routes[static_cast<size_t>(packet->stream_id)];
    if (h->cfg.async_mode) {
        return enqueueAsyncPacket(h, route, packet);
    }

    int rc = MFX50_OK;
    {
        std::lock_guard<std::mutex> lock(route.stateMu);
        RouteContext::InputPacket in = packetFromApi(packet);
        rc = processInputPacketLocked(h, route, in, true);
    }
    CallbackBatch callbacks = prepareCallbacks(h);
    invokeCallbacks(callbacks);
    return rc;
}

extern "C" int MFX50RT_PushFrame(MFX50RT_Handle h, const MFX50RT_RawFrame* frame) {
    (void)h;
    (void)frame;
    setGlobalError("raw-frame input is not implemented in realtime v0.4");
    return MFX50_ERR_NOT_IMPLEMENTED;
}

extern "C" int MFX50RT_PollPacket(MFX50RT_Handle h, MFX50RT_EncodedPacket* out_packet) {
    if (!h || !out_packet) {
        setGlobalError("handle or out_packet is null");
        return MFX50_ERR_INVALID_ARG;
    }
    return h->outputQueue.poll(out_packet);
}

extern "C" int MFX50RT_Flush(MFX50RT_Handle h) {
    if (!h) {
        setGlobalError("handle is null");
        return MFX50_ERR_INVALID_ARG;
    }
    int rc = MFX50_OK;
    try {
        for (auto& route : h->routes) {
            if (h->cfg.async_mode) {
                waitAsyncRouteIdle(*route);
            }
            std::lock_guard<std::mutex> stateLock(route->stateMu);
            if (!route->decoderReady || route->decoderDrained) continue;
            drainPipeline(h, *route, 0, nullptr);
            drainEncoder(h, *route);
            route->decoderDrained = true;
        }
    }
    catch (const std::exception& ex) {
        for (auto& route : h->routes) {
            std::lock_guard<std::mutex> stateLock(route->stateMu);
            route->encodeErrors++;
        }
        setError(h, MFX50_ERR_ENCODE, ex.what());
        rc = MFX50_ERR_ENCODE;
    }
    CallbackBatch callbacks = prepareCallbacks(h);
    invokeCallbacks(callbacks);
    return rc;
}

extern "C" int MFX50RT_GetStats(MFX50RT_Handle h, MFX50RT_Stats* stats) {
    if (!h || !stats) {
        setGlobalError("handle or stats is null");
        return MFX50_ERR_INVALID_ARG;
    }
    MFX50RT_Stats s = {};
    s.struct_size = sizeof(s);
    uint64_t decodeUsTotal = 0;
    uint64_t encodeSubmitUsTotal = 0;
    uint64_t syncUsTotal = 0;
    uint64_t preprocessUsTotal = 0;
    uint64_t sceneAnalyzeUsTotal = 0;
    for (const auto& route : h->routes) {
        {
            std::lock_guard<std::mutex> inputLock(route->inputMu);
            s.current_input_queue_packets += static_cast<int>(route->inputQueue.size());
        }
        {
            std::lock_guard<std::mutex> stateLock(route->stateMu);
            s.input_packets += route->inputPackets;
            s.decoded_frames += route->decodedFrames;
            s.encoded_frames += route->encodedFrames;
            s.output_packets += route->outputPackets;
            s.dropped_frames += route->droppedFrames;
            s.fallback_frames += route->fallbackFrames;
            s.input_bytes += route->bytesIn;
            s.output_bytes += route->bytesOut;
            s.decode_errors += route->decodeErrors;
            s.encode_errors += route->encodeErrors;
            s.preprocess_frames += route->preprocessFrames;
            s.smooth_scale_frames += route->smoothScaleFrames;
            s.pre_denoise_frames += route->preDenoiseFrames;
            s.scene_analyzed_frames += route->sceneAnalyzedFrames;
            s.adaptive_profile_switches += route->adaptiveProfileSwitches;
            s.adaptive_qp_frames += route->adaptiveQpFrames;
            s.mbqp_frames += route->mbqpFrames;
            s.mbqp_fallback_frames += route->mbqpFallbackFrames;
            s.mbqp_skipped_frames += route->mbqpSkippedFrames;
            s.mbqp_init_enabled_frames += route->mbqpInitEnabledFrames;
            s.async_enqueued_packets += route->asyncEnqueuedPackets;
            s.async_processed_packets += route->asyncProcessedPackets;
            s.backpressure_events += route->backpressureEvents;
            decodeUsTotal += route->decodeUs;
            encodeSubmitUsTotal += route->encodeSubmitUs;
            syncUsTotal += route->syncUs;
            preprocessUsTotal += route->preprocessUs;
            sceneAnalyzeUsTotal += route->sceneAnalyzeUs;
        }
    }
    const double elapsedSec = std::max(0.001, static_cast<double>(elapsedUs(h->startUs)) / 1000000.0);
    s.fps_in = static_cast<double>(s.input_packets) / elapsedSec;
    s.fps_out = static_cast<double>(s.encoded_frames) / elapsedSec;
    if (s.decoded_frames > 0) {
        s.avg_decode_us = static_cast<double>(decodeUsTotal) / static_cast<double>(s.decoded_frames);
    }
    if (s.encoded_frames > 0) {
        s.avg_encode_submit_us = static_cast<double>(encodeSubmitUsTotal) / static_cast<double>(s.encoded_frames);
        s.avg_sync_us = static_cast<double>(syncUsTotal) / static_cast<double>(s.encoded_frames + s.decoded_frames);
    }
    if (s.preprocess_frames > 0) {
        s.avg_algo_us = static_cast<double>(preprocessUsTotal) / static_cast<double>(s.preprocess_frames);
        s.avg_preprocess_ms = s.avg_algo_us / 1000.0;
    }
    if (s.scene_analyzed_frames > 0) {
        s.avg_scene_analyze_ms =
            static_cast<double>(sceneAnalyzeUsTotal) / static_cast<double>(s.scene_analyzed_frames) / 1000.0;
    }
    s.current_queue_packets = static_cast<int>(h->outputQueue.size());
    s.current_queue_surfaces = 0;
    s.route_count = static_cast<int>(h->routes.size());
    s.abi_version = MFX50RT_API_VERSION;
    {
        std::lock_guard<std::mutex> lock(h->mu);
        s.last_error_code = h->lastErrorCode;
        copyString(s.last_error_msg, sizeof(s.last_error_msg), h->lastError);
        MFX50RT_AlgoCaps caps = mfx50rt::buildAlgoCaps();
        s.mbqp_supported = caps.supports_mbqp;
        s.mbqp_disabled_reason = h->algoCfg.enable_mbqp && !caps.supports_mbqp
            ? MFX50RT_MBQP_DISABLED_UNSUPPORTED
            : MFX50RT_MBQP_DISABLED_NONE;
        s.active_profile = static_cast<int>(h->cfg.profile);
        s.active_algo_flags = mfx50rt::activeAlgoFlags(h->algoCfg);
        s.async_mode = h->cfg.async_mode;
    }

    const size_t copySize = stats->struct_size > 0
        ? std::min<size_t>(stats->struct_size, sizeof(s))
        : sizeof(s);
    std::memcpy(stats, &s, copySize);
    return MFX50_OK;
}

extern "C" const char* MFX50RT_GetLastError(MFX50RT_Handle h) {
    if (h && !h->lastError.empty()) return h->lastError.c_str();
    return g_last_error.c_str();
}

extern "C" int mfx50_realtime_set_frame_decision_callback(
    void* old_realtime_handle,
    MFX50RT_InternalFrameDecisionCallback cb,
    void* opaque) {
    auto* h = reinterpret_cast<MFX50RT_Handle>(old_realtime_handle);
    if (!h) {
        setGlobalError("handle is null");
        return MFX50_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(h->mu);
    h->frameDecisionCallback = cb;
    h->frameDecisionOpaque = opaque;
    return MFX50_OK;
}

extern "C" int mfx50_realtime_set_encode_control_event_callback(
    void* old_realtime_handle,
    MFX50RT_InternalEncodeControlEventCallback cb,
    void* opaque) {
    auto* h = reinterpret_cast<MFX50RT_Handle>(old_realtime_handle);
    if (!h) {
        setGlobalError("handle is null");
        return MFX50_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(h->mu);
    h->encodeControlEventCallback = cb;
    h->encodeControlEventOpaque = opaque;
    return MFX50_OK;
}

extern "C" int MFX50RT_Close(MFX50RT_Handle h) {
    if (!h) return MFX50_OK;
    MFX50RT_Flush(h);
    stopAsyncWorkers(h);
    for (auto& route : h->routes) {
        std::lock_guard<std::mutex> stateLock(route->stateMu);
        closeRoute(*route);
    }
    delete h;
    return MFX50_OK;
}
