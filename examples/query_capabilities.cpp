#include "mfx50rt.h"

#include <cstdio>

int main(void) {
    MFX50RT_Config cfg{};
    MFX50RT_DefaultConfig(&cfg);
    cfg.backend.type = MFX50RT_BACKEND_ONEVPL;

    MFX50RT_Capabilities caps{};
    caps.size = sizeof(caps);
    caps.version = MFX50RT_API_VERSION;
    MFX50RT_Status st = MFX50RT_QueryCapabilities(&cfg.backend, &caps);
    if (st != MFX50RT_OK) {
        std::printf("query failed: %s\n", MFX50RT_StatusString(st));
        return 1;
    }
    std::printf("backend=%s hevc_encode=%d mbqp=%d roi=%d\n",
                caps.backend_name,
                caps.supports_hevc_encode,
                caps.supports_mbqp,
                caps.supports_roi_delta_qp);
    return 0;
}
