# 接收端视频延迟分段表（v2，含 D→E）

> 数值请用本机 **logcat / Qt 日志** 实测填写；下表为**含义说明**与**观测方式**，非固定性能承诺。

## 日志关键字

| 标记 | 含义 |
|------|------|
| `McE2E` | **D**：`Decode()` 入口 → `Decoded()` 回调返回（Android MediaCodec 路径，见 `android_mediacodec_video_decoder.cpp`） |
| `McDE` | **D→E**：`Decoded()` 返回 → `WebRTCVideoRenderer::OnFrame` 入口（跨线程，见 `video_decode_sink_timing_bridge`） |
| `[VideoPerf-GL] OnFrame` | **E 之后**：OnFrame 内 YUV 拷贝、帧间隔等 |

## 分段表（v2）

| 符号 | 区间 | 说明 | 线程 / 同步性 |
|------|------|------|----------------|
| （沿用你之前的 A–C 若已定义） | 收包→入解码前 | 依你原有拆分 | 依路径 |
| **D** | Decode 入口 → `Decoded` 返回 | `McE2E`，含入队、worker、MediaCodec、NV12→I420、`cb->Decoded` | 解码 worker，同步块 |
| **D→E** | `Decoded` 返回 → `OnFrame` 入口 | `McDE`，WebRTC 内部投递 + `IncomingVideoStream` 等 | **异步、跨线程** |
| **E 之后** | `OnFrame` 内 → UI/GL | `[VideoPerf-GL]` 拷贝、`synchronize` 纹理上传、`render` | 主线程 / GL 线程 |

## 与 v1 的差异

- v1：`McE2E` 明确 **不含** WebRTC 到应用 sink 的延迟。
- v2：增加 **`McDE`**，把 **Decoded 返回到应用 `OnFrame` 入口** 量化，便于区分「解码器侧」与「框架投递侧」。

## 典型值占位（请替换为实测）

| 指标 | 典型范围（ms） | 尖峰备注 |
|------|----------------|----------|
| McE2E（D） | ___ | 与分辨率、机型、是否 low-latency 相关 |
| McDE（D→E） | ___ | 线程调度、队列深度；通常远小于 McE2E，但偶发尖峰 |
| OnFrame 拷贝 | 见 `[VideoPerf-GL]` | 与分辨率相关 |

## 注意

- **rtp_timestamp** 关联：若同一时刻存在重复 ts、或软解路径未调用 `DecodeSinkRecordAfterDecoded`，则不会出现 `McDE`（无匹配记录）。
- 地图上限 128 条，极端积压时会清空，可能导致个别帧无 `McDE`。
