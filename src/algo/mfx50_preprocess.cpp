#include "mfx50_preprocess.h"

#include "mfx50_realtime.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

uint8_t clip8(int value) {
    return static_cast<uint8_t>(std::max(0, std::min(255, value)));
}

bool validNv12View(uint8_t* y,
                   uint8_t* uv,
                   int width,
                   int height,
                   int pitchY,
                   int pitchUV) {
    if (!y || !uv || width <= 0 || height <= 0 || pitchY <= 0 || pitchUV <= 0) {
        return false;
    }
    const int chromaBytes = ((width + 1) / 2) * 2;
    return pitchY >= width && pitchUV >= chromaBytes;
}

void denoisePlane(uint8_t* src, int width, int height, int stride, int strength) {
    const int threshold = std::max(2, strength * 30 / 100);
    std::vector<uint8_t> rowBuf(static_cast<size_t>(width));
    for (int y = 0; y < height; ++y) {
        uint8_t* rowCur = src + static_cast<size_t>(y) * stride;
        uint8_t* rowUp = y > 0 ? src + static_cast<size_t>(y - 1) * stride : rowCur;
        uint8_t* rowDn = y < height - 1 ? src + static_cast<size_t>(y + 1) * stride : rowCur;
        for (int x = 0; x < width; ++x) {
            const int center = rowCur[x];
            int sum = center;
            int count = 1;
            int n = rowUp[x];
            if (std::abs(n - center) < threshold) {
                sum += n;
                count++;
            }
            n = rowDn[x];
            if (std::abs(n - center) < threshold) {
                sum += n;
                count++;
            }
            if (x > 0) {
                n = rowCur[x - 1];
                if (std::abs(n - center) < threshold) {
                    sum += n;
                    count++;
                }
            }
            if (x < width - 1) {
                n = rowCur[x + 1];
                if (std::abs(n - center) < threshold) {
                    sum += n;
                    count++;
                }
            }
            rowBuf[static_cast<size_t>(x)] = clip8(sum / count);
        }
        std::memcpy(rowCur, rowBuf.data(), static_cast<size_t>(width));
    }
}

void smoothPlane(uint8_t* src, int width, int height, int stride, int factor) {
    const int selfWeight = std::max(1, factor / 10);
    const int totalWeight = selfWeight + 4;
    std::vector<uint8_t> rowBuf(static_cast<size_t>(width));
    for (int y = 0; y < height; ++y) {
        uint8_t* rowCur = src + static_cast<size_t>(y) * stride;
        uint8_t* rowUp = y > 0 ? src + static_cast<size_t>(y - 1) * stride : rowCur;
        uint8_t* rowDn = y < height - 1 ? src + static_cast<size_t>(y + 1) * stride : rowCur;
        for (int x = 0; x < width; ++x) {
            int sum = rowCur[x] * selfWeight;
            sum += rowUp[x];
            sum += rowDn[x];
            sum += x > 0 ? rowCur[x - 1] : rowCur[x];
            sum += x < width - 1 ? rowCur[x + 1] : rowCur[x];
            rowBuf[static_cast<size_t>(x)] = clip8(sum / totalWeight);
        }
        std::memcpy(rowCur, rowBuf.data(), static_cast<size_t>(width));
    }
}

void denoiseNv12Chroma(uint8_t* uv, int pairs, int height, int stride, int strength) {
    const int threshold = std::max(2, strength * 30 / 100);
    std::vector<uint8_t> rowBuf(static_cast<size_t>(pairs) * 2);
    for (int y = 0; y < height; ++y) {
        uint8_t* rowCur = uv + static_cast<size_t>(y) * stride;
        uint8_t* rowUp = y > 0 ? uv + static_cast<size_t>(y - 1) * stride : rowCur;
        uint8_t* rowDn = y < height - 1 ? uv + static_cast<size_t>(y + 1) * stride : rowCur;
        for (int x = 0; x < pairs; ++x) {
            for (int c = 0; c < 2; ++c) {
                const int offset = 2 * x + c;
                const int center = rowCur[offset];
                int sum = center;
                int count = 1;
                int n = rowUp[offset];
                if (std::abs(n - center) < threshold) {
                    sum += n;
                    count++;
                }
                n = rowDn[offset];
                if (std::abs(n - center) < threshold) {
                    sum += n;
                    count++;
                }
                if (x > 0) {
                    n = rowCur[offset - 2];
                    if (std::abs(n - center) < threshold) {
                        sum += n;
                        count++;
                    }
                }
                if (x < pairs - 1) {
                    n = rowCur[offset + 2];
                    if (std::abs(n - center) < threshold) {
                        sum += n;
                        count++;
                    }
                }
                rowBuf[static_cast<size_t>(offset)] = clip8(sum / count);
            }
        }
        std::memcpy(rowCur, rowBuf.data(), static_cast<size_t>(pairs) * 2);
    }
}

