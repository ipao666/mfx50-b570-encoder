#include "mfx50_realtime.h"

#include <cstdio>
#include <cstring>

namespace {

bool requireTrue(bool value, const char* label) {
    if (value) return true;
    std::fprintf(stderr, "api probe failed: %s\n", label);
    return false;
}

bool requireStatusName(int code, const char* expected) {
    const char* actual = MFX50RT_StatusString(code);
    if (actual && std::strcmp(actual, expected) == 0) return true;
    std::fprintf(stderr,
                 "api probe failed: status %d expected %s got %s\n",
                 code,
                 expected,
                 actual ? actual : "(null)");
    return false;
}

} // namespace

int main() {
    bool ok = true;

    const char* version = MFX50RT_GetVersion();
    ok = requireTrue(version && version[0] != '\0', "version is empty") && ok;
    ok = requireTrue(MFX50RT_GetAbiVersion() == MFX50RT_API_VERSION, "ABI version mismatch") && ok;

    ok = requireStatusName(MFX50_OK, "MFX50_OK") && ok;
    ok = requireStatusName(MFX50_ERR_INVALID_ARG, "MFX50_ERR_INVALID_ARG") && ok;
    ok = requireStatusName(MFX50_ERR_BACKPRESSURE, "MFX50_ERR_BACKPRESSURE") && ok;
    ok = requireStatusName(MFX50_ERR_BUFFER_TOO_SMALL, "MFX50_ERR_BUFFER_TOO_SMALL") && ok;
    ok = requireStatusName(MFX50_ERR_NO_OUTPUT, "MFX50_ERR_NO_OUTPUT") && ok;
    ok = requireStatusName(MFX50_ERR_AGAIN, "MFX50_ERR_AGAIN") && ok;

    const char* unknown = MFX50RT_StatusString(-12345);
    ok = requireTrue(unknown && std::strstr(unknown, "UNKNOWN") != nullptr, "unknown status string") && ok;

    MFX50RT_Config cfg = {};
    int rc = MFX50RT_DefaultConfig(&cfg);
    ok = requireTrue(rc == MFX50_OK, "DefaultConfig rc") && ok;
    ok = requireTrue(cfg.struct_size == sizeof(cfg), "DefaultConfig struct_size") && ok;
    ok = requireTrue(cfg.input_mode == MFX50_INPUT_ENCODED_PACKET, "DefaultConfig input_mode") && ok;
    ok = requireTrue(cfg.output_codec == MFX50_CODEC_HEVC, "DefaultConfig output_codec") && ok;
    ok = requireTrue(cfg.abi_version == MFX50RT_API_VERSION, "DefaultConfig abi_version") && ok;
    ok = requireTrue(cfg.async_mode == 0, "DefaultConfig async_mode") && ok;
    ok = requireTrue(cfg.max_input_queue_packets > 0, "DefaultConfig max_input_queue_packets") && ok;
    ok = requireTrue(cfg.drop_policy == MFX50RT_DROP_NONE, "DefaultConfig drop_policy") && ok;
    ok = requireTrue(MFX50_PROFILE_COMPRESS_90_PROBE_A == 4, "compress90 profile A id") && ok;
    ok = requireTrue(MFX50_PROFILE_COMPRESS_90_PROBE_D == 7, "compress90 profile D id") && ok;

    MFX50RT_AlgoConfig algo = {};
    rc = MFX50RT_DefaultAlgoConfig(&algo);
    ok = requireTrue(rc == MFX50_OK, "DefaultAlgoConfig rc") && ok;
    ok = requireTrue(algo.struct_size == sizeof(algo), "DefaultAlgoConfig struct_size") && ok;
    ok = requireTrue(algo.abi_version == MFX50RT_API_VERSION, "DefaultAlgoConfig abi_version") && ok;
    ok = requireTrue(algo.enable_preprocess == 0, "DefaultAlgoConfig preprocess disabled") && ok;
    ok = requireTrue(algo.enable_smooth_scale == 0, "DefaultAlgoConfig smooth disabled") && ok;
    ok = requireTrue(algo.enable_pre_denoise == 0, "DefaultAlgoConfig denoise disabled") && ok;
    ok = requireTrue(algo.enable_scene_analyzer == 0, "DefaultAlgoConfig scene disabled") && ok;
    ok = requireTrue(algo.enable_adaptive_qp == 0, "DefaultAlgoConfig adaptive qp disabled") && ok;
    ok = requireTrue(algo.enable_mbqp == 0, "DefaultAlgoConfig mbqp disabled") && ok;
    ok = requireTrue(algo.target_output_ratio_permille == 100, "DefaultAlgoConfig target ratio") && ok;

    MFX50RT_AlgoCaps caps = {};
    caps.struct_size = sizeof(caps);
    rc = MFX50RT_GetAlgoCaps(nullptr, &caps);
    ok = requireTrue(rc == MFX50_OK, "GetAlgoCaps rc") && ok;
    ok = requireTrue(caps.supports_preprocess == 1, "GetAlgoCaps supports_preprocess") && ok;

    rc = MFX50RT_SetLogCallback(nullptr, nullptr, nullptr);
    ok = requireTrue(rc == MFX50_ERR_INVALID_ARG, "SetLogCallback null handle") && ok;
    ok = requireTrue(std::strcmp(MFX50RT_StatusString(rc), "MFX50_ERR_INVALID_ARG") == 0,
                     "SetLogCallback status string") && ok;

    MFX50RT_Stats stats = {};
    stats.struct_size = sizeof(stats);
    rc = MFX50RT_GetStats(nullptr, &stats);
    ok = requireTrue(rc == MFX50_ERR_INVALID_ARG, "GetStats null handle") && ok;

    rc = MFX50RT_SetAlgoConfig(nullptr, &algo);
    ok = requireTrue(rc == MFX50_ERR_INVALID_ARG, "SetAlgoConfig null handle") && ok;
    rc = MFX50RT_GetAlgoConfig(nullptr, &algo);
    ok = requireTrue(rc == MFX50_ERR_INVALID_ARG, "GetAlgoConfig null handle") && ok;

    std::printf("version=%s\n", version);
    std::printf("abi_version=%d\n", MFX50RT_GetAbiVersion());
    std::printf("status_ok=%s\n", MFX50RT_StatusString(MFX50_OK));
    std::printf("status_invalid_arg=%s\n", MFX50RT_StatusString(MFX50_ERR_INVALID_ARG));
    std::printf("status_backpressure=%s\n", MFX50RT_StatusString(MFX50_ERR_BACKPRESSURE));
    std::printf("status_buffer_too_small=%s\n", MFX50RT_StatusString(MFX50_ERR_BUFFER_TOO_SMALL));
    std::printf("algo_caps_preprocess=%d\n", caps.supports_preprocess);
    std::printf("algo_caps_scene_analyzer=%d\n", caps.supports_scene_analyzer);
    std::printf("algo_caps_mbqp=%d\n", caps.supports_mbqp);

    return ok ? 0 : 1;
}
