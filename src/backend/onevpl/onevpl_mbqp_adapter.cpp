#include "onevpl_mbqp_adapter.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <vector>

namespace mfx50rt::onevpl {

namespace {

int ceil_div(int value, int denom) {
    return denom > 0 ? (value + denom - 1) / denom : 0;
}

uint8_t clamp_qp(int value) {
    return static_cast<uint8_t>(std::max(1, std::min(51, value)));
}

void set_error(std::string* error, const char* msg) {
    if (error) *error = msg ? msg : "";
}

void copy_reason(char* dst, size_t dst_size, const char* reason) {
    if (!dst || dst_size == 0) return;
    std::snprintf(dst, dst_size, "%s", reason ? reason : "");
}

bool write_qp_map(const uint8_t* qp,
                  int block_cols,
                  int block_rows,
                  int pitch,
                  int frame_anchor_qp,
                  const char* reason,
                  MFX50RT_InternalEncodeDecision* out,
                  std::string* error) {
    if (!out || !qp) {
        set_error(error, "MBQP decision output or source map is null");
        return false;
    }
    if (block_cols <= 0 || block_rows <= 0 || pitch < block_cols) {
        set_error(error, "invalid MBQP geometry");
        return false;
    }
    const uint64_t needed = static_cast<uint64_t>(pitch) * static_cast<uint64_t>(block_rows);
    if (!out->mbqp_qp_buffer || needed == 0 || needed > out->mbqp_qp_capacity) {
        set_error(error, "internal MBQP buffer capacity is too small");
        return false;
    }
    int qp_min = 51;
    int qp_max = 1;
    uint64_t qp_sum = 0;
    uint64_t qp_count = 0;
    for (int row = 0; row < block_rows; ++row) {
        for (int col = 0; col < block_cols; ++col) {
            const size_t src_idx = static_cast<size_t>(row) * block_cols + col;
            const size_t dst_idx = static_cast<size_t>(row) * pitch + col;
            const uint8_t q = clamp_qp(qp[src_idx]);
            out->mbqp_qp_buffer[dst_idx] = q;
            qp_min = std::min(qp_min, static_cast<int>(q));
            qp_max = std::max(qp_max, static_cast<int>(q));
            qp_sum += q;
            qp_count++;
        }
        for (int col = block_cols; col < pitch; ++col) {
            out->mbqp_qp_buffer[static_cast<size_t>(row) * pitch + col] =
                clamp_qp(frame_anchor_qp);
        }
    }

    out->strategy = MFX50RT_INTERNAL_CONTROL_MBQP;
    out->has_mbqp = 1;
    out->mbqp_block_size = 16;
    out->mbqp_pitch = pitch;
    out->mbqp_block_cols = block_cols;
    out->mbqp_block_rows = block_rows;
    out->mbqp_num_qp_alloc = static_cast<uint32_t>(needed);
    out->spatial_min_qp = qp_min;
    out->spatial_max_qp = qp_max;
    out->spatial_avg_qp = qp_count > 0 ? static_cast<int>(qp_sum / qp_count) : frame_anchor_qp;
    out->frame_anchor_qp = frame_anchor_qp;
    copy_reason(out->reason, sizeof(out->reason), reason);
    return true;
}

} // namespace

MbqpFrameSummary summarizeMbqpMap(const mfx50rt::hybridtsrq::QpMap16x16& map) {
    MbqpFrameSummary out;
    if (map.qp.empty()) return out;
    auto [min_it, max_it] = std::minmax_element(map.qp.begin(), map.qp.end());
    const uint64_t sum = std::accumulate(map.qp.begin(), map.qp.end(), uint64_t{0});
    out.valid = true;
    out.qp_min = *min_it;
    out.qp_max = *max_it;
    out.qp_avg = static_cast<int>(sum / map.qp.size());
    out.block_count = static_cast<int>(map.qp.size());
    return out;
}

bool fillInternalMbqpDecisionFromMap(const mfx50rt::hybridtsrq::QpMap16x16& map,
                                     int frame_anchor_qp,
                                     MFX50RT_InternalEncodeDecision* out,
                                     std::string* error) {
    if (map.block_cols <= 0 || map.block_rows <= 0 ||
        map.qp.size() != static_cast<size_t>(map.block_cols) * map.block_rows) {
        set_error(error, "HybridTSRQ QpMap16x16 is empty or inconsistent");
        return false;
    }
    return write_qp_map(map.qp.data(),
                        map.block_cols,
                        map.block_rows,
                        map.block_cols,
                        frame_anchor_qp,
                        "HybridTSRQ MBQP map attached",
                        out,
                        error);
}

bool fillInternalForceMbqpPattern(const char* pattern,
                                  int width,
                                  int height,
                                  int frame_anchor_qp,
                                  MFX50RT_InternalEncodeDecision* out,
                                  std::string* error) {
    const std::string p = pattern ? pattern : "none";
    if (p.empty() || p == "none") {
        set_error(error, "force_mbqp_pattern is none");
        return false;
    }
    const int block_cols = ceil_div(width, 16);
    const int block_rows = ceil_div(height, 16);
    if (block_cols <= 0 || block_rows <= 0) {
        set_error(error, "invalid force MBQP dimensions");
        return false;
    }
    std::vector<uint8_t> qp(static_cast<size_t>(block_cols) * block_rows,
                            clamp_qp(frame_anchor_qp));
    if (p == "flat_low_qp") {
        std::fill(qp.begin(), qp.end(), 24);
    } else if (p == "flat_high_qp") {
        std::fill(qp.begin(), qp.end(), 44);
    } else if (p == "checkerboard") {
        for (int y = 0; y < block_rows; ++y) {
            for (int x = 0; x < block_cols; ++x) {
                qp[static_cast<size_t>(y) * block_cols + x] =
                    ((x + y) & 1) ? 44 : 24;
            }
        }
    } else if (p == "roi_center_low_qp") {
        std::fill(qp.begin(), qp.end(), 44);
        const int x0 = block_cols / 4;
        const int x1 = block_cols - x0;
        const int y0 = block_rows / 4;
        const int y1 = block_rows - y0;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                qp[static_cast<size_t>(y) * block_cols + x] = 24;
            }
        }
    } else {
        set_error(error, "unknown force_mbqp_pattern");
        return false;
    }

    std::string reason = "force_mbqp_pattern=" + p;
    return write_qp_map(qp.data(),
                        block_cols,
                        block_rows,
                        block_cols,
                        frame_anchor_qp,
                        reason.c_str(),
                        out,
                        error);
}

} // namespace mfx50rt::onevpl
