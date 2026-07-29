# Backend Guide

Backends are selected through `MFX50RT_BackendConfig.type` or JSON
`backend.type`.

Supported public values:

- `auto`
- `onevpl`
- `ffmpeg`
- `vaapi`
- `nvenc`
- `cpu`
- `null`

`MFX50RT_QueryCapabilities` is intentionally conservative. The oneVPL facade
reports codec and rate-control basics, but does not advertise MBQP or ROI until
a runtime probe verifies those extension buffers on the selected hardware and
driver. This avoids hardcoding MBQP support.

Fallback rules:

- `AUTO`: MBQP -> ROI DeltaQP -> GLOBAL.
- Requested `MBQP_CQP`: fallback to ROI if verified, otherwise GLOBAL.
- Requested `ROI_DELTA_QP`: fallback to GLOBAL if ROI is not verified.

Read `MFX50RT_EffectiveConfig.fallback_reason` after Create to see why fallback
happened.

## oneVPL MBQP Runtime Control

As of the MBQP continuation on 2026-05-31, `MFX50RT_BACKEND_ONEVPL` does more
than route packets into the old realtime core. The old core exposes a private
lower-case internal hook, loaded by the new facade through `dlopen`, so each
decoded surface can request per-frame encode control before
`MFXVideoENCODE_EncodeFrameAsync`.

The public strategy fields have two different meanings:

- `effective_strategy`: the SDK selected `MBQP_CQP`, `ROI_DELTA_QP`, or
  `GLOBAL` after capability probing and fallback.
- `actual_encode_control`: the control actually attached at runtime for the
  encoded frames, such as `MBQP` or `GLOBAL`.

For MBQP, encoder initialization sets
`mfxExtCodingOption3::EnableMBQP = MFX_CODINGOPTION_ON` with CQP rate control.
At runtime the old core attaches `mfxExtMBQP` through `mfxEncodeCtrl` for each
eligible frame. The QP buffer is kept in a per-route pending encode-control slot
until the async encode sync operation completes.

ROI remains a fallback interface only. Current hardware probing reports
`roi=0`, so the backend does not attach `mfxExtEncoderROI` by default.
