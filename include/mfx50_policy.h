#ifndef MFX50_POLICY_H
#define MFX50_POLICY_H

#include "mfx50_types.h"

#ifdef __cplusplus
extern "C" {
#endif

MFX50_POLICY_API MFX50_Status mfx50_policy_get_version(MFX50_Version* out_version);

MFX50_POLICY_API MFX50_Status mfx50_policy_create(const MFX50_PolicyConfig* config,
                                                  MFX50_PolicyContext** out_context);
MFX50_POLICY_API void mfx50_policy_destroy(MFX50_PolicyContext* context);

MFX50_POLICY_API MFX50_Status mfx50_policy_create_stream(MFX50_PolicyContext* context,
                                                         const MFX50_StreamConfig* config,
                                                         MFX50_PolicyStream** out_stream);
MFX50_POLICY_API void mfx50_policy_destroy_stream(MFX50_PolicyStream* stream);
MFX50_POLICY_API MFX50_Status mfx50_policy_reset_stream(MFX50_PolicyStream* stream,
                                                        MFX50_ResetMode mode);

MFX50_POLICY_API MFX50_Status mfx50_policy_submit_features(MFX50_PolicyStream* stream,
                                                           const MFX50_FrameFeatures* features);
MFX50_POLICY_API MFX50_Status mfx50_policy_submit_metadata(MFX50_PolicyStream* stream,
                                                           const MFX50_Metadata* metadata);
MFX50_POLICY_API MFX50_Status mfx50_policy_submit_frame(MFX50_PolicyStream* stream,
                                                        const MFX50_AnalyzeFrame* frame);

MFX50_POLICY_API MFX50_Status mfx50_policy_get_decision(MFX50_PolicyStream* stream,
                                                        MFX50_EncodeDecision* decision);

MFX50_POLICY_API MFX50_Status mfx50_policy_set_option(MFX50_PolicyContext* context,
                                                      const char* key,
                                                      const char* value);
MFX50_POLICY_API MFX50_Status mfx50_policy_get_option(MFX50_PolicyContext* context,
                                                      const char* key,
                                                      char* value,
                                                      size_t value_capacity);

MFX50_POLICY_API MFX50_Status mfx50_policy_get_stats(MFX50_PolicyStream* stream,
                                                     MFX50_PolicyStats* stats);
MFX50_POLICY_API MFX50_Status mfx50_policy_set_log_callback(MFX50_PolicyContext* context,
                                                            MFX50_LogCallback callback,
                                                            void* user_data);

#ifdef __cplusplus
}
#endif

#endif
