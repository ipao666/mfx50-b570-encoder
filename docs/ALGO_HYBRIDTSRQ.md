# HybridTSRQ

HybridTSRQ means Hybrid Temporal-Spatial ROI-QP Controller.

The temporal side keeps the v0.5 realtime SDK strategy: I/P/B QP separation,
scene-change IDR decisions, conservative frames after scene changes, adaptive
GOP suggestions, and quality-guard temporal QP deltas.

The spatial side ports the 212-linux ideas into route-local state:

- Y-plane 1/4 downsample analysis.
- Night/foreground/background estimation.
- Background model, foreground mask, morphology open, connected ROI boxes.
- CTU foreground, edge, texture, motion, and importance maps.
- CTU-to-16x16 absolute QP maps for MBQP-style control.
- ROI box fallback for DeltaQP-capable backends.

Strategy selection is layered:

1. `MBQP_CQP`: absolute 16x16 block QP map, CQP rate control.
2. `ROI_DELTA_QP`: frame-level temporal QP plus ROI deltas.
3. `GLOBAL`: temporal QP, scene cut, preprocess policy, and quality guard only.

When external SSIM is unavailable, QualityGuard enters proxy mode. Proxy mode
only estimates risk from motion/noise/edge/QP and must not be reported as true
SSIM acceptance.

## Runtime MBQP Injection

HybridTSRQ now feeds the real oneVPL path through a route-local callback. For
each decoded surface:

1. The old realtime core builds an internal surface view.
2. The new facade finds the matching route-local `HybridTSRQController`.
3. Temporal QP chooses the frame anchor QP.
4. Spatial QP builds an absolute 16x16 `QpMap16x16`.
5. The map is copied into an internal MBQP decision buffer.
6. The old core attaches `mfxExtMBQP` to `mfxEncodeCtrl`.
7. `MFXVideoENCODE_EncodeFrameAsync` receives that ctrl for the frame.

The QP map is absolute QP, not a delta map. The encoder profile still carries
QPI/QPP/QPB for CQP setup, but the MBQP payload itself is the per-block absolute
QP value and must stay in the valid HEVC range 1-51.

Debug patterns are available through `force_mbqp_pattern`:

- `none`
- `flat_low_qp`
- `flat_high_qp`
- `checkerboard`
- `roi_center_low_qp`

These patterns are for proving runtime injection. Final algorithm evaluation
should use `none` so HybridTSRQ supplies the real temporal/spatial decision.
