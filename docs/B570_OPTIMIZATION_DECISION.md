# B570 Optimization Decision Notes

## Root cause

The hard daytime videos fail mainly on luma detail, not chroma. U/V SSIM stays high even in aggressive profiles, while Y SSIM collapses on road texture, guardrails, lane markings, leaves, shadows, and fine high-frequency background.

This creates a conflict:

- To approach 90% compression, QP must be high enough to remove many Y-channel details.
- To keep every Y/U/V channel above the minimum SSIM guard, Y-channel background detail cannot be quantized that hard.

The conflict is visible even before full-run tuning:

- Size-first profile keeps 45-route throughput and high compression, but Y minimum fails.
- Y-guard profile keeps Y/U/V minimums above guard, but average compression falls far below 90%.

## Why ROI alone cannot fully solve it

Traffic ROI can protect vehicles, plates, motion targets, and lane/corridor regions. It helps avoid wasting low QP on obviously irrelevant areas.

However, the acceptance metric is full-frame SSIM. Full-frame SSIM still penalizes damage to unimportant but high-area background. In daytime traffic videos, road surface and roadside texture occupy much more area than vehicles/plates. If those areas are pushed too high in QP, Y SSIM drops even when vehicles are better protected.

So ROI improves perceptual usefulness, but it does not make full-frame Y SSIM ignore trees, road texture, or sky boundaries.

## Adaptive upper-bound check

On the 45-route 1000-frame validation set, 19 routes fall below `ssim_y=0.81` under the size-first `41/43/51` profile. Those routes need approximately the Y-guard `36/38/44` range to pass the channel guard.

If those 19 hard routes use Y-guard and the other 26 routes stay size-first, the average compression is about `75.62%`.

Even with an impossible upper bound where all 26 easy routes compress to `100%`, the overall average only reaches about `80.09%`.

This means route-level adaptive selection alone cannot recover a `90%` average while also keeping all hard Y-channel routes above the minimum guard on this validation set.

## Methods tested

- oneVPL true asynchronous multi-session pipeline: restored and verified above 45 x 30 fps on 1000-frame tests.
- IPB structure / B frames: enabled through `DIST=4`.
- Per-frame I/P/B QP separation: enabled with separate `QPI/QPP/QPB`.
- Traffic ROI-only delta QP: enabled for throughput.
- Plate ROI expansion and stronger plate delta QP: enabled.
- Full-frame MBQP reuse: tested, did not beat lower global QP for hard Y-channel scenes.
- Static-frame reuse / skip: implemented, but high-motion daytime tail scenes did not benefit materially.
- TransformSkip: tested, no meaningful effect in the B570 path.
- Main10 HEVC output: unsupported with the current NV12 sample path.
- Scenario/content/adaptive CQM flags: no measurable effect in tested B570 path.

## Current recommended operating modes

Size-first:

```text
QPI/QPP/QPB = 41/43/51
```

Use when storage pressure is dominant and occasional Y-channel tail degradation is acceptable.

Y-guard:

```text
QPI/QPP/QPB = 36/38/44
```

Use when Y/U/V minimum SSIM is a hard contractual requirement.

## What would count as a real next breakthrough

The likely next breakthrough is not another small QP constant. It would require changing one of these:

- the metric, for example using traffic-object-weighted quality instead of full-frame SSIM;
- the codec path, for example AV1/HEVC slower preset/higher quality mode if throughput allows;
- the hardware budget, for example more GPU capacity;
- the content path, for example accepting frame-rate adaptation on truly static periods;
- a stronger detector, for example a lightweight vehicle/plate/lane model, with proven GPU/CPU budget at 45 routes.

Under the current constraints, no tested H.265 B570 profile met 90% compression, 45 x 30 fps, average SSIM above 0.9, and every Y/U/V minimum above 0.81 at the same time.

## Revised day/night target check

The revised target was:

- SSIM/YUV must pass;
- daytime compression should not be far below 90%, target at least 86%;
- nighttime compression should be above 95%.

The night side is achievable. A `47/49/51` night profile reached `97.064%` average compression on the 16 night routes in the 45-route 1000-frame validation, with minimum Y-SSIM `0.864618` and minimum all-SSIM `0.902120`.

The day side is still not achievable on the hard daytime H.264 inputs while keeping the quality guard. The quality-first day profile `36/38/44` reached minimum Y-SSIM `0.812878`, but daytime average compression was only `60.913%`. More aggressive daytime QP values raise compression but immediately push the worst Y-channel SSIM below the guard.

So the practical profile is now:

- day: quality guard profile `36/38/44`;
- night: high-compression profile `47/49/51`.

This is a useful production profile when SSIM is mandatory, but it is not a solution to the daytime `86%` compression target.
