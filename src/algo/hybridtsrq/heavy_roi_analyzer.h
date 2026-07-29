#pragma once

#include "hybridtsrq_types.h"

#include <stdint.h>

#include <vector>

namespace mfx50rt::hybridtsrq {

class HeavyRoiAnalyzer {
public:
    void configure(int width, int height, int ctu_size, int interval);
    bool shouldAnalyze(const FastFrameFeatures& fast) const;
    RoiAnalysisResult analyzeYPlane(const uint8_t* y, int width, int height, int pitch);
    RoiAnalysisResult analyzeNV12(const uint8_t* y,
                                  int width,
                                  int height,
                                  int pitch,
                                  const uint8_t* uv,
                                  int uv_pitch);
    void markOverload();

private:
    void downsample(const uint8_t* y, int pitch, int width, int height);
    void downsampleNV12Chroma(const uint8_t* uv, int uv_pitch, int width, int height);
    void buildForegroundMask();
    bool isTrafficCorridorPixel(int x, int y) const;
    bool isLaneMarkCandidate(size_t idx, int gx, int gy) const;
    void morphOpen();
    void buildCtuFeatures(RoiAnalysisResult& out, int width, int height);
    void extractRois(RoiAnalysisResult& out, int width, int height);
    void extractTextSignRois(RoiAnalysisResult& out, int width, int height);
    bool isTextSignZonePixel(int x, int y) const;
    bool overlapsExistingRoi(const RoiAnalysisResult& out, int x, int y, int w, int h) const;

    int width_ = 0;
    int height_ = 0;
    int ctu_size_ = 64;
    int interval_ = 5;
    int overload_extra_interval_ = 0;
    mutable int frames_until_next_ = 0;
    uint64_t frame_id_ = 0;
    bool has_background_ = false;
    bool has_previous_frame_ = false;
    int ds_w_ = 0;
    int ds_h_ = 0;
    std::vector<uint8_t> ds_y_;
    std::vector<uint8_t> ds_u_;
    std::vector<uint8_t> ds_v_;
    std::vector<uint8_t> prev_ds_y_;
    std::vector<uint8_t> bg_y_;
    std::vector<uint8_t> fg_mask_;
    std::vector<uint8_t> temp_mask_;
    bool has_chroma_ = false;
};

} // namespace mfx50rt::hybridtsrq
