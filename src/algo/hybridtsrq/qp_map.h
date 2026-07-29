#pragma once

#include "hybridtsrq_types.h"
#include "tuning_profile.h"

namespace mfx50rt::hybridtsrq {

CtuMap build_ctu_importance_map(const RoiAnalysisResult& roi,
                                const FastFrameFeatures& fast,
                                int width,
                                int height,
                                int ctu_size);

QpMap16x16 ctu_qp_to_16x16_mbqp(const CtuMap& ctu_map,
                                int width,
                                int height,
                                int frame_anchor_qp,
                                const QualityState& quality);

QpMap16x16 ctu_qp_to_16x16_mbqp(const CtuMap& ctu_map,
                                int width,
                                int height,
                                int frame_anchor_qp,
                                const QualityState& quality,
                                const HybridTsrqQpTuning& tuning);

QpDistributionStats summarize_qp_distribution(const QpMap16x16& map);
RegionBlockStats summarize_region_blocks(const CtuMap& ctu_map,
                                         const RoiAnalysisResult& roi);

void smooth_qp_map(QpMap16x16& map, int max_neighbor_delta);
void clamp_qp_map(QpMap16x16& map, int min_qp, int max_qp);

} // namespace mfx50rt::hybridtsrq