void smoothNv12Chroma(uint8_t* uv, int pairs, int height, int stride, int factor) {
    const int selfWeight = std::max(1, factor / 10);
    const int totalWeight = selfWeight + 4;
    std::vector<uint8_t> rowBuf(static_cast<size_t>(pairs) * 2);
    for (int y = 0; y < height; ++y) {
        uint8_t* rowCur = uv + static_cast<size_t>(y) * stride;
        uint8_t* rowUp = y > 0 ? uv + static_cast<size_t>(y - 1) * stride : rowCur;
        uint8_t* rowDn = y < height - 1 ? uv + static_cast<size_t>(y + 1) * stride : rowCur;
        for (int x = 0; x < pairs; ++x) {
            for (int c = 0; c < 2; ++c) {
                const int offset = 2 * x + c;
                int sum = rowCur[offset] * selfWeight;
                sum += rowUp[offset];
                sum += rowDn[offset];
                sum += x > 0 ? rowCur[offset - 2] : rowCur[offset];
                sum += x < pairs - 1 ? rowCur[offset + 2] : rowCur[offset];
                rowBuf[static_cast<size_t>(offset)] = clip8(sum / totalWeight);
            }
        }
        std::memcpy(rowCur, rowBuf.data(), static_cast<size_t>(pairs) * 2);
    }
}

int ceilDiv(int value, int denom) {
    return denom > 0 ? (value + denom - 1) / denom : 0;
}

std::vector<uint8_t> buildSemanticSmoothMap(const uint8_t* y,
                                            const uint8_t* uv,
                                            int width,
                                            int height,
                                            int pitchY,
                                            int pitchUV) {
    constexpr int kTile = 16;
    const int cols = ceilDiv(width, kTile);
    const int rows = ceilDiv(height, kTile);
    std::vector<uint8_t> map(static_cast<size_t>(cols) * rows, 0);
    if (width < 8 || height < 8) return map;

    for (int ty = 0; ty < rows; ++ty) {
        const int y0 = ty * kTile;
        const int y1 = std::min(height - 1, y0 + kTile);
        for (int tx = 0; tx < cols; ++tx) {
            const int x0 = tx * kTile;
            const int x1 = std::min(width - 1, x0 + kTile);
            int samples = 0;
            int mediumEdges = 0;
            int strongEdges = 0;
            int strokeLike = 0;
            int colorEdges = 0;
            uint64_t sum = 0;
            uint64_t sumSq = 0;

            for (int yy = std::max(1, y0); yy < y1; yy += 2) {
                const uint8_t* row = y + static_cast<size_t>(yy) * pitchY;
                const uint8_t* up = y + static_cast<size_t>(yy - 1) * pitchY;
                const uint8_t* dn = y + static_cast<size_t>(yy + 1) * pitchY;
                for (int xx = std::max(1, x0); xx < x1; xx += 2) {
                    const int center = row[xx];
                    const int gx = std::abs(static_cast<int>(row[xx + 1]) -
                                            static_cast<int>(row[xx - 1]));
                    const int gy = std::abs(static_cast<int>(dn[xx]) -
                                            static_cast<int>(up[xx]));
                    const int grad = std::max(gx, gy);
                    if (grad > 18) mediumEdges++;
                    if (grad > 44) strongEdges++;
                    if ((gx > 34 && gy < 16) || (gy > 34 && gx < 16)) strokeLike++;
                    if (uv) {
                        const int cx = (xx / 2) * 2;
                        const int cy = yy / 2;
                        if (cx + 3 < pitchUV && cy + 1 < (height + 1) / 2) {
                            const uint8_t* uvRow = uv + static_cast<size_t>(cy) * pitchUV;
                            const uint8_t* uvDn = uv + static_cast<size_t>(cy + 1) * pitchUV;
                            const int dux = std::abs(static_cast<int>(uvRow[cx + 2]) -
                                                     static_cast<int>(uvRow[cx]));
                            const int dvx = std::abs(static_cast<int>(uvRow[cx + 3]) -
                                                     static_cast<int>(uvRow[cx + 1]));
                            const int duy = std::abs(static_cast<int>(uvDn[cx]) -
                                                     static_cast<int>(uvRow[cx]));
                            const int dvy = std::abs(static_cast<int>(uvDn[cx + 1]) -
                                                     static_cast<int>(uvRow[cx + 1]));
                            if (std::max(std::max(dux, dvx), std::max(duy, dvy)) > 28) {
                                colorEdges++;
                            }
                        }
                    }
                    sum += static_cast<uint64_t>(center);
                    sumSq += static_cast<uint64_t>(center) * center;
                    samples++;
                }
            }

            if (samples <= 0) continue;
            const double mean = static_cast<double>(sum) / samples;
            const double variance = static_cast<double>(sumSq) / samples - mean * mean;
            const double edgeDensity = static_cast<double>(mediumEdges) / samples;
            const double strongDensity = static_cast<double>(strongEdges) / samples;
            const double strokeDensity = static_cast<double>(strokeLike) / samples;
            const double colorDensity = static_cast<double>(colorEdges) / samples;

            uint8_t level = 0;
            const bool likelyPanelTextOrVehicle =
                (strokeDensity > 0.10 && edgeDensity < 0.42) ||
                (colorDensity > 0.10 && edgeDensity < 0.36) ||
                (strongDensity > 0.08 && edgeDensity < 0.28);
            if (!likelyPanelTextOrVehicle) {
                if (edgeDensity > 0.34 || (variance > 360.0 && edgeDensity > 0.18)) {
                    level = 2;
                } else if (edgeDensity < 0.06 || (variance > 160.0 && edgeDensity > 0.10)) {
                    level = 1;
                }
            }
            map[static_cast<size_t>(ty) * cols + tx] = level;
        }
    }
    return map;
}

