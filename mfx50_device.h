#pragma once

#include "mfx50_api.h"
#include "mfx50_types.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MFX50_DEVICE_API_VERSION 1u

typedef struct MFX50_Device MFX50_Device;

typedef enum MFX50_DeviceInteropType {
    MFX50_DEVICE_INTEROP_AUTO = 0,
    MFX50_DEVICE_INTEROP_VAAPI = 1,
    MFX50_DEVICE_INTEROP_ONEVPL = 2
} MFX50_DeviceInteropType;

typedef struct MFX50_DeviceConfig {
    uint32_t struct_size;
    uint32_t api_version;
    int32_t device_index;
    char device_path[128];
    MFX50_DeviceInteropType interop_type;
    void* external_va_display;
    void* external_mfx_session;
    int32_t take_external_ownership;
    int32_t require_zero_copy;
    char options_json[1024];
    uint8_t reserved[256];
} MFX50_DeviceConfig;

MFX50_API const char* mfx50_status_string(MFX50_Status status);

MFX50_API MFX50_Status mfx50_device_default_config(MFX50_DeviceConfig* config);

MFX50_API MFX50_Status mfx50_device_create(
    const MFX50_DeviceConfig* config,
    MFX50_Device** out_device);

MFX50_API const char* mfx50_device_get_last_error(MFX50_Device* device);

MFX50_API void mfx50_device_destroy(MFX50_Device* device);

#ifdef __cplusplus
}
#endif
