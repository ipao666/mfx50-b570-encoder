#include "decision_trace.h"

#include "tuning_profile.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace mfx50rt::hybridtsrq {

namespace {

void copy_cstr(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) return;
    std::snprintf(dst, dst_size, "%s", src ? src : "");
}

std::string detail_json_for(const HybridTSRQDecision& decision,
                            const FastFrameFeatures& fast,
                            const RoiAnalysisResult& roi,
                            const QualityState& quality,
                            bool mbqp_attached) {
    std::ostringstream os;
    os << "{\"has_mbqp\":" << (decision.spatial.has_mbqp ? "true" : "false")
       << ",\"has_roi\":" << (decision.spatial.has_roi ? "true" : "false")
       << ",\"mbqp_attached\":" << (mbqp_attached ? "true" : "false")
       << ",\"proxy_quality\":" << (quality.proxy_mode ? "true" : "false")
       << ",\"foreground_ratio\":" << roi.foreground_ratio
       << ",\"mean_luma\":" << fast.mean_luma
       << ",\"qp_p10\":" << decision.spatial.qp_stats.qp_p10
       << ",\"qp_p50\":" << decision.spatial.qp_stats.qp_p50
       << ",\"qp_p90\":" << decision.spatial.qp_stats.qp_p90
       << ",\"qp_p95\":" << decision.spatial.qp_stats.qp_p95
       << ",\"smoothing_changed_qp_avg\":" << decision.spatial.smoothing_changed_qp_avg
       << ",\"high_qp_block_ratio\":" << decision.spatial.qp_stats.high_qp_block_ratio
       << ",\"low_importance_block_ratio\":"
       << decision.spatial.region_stats.low_importance_block_ratio
       << ",\"roi_block_ratio\":" << decision.spatial.region_stats.roi_block_ratio
       << ",\"background_block_ratio\":"
       << decision.spatial.region_stats.background_block_ratio
       << ",\"flat_background_block_ratio\":"
       << decision.spatial.region_stats.flat_background_block_ratio
       << ",\"true_roi_block_ratio\":"
       << decision.spatial.region_stats.true_roi_block_ratio
       << ",\"edge_texture_roi_block_ratio\":"
       << decision.spatial.region_stats.edge_texture_roi_block_ratio
       << ",\"high_texture_background_block_ratio\":"
       << decision.spatial.region_stats.high_texture_background_block_ratio
       << ",\"hard_scene_background_block_ratio\":"
       << decision.spatial.region_stats.hard_scene_background_block_ratio
       << ",\"normal_background_block_ratio\":"
       << decision.spatial.region_stats.normal_background_block_ratio
       << ",\"scene_mode\":\"" << scene_mode_name(decision.spatial.scene_mode) << "\""
        << ",\"class_qp_table_name\":\"" << decision.spatial.class_qp_table_name << "\""
        << ",\"static_reuse_candidate\":"
        << (decision.static_reuse_candidate ? "true" : "false")
        << ",\"static_reuse_consecutive_frames\":"
        << decision.static_reuse_consecutive_frames
        << ",\"static_reuse_risk_score\":" << decision.static_reuse_risk_score
        << ",\"hard_scene_like\":"
        << (decision.spatial.hard_scene_like ? "true" : "false")
       << ",\"hard_guard_active\":"
       << (decision.spatial.hard_guard_active ? "true" : "false")
       << ",\"class_qp_table_overwrite_count\":"
       << decision.spatial.class_qp_table_overwrite_count
       << ",\"qp_histogram_1_51\":[";
    for (size_t qp = 1; qp <= 51; ++qp) {
        if (qp > 1) os << ',';
        os << decision.spatial.qp_stats.histogram[qp];
    }
    os << "]}";
    return os.str();
}

} // namespace

DecisionTraceBuffer::DecisionTraceBuffer(size_t capacity)
    : capacity_(capacity > 0 ? capacity : 256) {}

