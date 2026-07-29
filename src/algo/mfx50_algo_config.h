#pragma once

#include "mfx50_realtime_algo.h"

namespace mfx50rt {

void defaultAlgoConfig(MFX50RT_AlgoConfig* cfg);
MFX50RT_AlgoConfig normalizedAlgoConfig(const MFX50RT_AlgoConfig* cfg);
MFX50RT_AlgoCaps buildAlgoCaps();
int activeAlgoFlags(const MFX50RT_AlgoConfig& cfg);

} // namespace mfx50rt
