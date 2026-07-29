# MFX50 Device Session Fix 20260605

## 现象

工程师在 `rtsp_proc` 中调用 split codec API 时看到：

```text
decoder_create failed: MFX50_ERR_DEVICE
(MFXCreateSession failed: mfxStatus(-9))
```

oneVPL 中 `mfxStatus(-9)` 对应 `MFX_ERR_NOT_FOUND`，含义是 loader 没找到匹配的实现或设备。

## 根因

旧实现会先创建 oneVPL session：

```text
MFXLoad -> loader filter: HW + VAAPI -> MFXCreateSession
```

然后才打开默认 VA display：

```text
/dev/dri/renderD129 -> vaGetDisplayDRM -> vaInitialize
```

这在部分 B570 测试环境可以工作，但在工程师环境中，oneVPL loader 在 `MFXCreateSession()` 阶段还不知道应该选择哪个 DRM render node，于是返回 `MFX_ERR_NOT_FOUND`。

## 修复

`mfx50_device_create()` 内部现在会：

1. 从 `MFX50_DeviceConfig.device_path` 解析 DRM render node，例如 `/dev/dri/renderD129` -> `129`。
2. 在 `MFXCreateSession()` 前设置 oneVPL loader filter：

```text
mfxExtendedDeviceId.DRMRenderNodeNum = 129
```

3. session 创建后继续执行：

```text
MFXVideoCORE_SetHandle(MFX_HANDLE_VA_DISPLAY, va_display)
```

这样 oneVPL session 和 VA display 会绑定到同一个 B570 render node。

## 接口影响

无 public ABI 变化。

业务侧原有调用方式不需要改。默认仍使用：

```text
/dev/dri/renderD129
```

如需显式指定：

```c
MFX50_DeviceConfig device_cfg;
mfx50_device_default_config(&device_cfg);
snprintf(device_cfg.device_path, sizeof(device_cfg.device_path),
         "%s", "/dev/dri/renderD129");
mfx50_device_create(&device_cfg, &device);
```

## 验证

B570 上已验证：

```text
mfx50_split_file_transcode:
decoded_surfaces=252
encoded_packets=252
ffprobe: hevc,1920,1080,252
```

`test_split_codec_headers` 通过。