void semanticSmoothPlane(uint8_t* src,
                         int width,
                         int height,
                         int stride,
                         int factor,
                         const std::vector<uint8_t>& tileMap) {
    constexpr int kTile = 16;
    const int cols = ceilDiv(width, kTile);
    const int safeFactor = std::max(1, std::min(100, factor));
    const int selfWeightBase = std::max(3, safeFactor / 10 + 2);
    std::vector<uint8_t> rowBuf(static_cast<size_t>(width));
    for (int y = 0; y < height; ++y) {
        uint8_t* rowCur = src + static_cast<size_t>(y) * stride;
        uint8_t* rowUp = y > 0 ? src + static_cast<size_t>(y - 1) * stride : rowCur;
        uint8_t* rowDn = y < height - 1 ? src + static_cast<size_t>(y + 1) * stride : rowCur;
        std::memcpy(rowBuf.data(), rowCur, static_cast<size_t>(width));
        for (int x = 0; x < width; ++x) {
            const uint8_t level = tileMap.empty()
                ? 0
                : tileMap[static_cast<size_t>(y / kTile) * cols + (x / kTile)];
            if (!level) continue;

            const int center = rowCur[x];
            const int left = x > 0 ? rowCur[x - 1] : center;
            const int right = x + 1 < width ? rowCur[x + 1] : center;
            const int up = rowUp[x];
            const int down = rowDn[x];
            const int gx = std::abs(right - left);
            const int gy = std::abs(down - up);
            const int laplace = std::abs(center * 4 - left - right - up - down);
            if ((std::max(gx, gy) > 52 && laplace > 34) ||
                (std::max(gx, gy) > 72)) {
                continue;
            }

            const int threshold = level >= 2 ? 24 : 22;
            const int selfWeight = level >= 2 ? selfWeightBase : selfWeightBase + 2;
            int sum = center * selfWeight;
            int weight = selfWeight;
            auto addSimilar = [&](int n) {
                if (std::abs(n - center) <= threshold) {
                    sum += n;
                    weight++;
                }
            };
            addSimilar(left);
            addSimilar(right);
            addSimilar(up);
            addSimilar(down);
            if (weight > selfWeight) {
                rowBuf[static_cast<size_t>(x)] = clip8((sum + weight / 2) / weight);
            }
        }
        std::memcpy(rowCur, rowBuf.data(), static_cast<size_t>(width));
    }
}

