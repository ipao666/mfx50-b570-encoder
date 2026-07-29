#include "mfx50rt.h"

#include <array>
#include <cassert>

int main() {
    MFX50RT_Config cfg{};
    assert(MFX50RT_DefaultConfig(&cfg) == MFX50RT_OK);
    cfg.backend.type = MFX50RT_BACKEND_NULL;
    cfg.runtime.async_mode = 0;
    cfg.runtime.route_count = 2;
    cfg.algo.strategy = MFX50RT_STRATEGY_MBQP_CQP;

    MFX50RT_Handle h = nullptr;
    assert(MFX50RT_Create(&cfg, &h) == MFX50RT_OK);

    MFX50RT_EffectiveConfig eff{};
    eff.size = sizeof(eff);
    eff.version = MFX50RT_API_VERSION;
    assert(MFX50RT_GetEffectiveConfig(h, &eff) == MFX50RT_OK);
    assert(eff.effective_strategy == MFX50RT_STRATEGY_GLOBAL);
    assert(eff.fallback_reason[0] != '\0');

    std::array<unsigned char, 1000> input{};
    MFX50RT_InputPacket pkt{};
    pkt.size = sizeof(pkt);
    pkt.version = MFX50RT_API_VERSION;
    pkt.stream_id = 1;
    pkt.data = input.data();
    pkt.data_size = static_cast<uint32_t>(input.size());
    pkt.pts = 100;
    assert(MFX50RT_PushPacket(h, &pkt) == MFX50RT_OK);

    MFX50RT_OutputPacket out{};
    assert(MFX50RT_PollPacket(h, &out, 100) == MFX50RT_OK);
    assert(out.stream_id == 1);
    assert(out.data_size > 0);
    assert(out.qp_avg >= 1 && out.qp_avg <= 51);
    assert(MFX50RT_ReleasePacket(h, &out) == MFX50RT_OK);

    uint32_t count = 0;
    assert(MFX50RT_GetDecisionTrace(h, 1, nullptr, &count) == MFX50RT_OK);
    assert(count == 1);
    MFX50RT_DecisionTrace trace{};
    trace.size = sizeof(trace);
    trace.version = MFX50RT_API_VERSION;
    count = 1;
    assert(MFX50RT_GetDecisionTrace(h, 1, &trace, &count) == MFX50RT_OK);
    assert(trace.frame_anchor_qp >= 1 && trace.frame_anchor_qp <= 51);
    assert(trace.effective_strategy == MFX50RT_STRATEGY_GLOBAL);

    MFX50RT_QualityMetric metric{};
    metric.size = sizeof(metric);
    metric.version = MFX50RT_API_VERSION;
    metric.stream_id = 1;
    metric.ssim = 0.70f;
    metric.compression_ratio = 0.90f;
    assert(MFX50RT_UpdateQualityMetric(h, &metric) == MFX50RT_OK);

    MFX50RT_RouteStats stats{};
    stats.size = sizeof(stats);
    stats.version = MFX50RT_API_VERSION;
    assert(MFX50RT_GetRouteStats(h, 1, &stats) == MFX50RT_OK);
    assert(stats.input_packets == 1);
    assert(stats.output_packets == 1);
    assert(stats.min_ssim <= 0.70);

    assert(MFX50RT_Close(h) == MFX50RT_OK);
    return 0;
}
