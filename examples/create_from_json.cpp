#include "mfx50rt.h"

#include <cstdio>

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "configs/safe_fallback.json";
    MFX50RT_Handle h = nullptr;
    MFX50RT_Status st = MFX50RT_CreateFromJson(path, 1, &h);
    if (st != MFX50RT_OK) {
        std::printf("CreateFromJson failed: %s\n", MFX50RT_StatusString(st));
        return 1;
    }
    MFX50RT_EffectiveConfig eff{};
    eff.size = sizeof(eff);
    eff.version = MFX50RT_API_VERSION;
    st = MFX50RT_GetEffectiveConfig(h, &eff);
    if (st == MFX50RT_OK) {
        std::printf("effective_strategy=%d fallback=%s\n",
                    eff.effective_strategy,
                    eff.fallback_reason);
    }
    MFX50RT_Close(h);
    return st == MFX50RT_OK ? 0 : 2;
}
