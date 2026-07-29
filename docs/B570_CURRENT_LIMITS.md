# B570 Current Limits

## Verified strengths

- B570 discrete GPU path is selected through `/dev/dri/renderD129`.
- oneVPL `mfx-gen` path is active.
- 45-route asynchronous hardware transcode runs above 30 fps per route on 1000-frame tests.
- IPB structure, GOP 60, ROI-only protection, and plate ROI are enabled in the 运行示例.
- Y/U/V channel SSIM is measured separately in validation CSV files.

## Open constraint conflict

On the current hard daytime validation set, all four constraints have not been met at the same time:

- B570 single GPU.
- 45 routes at 30 fps.
- average compression around 90%.
- average SSIM above 0.9 and every Y/U/V channel minimum above 0.81.

The main failure mode is Y-channel loss on daytime scenes with road texture, guardrails, tree leaves, sky boundaries, and fine repeated background structure. Raising QP enough to approach 90% compression damages Y SSIM. Lowering QP enough to protect Y SSIM reduces compression far below 90% on those scenes.

Full 45-route q41/q43/q51 validation on B570:

- average compression: `86.122%`
- minimum route fps: `32.410`
- average all-SSIM: `0.895916`
- minimum all-SSIM: `0.791514`
- average Y-SSIM: `0.851979`
- minimum Y-SSIM: `0.699790`
- minimum U-SSIM: `0.963888`
- minimum V-SSIM: `0.978602`

So throughput passes, but compression, average SSIM, minimum all-SSIM, and minimum Y-channel SSIM do not all pass.

Quality-first day/night validation on 45 x 1000 frames:

- day profile: `36/38/44`
- night profile: `47/49/51`
- day average compression: `60.913%`
- day minimum Y-SSIM: `0.812878`
- day average all-SSIM: `0.905795`
- night average compression: `97.064%`
- night minimum Y-SSIM: `0.864618`
- night minimum all-SSIM: `0.902120`

This meets the quality guard and night compression goal on the 1000-frame test, but it does not meet the requested daytime `86%` compression goal.

## Practical profiles

Size-first profile:

- `QPI/QPP/QPB=41/43/51`
- 45-route throughput passes.
- Compression is high.
- Some daytime Y-channel tail cases fail the minimum SSIM guard.

Y-guard profile:

- `QPI/QPP/QPB=36/38/44`
- 45-route throughput passes.
- Y/U/V minimum SSIM passes on the 1000-frame test.
- Compression is substantially lower.

## What did not solve the conflict

- Full-frame MBQP reuse did not beat lower global QP on hard Y-channel scenes.
- Static skip did not materially help high-motion daytime tail scenes.
- TransformSkip had no meaningful effect in the tested B570 path.
- Main10 output was unsupported with the current NV12 sample path.
- `ScenarioInfo`, `ContentInfo`, and `AdaptiveCQM` had no measurable effect in the tested path.

## Commercial recommendation

Use size-first profile for storage-constrained deployments that tolerate lower Y-channel SSIM on difficult daytime backgrounds.

Use Y-guard profile where visual fidelity and channel minimum SSIM are contract requirements.

If the product contract requires all current constraints simultaneously, one of the constraints should be relaxed or the codec/hardware path should change.
