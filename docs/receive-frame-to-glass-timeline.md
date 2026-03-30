# 一帧从 RTP 进机到像素上屏：时间轴说明

本文梳理接收端视频路径：**RTP → 抖动缓冲 → 解码队列 → McE2E（worker 同步块）→ McDE（跨线程延迟）→ OnFrame → GL 上传与绘制**，并标注**线程**、**同步/异步**，以及与常见**日志标签**的对应关系。

---

## 总览：一条时间轴（从左到右）

```
[RTP 收包] → [抖动缓冲 JB] → [送解码/入队] → [解码 worker：McE2E 整段]
       ↑ 异步流水线（通常不在 UI/GL 线程）↑
                    ↓
         [Decoded 回调返回] ——McDE——→ [OnFrame] ——→ [GL 上传 + 绘制] ——→ [SurfaceFlinger/合成上屏]
                    ↑ 跨线程排队/调度 ↑      ↑ 渲染线程，同步 ↑
```

---

## 分段说明

### 1. RTP → 抖动缓冲（JB）

| 项目 | 说明 |
|------|------|
| **做什么** | 收 UDP/RTP、排序、等迟到包、按时间做播放调度。 |
| **线程** | 一般是**网络/工作线程**（不是 GL）。 |
| **相对 UI** | **异步**；卡在这里表现为**端到端延迟变大**，但通常**不会直接卡死 GL 线程**。 |
| **日志** | `NetStats`（RTT/抖动/丢包）反映网络侧；`DecodeStats` 中的「抖动缓冲 xx ms」与 JB 目标延迟更贴近。 |

---

### 2. JB → 送解码 → 解码队列

| 项目 | 说明 |
|------|------|
| **做什么** | 组完一帧可解码数据后，交给 MediaCodec 路径（**Decode ingress** 即「进解码入口」）。 |
| **线程** | 从 WebRTC 到自定义适配器，往往是**投递到解码专用线程/队列**。 |
| **性质** | **异步**（相对 RTP 线程）；在**解码 worker** 上会变成**排队**（`tasks_` 与 `WorkerLoop`）。 |
| **日志** | WebRTC→worker 的排队**不**单独打在 McPerf 里；**McPerf** 的 **`q_in`** 见下文「同步大头」专节——实为 **`queueInputBuffer` 调用**耗时。 |

---

### 3. McE2E 整段（解码入口 → Decoded 返回）

| 项目 | 说明 |
|------|------|
| **包含** | 入队等待 + worker 上整段 + **MediaCodec** + **NV12→I420** + **Decoded 回调返回**。 |
| **不包含** | WebRTC 再往后走到 sink；不包含 GL。 |
| **线程** | 主要在**解码 worker**（及 MediaCodec 内部线程/硬件）。 |
| **性质** | 对 RTP 收包线程是**异步**；对 **worker 自身**是一大段**同步连续工作** + 在 `dequeue` 上**阻塞等待**。 |
| **日志** | **`McE2E`** = 整段墙钟；**`McPerf`** 拆成 `prep`、`deq_in`、`memcpy_in`、`q_in`（`queueInputBuffer`）、`drain` 等；**`deq`（出队输出）、`getbuf`、`nv12_i420`** 均为 worker 上的**同步耗时**（见文末专节）。 |

---

### 4. McDE（Decoded 返回 → OnFrame 入口）

| 项目 | 说明 |
|------|------|
| **做什么** | 解码产出 I420（或等价 buffer）后，WebRTC 将 **VideoFrame** 交给 **sink**（如 `VideoRenderer`），直到 **`OnFrame` 被调用**。 |
| **线程** | **跨线程**——从解码回调线程到 sink/渲染相关线程的 **post / 队列 / 锁**。 |
| **性质** | 解码回调里通常很快返回（**异步交接**）；从「帧已解码」到「渲染侧拿到帧」之间的墙钟间隔即 **McDE**，偏大时多为**调度/排队**。 |
| **日志** | **`VideoPerf` McDE** 一行。 |

---

### 5. OnFrame → GL 纹理上传 + 绘制

| 项目 | 说明 |
|------|------|
| **做什么** | 将 I420（或纹理）接到 GL，**上传/采样**，**draw**，**swap/present** 到 Surface。 |
| **线程** | **GL/渲染线程**（常见为单线程串行）。 |
| **性质** | **同步**——本帧上传 + draw 未完成，同一线程上的后续逻辑需等待。 |
| **日志** | **`VideoPerf-GL`** 的纹理上传、GPU 渲染；**OnFrame#… 投递** 多为锁内挂接，通常极短。 |

---

### 6. GL 之后到「真正上屏」

| 项目 | 说明 |
|------|------|
| **做什么** | BufferQueue → **SurfaceFlinger** 合成 → 显示刷新。 |
| **线程** | 系统合成进程，与 App **异步**。 |
| **日志** | 常见业务 log **不含**这一段；若需「真上屏」可结合 **Systrace**、**GPU 呈现时间** 等。 |

---

## 一句话串联

