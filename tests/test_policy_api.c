#include "mfx50_policy.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    MFX50_Version version;
    memset(&version, 0, sizeof(version));
    version.struct_size = sizeof(version);
    assert(mfx50_policy_get_version(&version) == MFX50_OK);
    assert(version.api_version == MFX50_POLICY_API_VERSION);

    MFX50_PolicyConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.api_version = MFX50_POLICY_API_VERSION;
    cfg.mode = MFX50_MODE_TARGET_90;
    cfg.target_streams = 45;
    cfg.input_fps = 30.0f;
    cfg.target_encode_fps = 30.0f;
    cfg.encoder_caps.struct_size = sizeof(cfg.encoder_caps);
    cfg.encoder_caps.supports_b_frames = 1;
    cfg.encoder_caps.supports_roi = 1;
    cfg.encoder_caps.supports_dynamic_gop = 1;
    cfg.encoder_caps.supports_idr_request = 1;
    cfg.encoder_caps.supports_positive_delta_qp = 1;
    cfg.encoder_caps.supports_negative_delta_qp = 1;
    cfg.encoder_caps.supports_roi_delta_qp = 1;
    cfg.encoder_caps.max_roi_count = 4;
    cfg.encoder_caps.roi_alignment = 16;
    cfg.encoder_caps.min_qp = 1;
    cfg.encoder_caps.max_qp = 51;
    cfg.encoder_caps.min_delta_qp = -16;
    cfg.encoder_caps.max_delta_qp = 16;
    cfg.encoder_caps.max_b_frame_dist = 4;
    cfg.encoder_caps.max_gop_size = 300;

    MFX50_PolicyContext* ctx = NULL;
    assert(mfx50_policy_create(&cfg, &ctx) == MFX50_OK);
    assert(ctx != NULL);
    assert(mfx50_policy_set_option(ctx, "plate.delta_qp", "-10") == MFX50_OK);
    char value[32];
    assert(mfx50_policy_get_option(ctx, "plate.delta_qp", value, sizeof(value)) == MFX50_OK);
    assert(strcmp(value, "-10") == 0);

    MFX50_StreamConfig scfg;
    memset(&scfg, 0, sizeof(scfg));
    scfg.struct_size = sizeof(scfg);
    scfg.stream_id = 7;
    scfg.width = 1920;
    scfg.height = 1080;
    scfg.fps = 30.0f;
    scfg.input_codec = MFX50_CODEC_H264;
    scfg.pixel_format = MFX50_PIXFMT_NV12;
    scfg.is_realtime = 1;

    MFX50_PolicyStream* stream = NULL;
    assert(mfx50_policy_create_stream(ctx, &scfg, &stream) == MFX50_OK);
    assert(stream != NULL);

    MFX50_FrameFeatures features;
    memset(&features, 0, sizeof(features));
    features.struct_size = sizeof(features);
    features.frame_index = 1;
    features.mean_y = 104.0f;
    features.edge_density = 0.18f;
    features.recent_compression_percent = 82.0f;
    assert(mfx50_policy_submit_features(stream, &features) == MFX50_OK);

    MFX50_Metadata meta;
    memset(&meta, 0, sizeof(meta));
    meta.struct_size = sizeof(meta);
    meta.object_count = 2;
    meta.objects[0].type = MFX50_OBJECT_VEHICLE;
    meta.objects[0].x = 101;
    meta.objects[0].y = 201;
    meta.objects[0].w = 120;
    meta.objects[0].h = 80;
    meta.objects[0].confidence = 0.8f;
    meta.objects[1].type = MFX50_OBJECT_PLATE;
    meta.objects[1].x = 130;
    meta.objects[1].y = 250;
    meta.objects[1].w = 60;
    meta.objects[1].h = 20;
    meta.objects[1].confidence = 0.9f;
    assert(mfx50_policy_submit_metadata(stream, &meta) == MFX50_OK);

    MFX50_EncodeDecision decision;
    memset(&decision, 0, sizeof(decision));
    decision.struct_size = sizeof(decision);
    assert(mfx50_policy_get_decision(stream, &decision) == MFX50_OK);
    assert(decision.profile_id == MFX50_PROFILE_DAY_GUARD_Q34);
    assert(decision.qpi == 34);
    assert(decision.qpp == 36);
    assert(decision.qpb == 42);
    assert(decision.b_frame_dist == 4);
    assert(decision.roi_count == 2);
    assert(decision.rois[0].x % 16 == 0);
    assert(decision.rois[0].delta_qp < 0);
    assert(decision.rois[0].delta_qp == -10);

    MFX50_PolicyStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.struct_size = sizeof(stats);
    assert(mfx50_policy_get_stats(stream, &stats) == MFX50_OK);
    assert(stats.frames_seen == 1);
    assert(stats.decisions_made == 1);

    mfx50_policy_destroy_stream(stream);
    mfx50_policy_destroy(ctx);
    puts("policy api ok");
    return 0;
}
