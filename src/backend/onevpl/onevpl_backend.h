#pragma once

#include "mfx50rt.h"

namespace mfx50rt::onevpl {

MFX50RT_Status queryConservativeCapabilities(const MFX50RT_BackendConfig& backend,
                                             MFX50RT_Capabilities* caps);
MFX50RT_Status queryRealCapabilities(const MFX50RT_BackendConfig& backend,
                                     MFX50RT_Capabilities* caps);

} // namespace mfx50rt::onevpl