void semanticSmoothNv12Chroma(uint8_t* uv,
                              int pairs,
                              int height,
                              int stride,
                              int factor,
                              const std::vector<uint8_t>& tileMap,
                              int lumaWidth) {
    constexpr int kTile = 16;
    const int cols = ceilDiv(lumaWidth, kTile);
    const int safeFactor = std::max(1, std::min(100, factor));
    const int selfWeight = std::max(2, safeFactor / 12 + 1);
    std::vector<uint8_t> rowBuf(static_cast<size_t>(pairs) * 2);
    for (int y = 0; y < height; ++y) {
        uint8_t* rowCur = uv + static_cast<size_t>(y) * stride;
        uint8_t* rowUp = y > 0 ? uv + static_cast<size_t>(y - 1) * stride : rowCur;
        uint8_t* rowDn = y < height - 1 ? uv + static_cast<size_t>(y + 1) * stride : rowCur;
        std::memcpy(rowBuf.data(), rowCur, static_cast<size_t>(pairs) * 2);
        for (int x = 0; x < pairs; ++x) {
            const int lumaX = x * 2;
            const int lumaY = y * 2;
            const uint8_t level = tileMap.empty()
                ? 0
                : tileMap[static_cast<size_t>(lumaY / kTile) * cols + (lumaX / kTile)];
            if (!level) continue;
            for (int c = 0; c < 2; ++c) {
                const int offset = x * 2 + c;
                const int center = rowCur[offset];
                const int left = x > 0 ? rowCur[offset - 2] : center;
                const int right = x + 1 < pairs ? rowCur[offset + 2] : center;
                const int up = rowUp[offset];
                const int down = rowDn[offset];
                if (std::max(std::abs(right - left), std::abs(down - up)) > 36) {
                    continue;
                }
                const int threshold = level >= 2 ? 28 : 20;
                int sum = center * selfWeight;
                int weight = selfWeight;
                auto addSimilar = [&](int n) {
                    if (std::abs(n - center) <= threshold) {
                        sum += n;
                        weight++;
                    }
                };
                addSimilar(left);
                addSimilar(right);
                addSimilar(up);
                addSimilar(down);
                if (weight > selfWeight) {
                    rowBuf[static_cast<size_t>(offset)] = clip8((sum + weight / 2) / weight);
                }
            }
        }
        std::memcpy(rowCur, rowBuf.data(), static_cast<size_t>(pairs) * 2);
    }
}

} // namespace

int mfx50_preprocess_smooth_scale_nv12(uint8_t* y,
                                       uint8_t* uv,
                                       int width,
                                       int height,
                                       int pitch_y,
                                       int pitch_uv,
                                       int factor) {
    if (!validNv12View(y, uv, width, height, pitch_y, pitch_uv)) {
        return MFX50_ERR_INVALID_ARG;
    }
    const int safeFactor = std::max(1, std::min(100, factor));
    smoothPlane(y, width, height, pitch_y, safeFactor);
    smoothNv12Chroma(uv, (width + 1) / 2, (height + 1) / 2, pitch_uv, safeFactor);
    return MFX50_OK;
}

int mfx50_preprocess_semantic_smooth_nv12(uint8_t* y,
                                          uint8_t* uv,
                                          int width,
                                          int height,
                                          int pitch_y,
                                          int pitch_uv,
                                          int factor) {
    if (!validNv12View(y, uv, width, height, pitch_y, pitch_uv)) {
        return MFX50_ERR_INVALID_ARG;
    }
    const int safeFactor = std::max(1, std::min(100, factor));
    const std::vector<uint8_t> map =
        buildSemanticSmoothMap(y, uv, width, height, pitch_y, pitch_uv);
    semanticSmoothPlane(y, width, height, pitch_y, safeFactor, map);
    semanticSmoothNv12Chroma(uv,
                             (width + 1) / 2,
                             (height + 1) / 2,
                             pitch_uv,
                             safeFactor,
                             map,
                             width);
    return MFX50_OK;
}

int mfx50_preprocess_denoise_nv12(uint8_t* y,
                                  uint8_t* uv,
                                  int width,
                                  int height,
                                  int pitch_y,
                                  int pitch_uv,
                                  int strength) {
    if (!validNv12View(y, uv, width, height, pitch_y, pitch_uv)) {
        return MFX50_ERR_INVALID_ARG;
    }
    const int safeStrength = std::max(1, std::min(100, strength));
    denoisePlane(y, width, height, pitch_y, safeStrength);
    denoiseNv12Chroma(uv, (width + 1) / 2, (height + 1) / 2, pitch_uv, safeStrength);
    return MFX50_OK;
}
