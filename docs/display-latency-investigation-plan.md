# 60ms Display Latency Investigation Plan

目标：定位“同一台服务器、同一路 WebRTC 流，自定义 Qt 渲染比谷歌内嵌 web 渲染慢约 60ms”的主要开销位置。

## 当前怀疑点

基于现有代码，自定义渲染链主要经过以下阶段：

1. SDK `onVideo` 回调线程收到解码后 I420 帧
2. `QueuedConnection` 切到 Qt 线程
3. Qt 线程拷贝 `QByteArray`
4. `WebRTCVideoRenderer::presentFrame()`
5. `QQuickFramebufferObject::synchronize()`
6. `QQuickFramebufferObject::render()`

其中第 2 到第 6 段，最容易累计出 3 到 4 帧的显示排队。

## 已加入的定位日志

本工程已经加入逐帧时间线日志，tag 为：

`[LatencyTrace]`

日志覆盖以下 5 个关键点：

1. `SDK callback`
2. `UI dispatch`
3. `presentFrame`
4. `synchronize`
5. `render`

输出字段包括：

- `frame`
- `pts_ms`
- `utc_ms`
- `size`
- `sdk->ui`
- `ui->present`
- `present->sync`
- `sync->render`
- `total sdk->render`

## 代码位置

### 1. SDK 回调入口

文件：

`webrtc_receiver_client.cpp`

函数：

`WebRTCReceiverClient::onVideoFrameThunk`

作用：

- 记录 SDK 回调进入时间
- 记录 frame id / pts / utc / 分辨率 / 数据大小

### 2. Qt 主线程处理入口

文件：

`webrtc_receiver_client.cpp`

函数：

`WebRTCReceiverClient::deliverPendingFrame`

作用：

- 记录从 SDK 回调切到 Qt 主线程的耗时

### 3. QML Item 入队

文件：

`webrtc_video_renderer.cpp`

函数：

`WebRTCVideoRenderer::presentFrame`

作用：

- 记录 UI 线程将帧挂入渲染器的时刻

### 4. Render thread 取帧

文件：

`webrtc_video_renderer.cpp`

函数：

`WebRTCGLRendererImpl::synchronize`

作用：

- 记录 Scene Graph render thread 实际取到该帧的时刻

### 5. OpenGL 绘制结束

文件：

`webrtc_video_renderer.cpp`

函数：

`WebRTCGLRendererImpl::render`

作用：

- 输出当前帧完整的自定义渲染时间线

## 如何看日志

重点看以下几段：

- `sdk->ui`
  说明 SDK 回调线程到 Qt 主线程的排队成本

- `ui->present`
  说明主线程内部处理与拷贝成本

- `present->sync`
  说明 `update()` 到 Scene Graph 下一帧 `synchronize()` 的等待成本

- `sync->render`
  说明 OpenGL 上传与绘制成本

- `total sdk->render`
  说明自定义显示链总开销

## 如何和 web 渲染对比

推荐使用同一套 frame identity：

1. 优先使用发送端附带的业务帧号
2. 如果没有，至少对齐 `pts_ms` / `utc_ms`
3. 保证 web 和自定义渲染都打印同一路流的同一帧标识

对比方式：

| 对比项 | 自定义渲染 | 内嵌 web |
|---|---|---|
| 解码后回调时刻 | 取 `SDK callback` | 取 web 侧解码回调或 video sink 回调 |
| 实际开始渲染时刻 | 取 `render` | 取 web 侧 compositor / render 日志 |
| 总显示链耗时 | `total sdk->render` | web 对应 `decode/output -> render` |

如果两边解码输出很接近，但自定义渲染在 `present->sync` 或 `sync->render` 明显偏大，就能基本坐实瓶颈在 Qt 显示链。

## 典型结论模板

### 场景 A：`sdk->ui` 偏大

说明：

- 主问题在线程切换或 Qt 主线程繁忙

优化方向：

- 减少 `QueuedConnection` 中转
- 降低主线程工作量
- 改用更直接的 render thread / texture 路径

### 场景 B：`ui->present` 偏大

说明：

- 主问题在 CPU 拷贝或主线程内帧处理

优化方向：

- 去掉 `QByteArray` 二次拷贝
- 改固定帧池 / ring buffer

### 场景 C：`present->sync` 偏大

说明：

- 主问题在 `QQuickFramebufferObject` 与 Qt Scene Graph 帧调度

优化方向：

- 评估替换 `QQuickFramebufferObject`
- 评估 `GStreamer + qml6glsink`
- 评估更贴近 texture node / native video surface 的路径

### 场景 D：`sync->render` 偏大

说明：

- 主问题在 I420 三平面上传和绘制

优化方向：

- 减少纹理重建
- 评估 PBO / 更优上传路径
- 评估 NV12 / 外部纹理 / 零拷贝路径

## 建议的下一步实验

### 实验 1：先跑现有链路

目标：

- 量化 60ms 主要落在哪一段

### 实验 2：关闭部分 UI 干扰

目标：

- 排除 QML 叠层和主线程控件刷新影响

方法：

- 暂时隐藏界面上的状态文本与覆盖层

### 实验 3：做最小 GStreamer 验证

目标：

- 比较 `appsrc -> glupload -> qml6glsink` 与当前自定义渲染的显示链差异

验收指标：

- 首帧时间
- 稳态平均延迟
- 最差帧延迟
- 抖动

