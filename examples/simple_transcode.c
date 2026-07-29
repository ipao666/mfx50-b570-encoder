#include "mfx50rt.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    MFX50RT_Config cfg;
    if (MFX50RT_DefaultConfig(&cfg) != MFX50RT_OK) return 1;
    cfg.backend.type = MFX50RT_BACKEND_NULL;
    cfg.runtime.async_mode = 0;
    cfg.algo.strategy = MFX50RT_STRATEGY_GLOBAL;

    MFX50RT_Handle h = NULL;
    if (MFX50RT_Create(&cfg, &h) != MFX50RT_OK) return 2;

    unsigned char input[1024];
    memset(input, 0x55, sizeof(input));
    MFX50RT_InputPacket in = {0};
    in.size = sizeof(in);
    in.version = MFX50RT_API_VERSION;
    in.stream_id = 0;
    in.data = input;
    in.data_size = sizeof(input);
    in.pts = 0;
    in.dts = 0;
    if (MFX50RT_PushPacket(h, &in) != MFX50RT_OK) return 3;

    MFX50RT_OutputPacket out = {0};
    MFX50RT_Status st = MFX50RT_PollPacket(h, &out, 100);
    if (st == MFX50RT_OK) {
        printf("stream=%u bytes=%u qp=%d\n", out.stream_id, out.data_size, out.qp_avg);
        MFX50RT_ReleasePacket(h, &out);
    }
    MFX50RT_Close(h);
    return st == MFX50RT_OK ? 0 : 4;
}
