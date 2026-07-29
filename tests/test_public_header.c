#include "mfx50rt.h"

#include <assert.h>

int main(void) {
    MFX50RT_Config cfg;
    assert(MFX50RT_DefaultConfig(&cfg) == MFX50RT_OK);
    assert(cfg.size == sizeof(cfg));
    assert(cfg.version == MFX50RT_API_VERSION);
    assert(cfg.algo.target_compression_percent == 90);
    return 0;
}