**网络与 JB 在专用线程上异步往前推；解码在 worker 上排队 + 硬等 MediaCodec + CPU 转格式（McE2E / McPerf）；解码完成后经 WebRTC 投递到 sink（McDE）；最后在 GL 线程同步做上传和绘制；再交给系统异步合成上屏。**

---

## 可选：App 侧粗粒度 E2E（不含 SF）

可将 **McE2E + McDE +（纹理上传 + GPU）** 加总，作为「接收端 App 侧到画完一帧」的粗粒度指标，再与 **DecodeStats 抖动缓冲** 对比，判断延迟主要落在 **JB** 还是 **解码 / 调度 / GL**。

---

## 同步大头细拆：McPerf（解码 worker）与 VideoPerf-GL（GL 线程）

下面与 `android_mediacodec_video_decoder.cpp`、`webrtc_video_renderer.cpp` 中的计时一一对应。

### McPerf：`ProcessOneFrame` + 两轮 `DrainOutputs`

| 字段 | 代码含义 | 为何常成瓶颈 |
|------|----------|----------------|
| **deq_in** | `AMediaCodec_dequeueInputBuffer` 从调用到返回的墙钟时间 | 在等 Codec 侧**空出 input 槽位**；解码/出帧慢、管道背压时，这里会**阻塞拉长**。 |
| **q_in** | `AMediaCodec_queueInputBuffer` **单次调用**耗时（memcpy 之后、真正把 AU 交给解码器） | 不是 `tasks_` 排队时间；反映 **Binder/Codec2 入队**等系统路径。异常大时多为驱动或系统负载。 |
| **memcpy_in** | 压缩码流拷入 input buffer | 与 `feed_sz`、总线/内存带宽相关。 |
| **drain0 / drain1 的 deq** | 循环里 `AMediaCodec_dequeueOutputBuffer` 累计时间 | 首轮多为 **timeout=0** 轮询；若首轮未出帧，第二轮可带**短超时**（见 `kDrainAfterQueueShortWaitUs`），**deq** 里会包含「等硬件产出」的阻塞。 |
| **getbuf** | `AMediaCodec_getOutputBuffer` | 一般较小；偶发映射/锁竞争时会翘尾。 |
| **nv12_i420** | `FillI420FromNv12` / `FillI420FromNv12Tight`：从 MediaCodec **NV12 族**输出拷到 **I420**（池或新建 buffer） | **纯 CPU** 色彩平面重排 + 拷贝，随分辨率、stride、`slice_height` 线性涨；是 McE2E 里最常见的 CPU 大头之一。 |
| **decoded_cb** | `cb->Decoded(frame)` 回调链 | WebRTC 内部拷贝/投递；过大时需结合 Trace 看是否适配器过重。 |
| **rel** | `releaseOutputBuffer` | 归还输出 buffer；通常不大。 |

**读日志时注意**：`drain0` / `drain1` 两轮可能各出多帧（`out` 计数），**deq / nv12_i420 是多帧累加**；要和单帧体感对齐，需看是否每轮只出一帧或拆均摊。

### VideoPerf-GL：`synchronize` 与 `render`

实现上（Qt `QQuickFramebufferObject`）：

- **纹理上传**（日志里的「纹理上传」ms）：在 **`synchronize()`** 里计时——`takeFrame` 取到 I420 后，对 **Y / U / V** 各调用 `UploadLuminancePlane`（`glTexImage2D`，stride≠width 时用 `GL_UNPACK_ROW_LENGTH` 或逐行 `glTexSubImage2D`）。这是 **CPU 经 GL 驱动把 I420 推进 GPU 纹理** 的同步段，带宽与驱动实现强相关。
- **GPU 渲染**（日志里的「GPU渲染」ms）：在 **`render()`** 里从清屏、绑 shader、采样三纹理、`glDrawArrays` 到 `release()` 的墙钟时间——主要是 **片元阶段 YUV→RGB**（shader 内矩阵）+ 光栅化；不含当帧上传（上传已在上一次 `synchronize` 完成）。

**为何也算同步大头**：与解码 worker 一样，**Scene Graph 的 GL 线程**上 `synchronize` → `render` 串行；上传慢会挤占同一帧内后续 draw 的预算，并推高帧间隔。

### 优化时怎么拆问题

1. **McPerf 里 deq（输出）远大于 nv12_i420**：偏 **硬件解码/流水线** 或 **dequeue 等待**；查分辨率、低延迟模式、是否 Codec2 告警。
2. **nv12_i420 独大**：偏 **CPU 转 I420**；可考虑能直接消费 NV12/OES 的渲染路径（改动大）或降低分辨率。
3. **deq_in 独大**：**input 背压**；解码器来不及吃码流，常与输出侧堆积相关。
4. **VideoPerf-GL 纹理上传独大**：**CPU→GPU 带宽**或 `glTexImage2D` 全量上传策略；可评估 PBO、半宽平面、或避免每帧全尺寸 `TexImage`（仅在有测量佐证时改）。
5. **GPU 渲染独大**：片元负载、FBO 尺寸、过draw；Profiler 看 GPU 时间线。