void DecisionTraceBuffer::push(uint32_t stream_id,
                               int64_t pts,
                               int frame_type,
                               const HybridTSRQDecision& decision,
                               const FastFrameFeatures& fast,
                               const RoiAnalysisResult& roi,
                               const QualityState& quality) {
    MFX50RT_DecisionTrace trace{};
    trace.size = sizeof(trace);
    trace.version = MFX50RT_API_VERSION;
    trace.stream_id = stream_id;
    trace.frame_id = static_cast<int64_t>(decision.frame_id);
    trace.pts = pts;
    trace.frame_type = frame_type;
    trace.base_scene_qp = decision.temporal.base_scene_qp;
    trace.qpi = decision.temporal.qpi;
    trace.qpp = decision.temporal.qpp;
    trace.qpb = decision.temporal.qpb;
    trace.frame_anchor_qp = decision.temporal.frame_anchor_qp;
    trace.spatial_min_qp = decision.spatial.spatial_min_qp;
    trace.spatial_max_qp = decision.spatial.spatial_max_qp;
    trace.spatial_avg_qp = decision.spatial.spatial_avg_qp;
    trace.qp_p10 = decision.spatial.qp_stats.qp_p10;
    trace.qp_p50 = decision.spatial.qp_stats.qp_p50;
    trace.qp_p90 = decision.spatial.qp_stats.qp_p90;
    trace.qp_p95 = decision.spatial.qp_stats.qp_p95;
    trace.roi_count = static_cast<int32_t>(decision.spatial.roi_boxes.size());
    trace.foreground_ratio = roi.foreground_ratio;
    trace.edge_density = fast.edge_density;
    trace.motion_score = fast.motion_score;
    trace.noise_score = fast.noise_score;
    trace.scene_cut_score = fast.scene_cut_score;
    trace.low_importance_block_ratio = decision.spatial.region_stats.low_importance_block_ratio;
    trace.high_qp_block_ratio = decision.spatial.qp_stats.high_qp_block_ratio;
    trace.roi_block_ratio = decision.spatial.region_stats.roi_block_ratio;
    trace.background_block_ratio = decision.spatial.region_stats.background_block_ratio;
    trace.flat_background_block_ratio = decision.spatial.region_stats.flat_background_block_ratio;
    trace.foreground_block_ratio = decision.spatial.region_stats.foreground_block_ratio;
    trace.edge_block_ratio = decision.spatial.region_stats.edge_block_ratio;
    trace.texture_block_ratio = decision.spatial.region_stats.texture_block_ratio;
    trace.transition_block_ratio = decision.spatial.region_stats.transition_block_ratio;
    trace.true_roi_block_ratio = decision.spatial.region_stats.true_roi_block_ratio;
    trace.edge_texture_roi_block_ratio =
        decision.spatial.region_stats.edge_texture_roi_block_ratio;
    trace.high_texture_background_block_ratio =
        decision.spatial.region_stats.high_texture_background_block_ratio;
    trace.hard_scene_background_block_ratio =
        decision.spatial.region_stats.hard_scene_background_block_ratio;
    trace.normal_background_block_ratio =
        decision.spatial.region_stats.normal_background_block_ratio;
    trace.hard_scene_like = decision.spatial.hard_scene_like ? 1 : 0;
    trace.hard_guard_active = decision.spatial.hard_guard_active ? 1 : 0;
    trace.class_qp_table_overwrite_count =
        decision.spatial.class_qp_table_overwrite_count;
    copy_cstr(trace.scene_mode,
              sizeof(trace.scene_mode),
              scene_mode_name(decision.spatial.scene_mode));
    copy_cstr(trace.class_qp_table_name,
              sizeof(trace.class_qp_table_name),
              decision.spatial.class_qp_table_name.c_str());
    trace.static_reuse_candidate = decision.static_reuse_candidate ? 1 : 0;
    trace.static_reuse_consecutive_frames =
        decision.static_reuse_consecutive_frames;
    trace.static_reuse_risk_score = decision.static_reuse_risk_score;
    trace.smoothing_changed_qp_avg = decision.spatial.smoothing_changed_qp_avg;
    trace.quality_guard_state = quality.active ? 1 : 0;
    trace.effective_strategy = decision.strategy;
    trace.mbqp_requested = decision.spatial.has_mbqp ? 1 : 0;
    trace.mbqp_attached = 0;
    trace.mbqp_block_cols = decision.spatial.mbqp_map.block_cols;
    trace.mbqp_block_rows = decision.spatial.mbqp_map.block_rows;
    trace.mbqp_pitch = decision.spatial.mbqp_map.block_cols;
    trace.mbqp_num_qp_alloc = static_cast<uint32_t>(decision.spatial.mbqp_map.qp.size());
    copy_cstr(trace.actual_encode_control,
              sizeof(trace.actual_encode_control),
              "PENDING");
    copy_cstr(trace.encode_ctrl_reason,
              sizeof(trace.encode_ctrl_reason),
              "EncodeFrameAsync control result not reported yet");
    copy_cstr(trace.reason, sizeof(trace.reason), decision.reason);
    const std::string detail = detail_json_for(decision, fast, roi, quality, false);
    copy_cstr(trace.detail_json, sizeof(trace.detail_json), detail.c_str());

    std::lock_guard<std::mutex> lock(mutex_);
    if (traces_.size() >= capacity_) traces_.pop_front();
    traces_.push_back(trace);
}

