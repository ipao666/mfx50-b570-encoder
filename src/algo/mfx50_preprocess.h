#pragma once

#include <stdint.h>

int mfx50_preprocess_smooth_scale_nv12(
    uint8_t* y,
    uint8_t* uv,
    int width,
    int height,
    int pitch_y,
    int pitch_uv,
    int factor);

int mfx50_preprocess_semantic_smooth_nv12(
    uint8_t* y,
    uint8_t* uv,
    int width,
    int height,
    int pitch_y,
    int pitch_uv,
    int factor);

int mfx50_preprocess_denoise_nv12(
    uint8_t* y,
    uint8_t* uv,
    int width,
    int height,
    int pitch_y,
    int pitch_uv,
    int strength);
