# GStreamer Migration Outline

Date: 2026-04-24

## Goal

Replace the current Qt custom renderer path with a GStreamer-based display path while keeping:

- `libRoboFlow` SDK receive logic
- current Qt/QML UI
- C/C++ oriented app architecture

## Current state

- `WebRTCReceiverClient` now sends decoded I420 frames to a generic `VideoFrameSink`.
- `WebRTCVideoRenderer` is the current Qt implementation of `VideoFrameSink`.
- This makes the render path replaceable without rewriting the stream receive logic.

## Recommended next steps

1. Add a new `GStreamerVideoSink` implementation of `VideoFrameSink`.
2. Feed SDK raw frames into `appsrc`.
3. Build a minimal pipeline:
   `appsrc -> queue leaky=downstream max-size-buffers=1 -> glupload -> qml6glsink`
4. Keep the existing Qt renderer as a fallback path for comparison.
5. Compare runtime stats against `docs/current-renderer-latency-baseline.md`.

## Initial comparison targets

- lower `total avg`
- lower `total p95`
- lower `present->sync` equivalent cost
- fewer frame drops / lower `seq_gap_total`
- smaller slow-window count

## Engineering boundary

The first GStreamer milestone should only replace the display path.
Do not change:

- SDK signaling lifecycle
- stream open/close control flow
- app UI control logic
- diagnostics unrelated to rendering
