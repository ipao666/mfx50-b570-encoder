#include "mfx50_policy.h"

#include <stdio.h>
#include <string.h>

int main(void) {
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
    cfg.encoder_caps.max_roi_count = 64;
    cfg.encoder_caps.roi_alignment = 16;
    cfg.encoder_caps.min_qp = 1;
    cfg.encoder_caps.max_qp = 51;
    cfg.encoder_caps.min_delta_qp = -16;
    cfg.encoder_caps.max_delta_qp = 16;
    cfg.encoder_caps.max_b_frame_dist = 4;
    cfg.encoder_caps.max_gop_size = 300;

    MFX50_PolicyContext* ctx = NULL;
    if (mfx50_policy_create(&cfg, &ctx) != MFX50_OK) {
        fprintf(stderr, "failed to create policy context\n");
        return 1;
    }

    MFX50_StreamConfig stream_cfg;
    memset(&stream_cfg, 0, sizeof(stream_cfg));
    stream_cfg.struct_size = sizeof(stream_cfg);
    stream_cfg.stream_id = 0;
    stream_cfg.width = 1920;
    stream_cfg.height = 1080;
    stream_cfg.fps = 30.0f;
    stream_cfg.input_codec = MFX50_CODEC_H264;
    stream_cfg.pixel_format = MFX50_PIXFMT_NV12;
    stream_cfg.is_realtime = 1;

    MFX50_PolicyStream* stream = NULL;
    if (mfx50_policy_create_stream(ctx, &stream_cfg, &stream) != MFX50_OK) {
        fprintf(stderr, "failed to create policy stream\n");
        mfx50_policy_destroy(ctx);
        return 1;
    }

    MFX50_FrameFeatures features;
    memset(&features, 0, sizeof(features));
    features.struct_size = sizeof(features);
    features.frame_index = 1;
    features.mean_y = 104.0f;
    features.edge_density = 0.18f;
    features.motion_score = 0.12f;
    features.recent_compression_percent = 85.0f;
    mfx50_policy_submit_features(stream, &features);

    MFX50_Metadata metadata;
    memset(&metadata, 0, sizeof(metadata));
    metadata.struct_size = sizeof(metadata);
    metadata.object_count = 1;
    metadata.objects[0].type = MFX50_OBJECT_PLATE;
    metadata.objects[0].x = 640;
    metadata.objects[0].y = 480;
    metadata.objects[0].w = 120;
    metadata.objects[0].h = 36;
    metadata.objects[0].confidence = 0.9f;
    metadata.objects[0].track_id = 42;
    mfx50_policy_submit_metadata(stream, &metadata);

    MFX50_EncodeDecision decision;
    memset(&decision, 0, sizeof(decision));
    decision.struct_size = sizeof(decision);
    if (mfx50_policy_get_decision(stream, &decision) == MFX50_OK) {
        printf("profile=%d qpi=%d qpp=%d qpb=%d gop=%d bdist=%d roi_count=%d reason=%s\n",
               decision.profile_id,
               decision.qpi,
               decision.qpp,
               decision.qpb,
               decision.gop_size,
               decision.b_frame_dist,
               decision.roi_count,
               decision.reason);
        for (int i = 0; i < decision.roi_count; ++i) {
            const MFX50_Roi* roi = &decision.rois[i];
            printf("roi[%d] x=%d y=%d w=%d h=%d delta_qp=%d type=%d priority=%d\n",
                   i, roi->x, roi->y, roi->w, roi->h, roi->delta_qp, roi->type, roi->priority);
        }
    }

    mfx50_policy_destroy_stream(stream);
    mfx50_policy_destroy(ctx);
    return 0;
}
