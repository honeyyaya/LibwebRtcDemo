# LibwebRtcDemo

基于 **Qt 6** 与 **预编译 WebRTC 静态库** 的 **视频接收端** 示例：通过 **TCP 信令** 建立 P2P，使用 **PeerConnection** 接收远端视频轨道，在 **QML + OpenGL** 上渲染。

> 本仓库为演示与学习用途；**不包含** `libwebrtc.a` 与完整 `include/` 头文件树，需自行准备（见下文）。

---

## 功能概览

- **WebRTC 接收端**：`RTCPeerConnection`、远端视频轨、`VideoSink` 回调。
- **Android**：自定义 **MediaCodec** 硬件解码（H.264 等），NV12 → I420、**I420 内存池**，可选性能日志（McE2E / McPerf / McDE）。
- **渲染**：`QQuickFramebufferObject` + **I420 引用直传纹理**（避免 OnFrame 整帧 memcpy），`GL_UNPACK_ROW_LENGTH`（ES 3+）处理 stride。
- **信令**：`SignalingClient` — **纯 TCP**、**JSON 每行一条**（非浏览器 WebSocket），地址形如 `host:port`。
- **UI**：`main.qml` 连接信令、展示状态与 `WebRTCVideoRenderer`。

---

## 技术栈

| 组件 | 说明 |
|------|------|
| Qt | 6.2+，模块：Quick、WebSockets、Widgets、Network、OpenGL |
| C++ | C++17 |
| WebRTC | 静态链接 `libwebrtc.a`（需与头文件版本一致） |
| Android NDK | MediaCodec / `mediandk`、`GLESv2` |

---

## 仓库结构（主要部分）

```
LibwebRtcDemo/
├── CMakeLists.txt          # 工程与 WebRTC 路径
├── main.cpp / main.qml     # 入口与 QML UI
├── webrtc_receiver_client.*   # PeerConnection、统计定时器（DecodeStats / NetStats）
├── webrtc_factory_helper.*    # PeerConnectionFactory、Android 下挂接 HW 解码工厂
├── signaling_client.*         # TCP JSON-line 信令
├── webrtc_video_renderer.*    # VideoSink、OpenGL I420 渲染
├── video_decode_sink_timing_bridge.*  # Decoded → OnFrame 时延（McDE）
├── android_mediacodec_video_decoder.*  # MediaCodec + I420 池
├── android_hw_video_decoder_factory.*
├── android/                # Android 包配置（如 cleartext、QtActivity）
├── include/                # WebRTC 头文件树（需自备，通常不入库）
├── lib/<ABI>/              # libwebrtc.a（需自备）
└── docs/                   # 搭建、信令序列、接收延迟分段说明等
```

---

## 前置条件

1. **Qt 6**（含 Android 套件时用于交叉编译 APK）。
2. **与库匹配的 WebRTC 构建产物**：
   - 头文件树根目录：含 `api/`、`rtc_base/`、`third_party/` 等（CMake 变量 `WEBRTC_INCLUDE_DIR`，默认 `./include`）。
   - 静态库：`libwebrtc.a`  
     - 桌面：默认 `./lib/libwebrtc.a`  
     - Android：默认 `./lib/${ANDROID_ABI}/libwebrtc.a`
3. **信令服务器**：能按项目约定推送 **每行一个 JSON** 的 SDP/ICE（详见 `docs/webrtc-signaling-sequence.md` 等）。

若缺少 `libwebrtc.a`，CMake 会 **FATAL_ERROR** 并提示路径。

---

## 构建说明

### Android（常见）

1. 将对应 ABI 的 `libwebrtc.a` 放到 `lib/arm64-v8a/`（或 `armeabi-v7a` 等）。
2. 配置好 `include/` 头文件树。
3. 使用 Qt Creator 或 CMake 指定 Android 工具链与 Qt 6，生成 Ninja/Make 后编译。  
4. `QT_ANDROID_PACKAGE_SOURCE_DIR` 指向 `android/`，用于 **明文 HTTP/WebSocket（若需要）** 等清单项。

### 桌面（可选）

- 将 `libwebrtc.a` 放在 `lib/`，并确保 `WEBRTC_WIN` / `WEBRTC_POSIX` 等与平台一致（`CMakeLists.txt` 已按平台加宏）。

### CMake 常用变量

| 变量 | 含义 |
|------|------|
| `WEBRTC_INCLUDE_DIR` | WebRTC 头文件根目录 |
| `WEBRTC_LIB_DIR` | 预置库目录（Android 下含 ABI 子目录） |
| `WEBRTC_STATIC_LIB` | `libwebrtc.a` 绝对路径（可覆盖默认） |

---

## 运行与配置

- **信令地址**：在 `main.qml` 中修改 `signalingUrl`（默认示例为内网 `IP:8765`）。
- **Android**：启动后约 **1.5s** 再允许连接，用于等待 Activity/JNI 就绪（见 QML 中 `connectReadyTimer`）。

---

## 性能与日志（可选）

日志标签示例（logcat / Qt 调试输出）：

| 标记 | 含义 |
|------|------|
| `McE2E` | Decode 入口 → `Decoded` 返回 |
| `McDE` | `Decoded` 返回 → `OnFrame` 入口 |
| `[VideoPerf-GL]` | 纹理上传、GPU 绘制；OnFrame 为 I420 挂接耗时 |
| `[DecodeStats]` / `[NetStats]` | WebRTC `GetStats`：解码、抖动缓冲、RTT 等 |

更完整的阶段划分见 **`docs/video-receive-latency-table-v2.md`**。

---

## 文档索引

| 文档 | 内容 |
|------|------|
| `docs/WebRTC搭建指南.md` | 环境与构建思路 |
| `docs/webrtc-signaling-sequence.md` | 信令交互 |
| `docs/video-receive-latency-table-v2.md` | 接收端延迟分段与日志 |
| `docs/WebRTC 入门连接.md` / `docs/libmediastream设计方案.md` / `docs/WebRTC二阶段移植方案.md` | 扩展设计与笔记 |

---

## 许可证

- 本演示工程代码：请根据你方计划自行添加 **LICENSE**（如 MIT、BSD 等）。
- **WebRTC** 与 **Qt** 分别遵循其官方许可证；预编译库文件通常**不宜**直接提交到公开仓库，建议在 README 中说明获取方式。

---

## 贡献与反馈

Issue / PR 欢迎；若涉及 WebRTC 版本或 ABI，请在说明中注明 **NDK / Qt / libwebrtc 分支或版本**，便于复现。
