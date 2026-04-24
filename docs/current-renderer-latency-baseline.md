# Current Qt Renderer Latency Baseline

Date: 2026-04-24
Platform: Android / Qt Quick + `QQuickFramebufferObject` + raw I420 upload
Input path: `libRoboFlow SDK -> on_video -> QByteArray -> Qt renderer`

## Purpose

This file records the current latency baseline before replacing the renderer with a GStreamer-based path.
Subsequent renderer experiments should compare against this baseline.

## Current conclusions

- The decode path is not the main bottleneck.
- The dominant cost is still `present->sync`, which maps to Qt render scheduling.
- `sdk->ui` has visible jitter and occasionally rises to `10-13 ms`.
- `ui->present` is small and stable.
- `sync->render` is not free, but it is not the primary bottleneck.
- The current pipeline already drops old frames under load to protect latency.

## Stage breakdown

### `sdk->ui`

- Typical: `0.1-2 ms`
- Jitter cases: `10-13 ms`

### `ui->present`

- Typical: `0.4-0.8 ms`
- Not a bottleneck.

### `present->sync`

- Typical: `11-16 ms`
- P95 often: `15-16 ms`
- Peak observed: `22.209 ms`
- This is the dominant steady-state cost.

### `sync->render`

- Typical: `1.5-4 ms`
- GL upload and draw cost exists but is not the main issue.

### `total sdk->render`

- Best frames: `6-10 ms`
- Typical range: `14-20 ms`
- Slow frames: `30-32 ms`

## Summary samples from runtime logs

### Report 1

```text
[LatencySummary] report=1 window=120 rendered_total=120 slow_total=12 seq_gap_total=7 slow_window=12 |
sdk->ui(avg=2.546 p95=9.617 max=11.385 ms) |
ui->present(avg=0.542 p95=0.849 max=1.503 ms) |
present->sync(avg=11.841 p95=15.803 max=18.189 ms) |
sync->render(avg=2.474 p95=4.115 max=5.107 ms) |
total(avg=17.404 p95=27.348 max=30.572 ms)
```

### Report 2

```text
[LatencySummary] report=2 window=180 rendered_total=240 slow_total=16 seq_gap_total=8 slow_window=11 |
sdk->ui(avg=2.462 p95=7.977 max=10.452 ms) |
ui->present(avg=0.489 p95=0.691 max=1.794 ms) |
present->sync(avg=13.867 p95=16.437 max=18.189 ms) |
sync->render(avg=1.942 p95=3.216 max=4.269 ms) |
total(avg=18.760 p95=25.198 max=30.572 ms)
```

### Report 3

```text
[LatencySummary] report=3 window=180 rendered_total=360 slow_total=72 seq_gap_total=9 slow_window=59 |
sdk->ui(avg=6.600 p95=11.249 max=13.769 ms) |
ui->present(avg=0.498 p95=0.687 max=1.467 ms) |
present->sync(avg=14.397 p95=16.813 max=20.310 ms) |
sync->render(avg=2.272 p95=3.697 max=4.269 ms) |
total(avg=23.766 p95=29.390 max=32.767 ms)
```

### Report 4

```text
[LatencySummary] report=4 window=180 rendered_total=480 slow_total=143 seq_gap_total=18 slow_window=113 |
sdk->ui(avg=7.607 p95=11.643 max=13.769 ms) |
ui->present(avg=0.516 p95=0.780 max=1.467 ms) |
present->sync(avg=13.314 p95=16.248 max=20.244 ms) |
sync->render(avg=2.320 p95=3.841 max=4.557 ms) |
total(avg=23.757 p95=30.082 max=32.767 ms)
```

### Report 5

```text
[LatencySummary] report=5 window=180 rendered_total=600 slow_total=185 seq_gap_total=26 slow_window=89 |
sdk->ui(avg=5.357 p95=11.765 max=13.362 ms) |
ui->present(avg=0.492 p95=0.745 max=1.195 ms) |
present->sync(avg=12.541 p95=16.097 max=17.023 ms) |
sync->render(avg=1.983 p95=3.450 max=4.557 ms) |
total(avg=20.372 p95=29.444 max=31.793 ms)
```

### Report 6

```text
[LatencySummary] report=6 window=180 rendered_total=720 slow_total=196 seq_gap_total=33 slow_window=31 |
sdk->ui(avg=2.232 p95=11.411 max=12.890 ms) |
ui->present(avg=0.485 p95=0.705 max=1.001 ms) |
present->sync(avg=12.232 p95=15.711 max=22.209 ms) |
sync->render(avg=2.053 p95=3.437 max=5.212 ms) |
total(avg=17.003 p95=28.955 max=31.920 ms)
```

## Comparison metrics for future renderer tests

- `total avg`
- `total p95`
- `present->sync avg`
- `present->sync p95`
- `sdk->ui p95`
- `seq_gap_total`
- slow frame count in the current window

## Baseline verdict

- Expected current average total latency: about `17-20 ms`
- Expected current P95 total latency: about `29-31 ms`
- Expected current peak total latency: about `31-32 ms`
- Main bottleneck: Qt render scheduling, not decode
- Main migration goal: reduce `present->sync` and improve latency tail behavior