MFX50RT_Status DecisionTraceBuffer::copyTo(MFX50RT_DecisionTrace* traces,
                                           uint32_t* inout_count) const {
    if (!inout_count) return MFX50RT_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t required = static_cast<uint32_t>(traces_.size());
    if (!traces || *inout_count < required) {
        *inout_count = required;
        return traces ? MFX50RT_ERR_BUFFER_TOO_SMALL : MFX50RT_OK;
    }
    std::copy(traces_.begin(), traces_.end(), traces);
    *inout_count = required;
    return MFX50RT_OK;
}

void DecisionTraceBuffer::updateEncodeControl(uint32_t stream_id,
                                              int64_t frame_id,
                                              int mbqp_attached,
                                              const char* actual_encode_control,
                                              const char* encode_ctrl_reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = traces_.rbegin(); it != traces_.rend(); ++it) {
        if (it->stream_id != stream_id || it->frame_id != frame_id) continue;
        it->mbqp_attached = mbqp_attached ? 1 : 0;
        copy_cstr(it->actual_encode_control,
                  sizeof(it->actual_encode_control),
                  actual_encode_control);
        copy_cstr(it->encode_ctrl_reason,
                  sizeof(it->encode_ctrl_reason),
                  encode_ctrl_reason);
        std::snprintf(it->detail_json,
                      sizeof(it->detail_json),
                      "{\"has_mbqp\":%s,\"has_roi\":%s,\"mbqp_attached\":%s,"
                      "\"foreground_ratio\":%.4f,\"qp_p10\":%d,\"qp_p50\":%d,"
                      "\"qp_p90\":%d,\"qp_p95\":%d,\"high_qp_block_ratio\":%.4f,"
                      "\"low_importance_block_ratio\":%.4f,\"roi_block_ratio\":%.4f,"
                      "\"background_block_ratio\":%.4f,\"flat_background_block_ratio\":%.4f,"
                      "\"true_roi_block_ratio\":%.4f,"
                      "\"edge_texture_roi_block_ratio\":%.4f,"
                      "\"high_texture_background_block_ratio\":%.4f,"
                      "\"hard_scene_background_block_ratio\":%.4f,"
                      "\"normal_background_block_ratio\":%.4f,"
                      "\"scene_mode\":\"%s\",\"class_qp_table_name\":\"%s\","
                      "\"static_reuse_candidate\":%s,"
                      "\"static_reuse_consecutive_frames\":%d,"
                      "\"static_reuse_risk_score\":%.4f,"
                      "\"hard_scene_like\":%s,\"hard_guard_active\":%s,"
                      "\"class_qp_table_overwrite_count\":%d,"
                      "\"smoothing_changed_qp_avg\":%.4f}",
                      it->mbqp_requested ? "true" : "false",
                      it->roi_count > 0 ? "true" : "false",
                      it->mbqp_attached ? "true" : "false",
                      it->foreground_ratio,
                      it->qp_p10,
                      it->qp_p50,
                      it->qp_p90,
                      it->qp_p95,
                      it->high_qp_block_ratio,
                      it->low_importance_block_ratio,
                      it->roi_block_ratio,
                      it->background_block_ratio,
                      it->flat_background_block_ratio,
                      it->true_roi_block_ratio,
                      it->edge_texture_roi_block_ratio,
                      it->high_texture_background_block_ratio,
                      it->hard_scene_background_block_ratio,
                      it->normal_background_block_ratio,
                      it->scene_mode,
                      it->class_qp_table_name,
                      it->static_reuse_candidate ? "true" : "false",
                      it->static_reuse_consecutive_frames,
                      it->static_reuse_risk_score,
                      it->hard_scene_like ? "true" : "false",
                      it->hard_guard_active ? "true" : "false",
                      it->class_qp_table_overwrite_count,
                      it->smoothing_changed_qp_avg);
        return;
    }
}

} // namespace mfx50rt::hybridtsrq
