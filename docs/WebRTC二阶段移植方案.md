# WebRTC 二阶段方案：libdatachannel + FFmpeg 纯 C++ 移植

> 日期：2026-02-28
> 前置条件：一阶段（QWebEngine 方案）已验证通过，端到端延迟约 130ms
> 目标：去除 Chromium 引擎依赖，用 libdatachannel + FFmpeg 实现纯 C++/QML 接收端，并逐步加入 RTP 层优化

---

## 一、为什么要移植

| 维度 | 一阶段（QWebEngine） | 二阶段（libdatachannel + FFmpeg） |
|------|---------------------|----------------------------------|
| 包体积 | ~200MB（含 Chromium） | ~15-20MB |
| 安卓兼容 | Qt WebEngine 不支持安卓 | 纯 C++ 跨平台，安卓完全支持 |
| 延迟可控性 | 黑盒（浏览器内核控制） | 全链路可调（Jitter Buffer、解码策略） |
| 定制能力 | 受限于 Web API | 完全掌控 RTP/RTCP 层 |
| 资源占用 | 高（Chromium 进程） | 低（仅 FFmpeg + 轻量网络库） |

---

## 二、整体架构

```
电脑A (发送端，不变)                         电脑B (接收端，重写)
┌───────────────────────────┐              ┌─────────────────────────────────────┐
│                           │              │  Qt/QML 应用                        │
│  ① 信令服务器 (Node.js)   │◄──Socket.io──►  ② 信令客户端 (C++ WebSocket)      │
│     localhost:3000        │    信令        │                                    │
│                           │              │  ③ libdatachannel                   │
│  ③ Python aiortc 发送端   │──WebRTC P2P──►    ├─ DTLS/ICE/SRTP                 │
│     采集摄像头            │   UDP 直连    │    └─ RTP 解包                      │
│     编码 H.264            │  视频数据     │                                    │
│                           │              │  ④ RTP 处理层 (自研)                │
│                           │              │    ├─ Jitter Buffer (简版)          │
│                           │              │    ├─ NACK 重传请求                 │
│                           │              │    ├─ PLI/FIR 关键帧请求            │
│                           │              │    ├─ FEC 解码 (RFC 5109)           │
│                           │              │    └─ GCC 拥塞反馈 (REMB/TWCC)     │
│                           │              │                                    │
│                           │              │  ⑤ FFmpeg 解码器                    │
│                           │              │    └─ H.264 → YUV/NV12             │
│                           │              │                                    │
│                           │              │  ⑥ QML 渲染                        │
│                           │              │    └─ OpenGL/D3D 纹理上屏           │
└───────────────────────────┘              └─────────────────────────────────────┘
```

### 数据流

```
RTP 包 (SRTP) → libdatachannel 解密 → RTP 解包
    → Jitter Buffer 排序/缓冲
    → NACK 检测丢包 → 发送 RTCP NACK
    → FEC 恢复丢失包
    → H.264 NAL 重组 (FU-A / STAP-A)
    → FFmpeg avcodec_send_packet / avcodec_receive_frame
    → YUV420P / NV12 帧
    → QML VideoOutput (OpenGL 纹理)
```

---

## 三、技术选型

| 组件 | 选型 | 版本 | 理由 |
|------|------|------|------|
| WebRTC 协议栈 | libdatachannel | 0.21+ | 轻量 C++17，DTLS/ICE/SRTP 内置，支持媒体传输 |
| 视频解码 | FFmpeg (libavcodec) | 6.x / 7.x | 成熟稳定，H.264/H.265 全覆盖，硬件加速支持 |
| 信令通信 | IXWebSocket 或 QWebSocket | — | 纯 C++ WebSocket 客户端，替代 socket.io 客户端 |
| JSON 解析 | nlohmann/json | 3.x | 头文件 only，方便集成 |
| UI 框架 | Qt 5.14.2 QML | 5.14.2 | 项目已有基础设施 |
| 构建系统 | CMake | 3.16+ | 跨平台，libdatachannel 原生 CMake 支持 |

### 信令协议适配说明

一阶段使用 Socket.IO（基于 Engine.IO），这不是标准 WebSocket。二阶段有两种路径：

- **方案 A**：信令服务器新增一个原生 WebSocket 端点（如 `/ws`），与 Socket.IO 共存，C++ 端连接 `/ws`
- **方案 B**：C++ 端集成 Socket.IO C++ 客户端库（较重，不推荐）

**推荐方案 A**，改动最小，信令服务器加约 30 行代码。

---

## 四、开发分步计划

### Phase 2.1：最小可用 — 基础视频通路（目标：能看到画面）

**预计时间：3~5 天**

| 步骤 | 任务 | 说明 |
|------|------|------|
| 2.1.1 | 搭建 CMake 工程骨架 | Qt5 + libdatachannel + FFmpeg 依赖引入 |
| 2.1.2 | 信令服务器添加 WebSocket 端点 | `server.js` 新增 `/ws` 路径，转发 offer/answer/ice |
| 2.1.3 | C++ 信令客户端 | WebSocket 连接，JSON 收发 offer/answer/ICE candidate |
| 2.1.4 | libdatachannel PeerConnection | 配置 ICE/DTLS，处理 onTrack 回调，获取 RTP 包 |
| 2.1.5 | RTP 解包 → H.264 NAL 重组 | 处理 FU-A 分片、STAP-A 聚合，拼装完整 NAL Unit |
| 2.1.6 | FFmpeg 解码 | avcodec H.264 解码器，packet → frame (YUV420P) |
| 2.1.7 | QML 渲染 | QQuickImageProvider 或 QSGTexture，YUV → 显示 |

**2.1 里程碑验收标准：**
- 电脑 B 运行纯 C++ Qt 程序，能接收并显示电脑 A 的摄像头画面
- 无需任何优化，允许偶尔花屏 / 丢帧

---

### Phase 2.2：Jitter Buffer（简版）

**预计时间：2~3 天**

#### 设计思路

简版 Jitter Buffer 不追求 WebRTC 标准实现（NetEQ 级别），只做核心功能：

```
目标：
1. 按 RTP 序列号排序乱序包
2. 按时间戳 (RTP timestamp) 组帧
3. 提供固定/自适应延迟缓冲，平滑网络抖动

数据结构：
┌─────────────────────────────────────────┐
│  PacketBuffer (环形缓冲区，按 seq 排序)  │
│  ┌───┬───┬───┬───┬───┬───┬───┬───┐     │
│  │ S │S+1│S+2│ ? │S+4│S+5│ ? │S+7│     │ ? = 空洞（丢包）
│  └───┴───┴───┴───┴───┴───┴───┴───┘     │
│                                         │
│  FrameAssembler                         │
│  - 相同 timestamp 的包 → 一帧           │
│  - 检测帧完整性 (marker bit + 序列连续)  │
│  - 完整帧 → 送入解码队列               │
│                                         │
│  PlayoutTimer                           │
│  - 目标缓冲: 30~80ms (可配)            │
│  - 自适应: 根据抖动统计动态调整         │
└─────────────────────────────────────────┘
```

#### 核心接口

```cpp
class JitterBuffer {
public:
    struct Config {
        uint32_t min_delay_ms = 30;
        uint32_t max_delay_ms = 150;
        uint32_t initial_delay_ms = 50;
        uint16_t max_packet_count = 512;
    };

    void insertPacket(RtpPacket packet);

    // 返回一个完整的、可解码的帧（按解码顺序）
    // 如果当前没有就绪帧，返回 nullopt
    std::optional<VideoFrame> popFrame();

    struct Stats {
        uint32_t current_delay_ms;
        uint32_t packets_received;
        uint32_t packets_lost;
        uint32_t frames_decoded;
        uint32_t frames_dropped;
        float jitter_ms;
    };
    Stats getStats() const;
};
```

**2.2 验收标准：**
- 在模拟 20ms 抖动的网络环境下（用 clumsy 或 tc），画面平滑无跳帧
- 端到端延迟 < 150ms（含缓冲）

---

### Phase 2.3：NACK 重传请求

**预计时间：1~2 天**

#### 设计思路

```
检测丢包时机：Jitter Buffer 发现序列号空洞
    ↓
生成 RTCP NACK (RFC 4585)
    ↓
通过 libdatachannel RTCP 通道发送
    ↓
发送端收到 NACK → 重传对应 RTP 包
    ↓
Jitter Buffer 收到重传包 → 补洞
```

#### 关键参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| nack_delay_ms | 20 | 检测到丢包后等待多久发 NACK（避免乱序误判） |
| max_nack_retries | 3 | 同一个包最多请求重传几次 |
| nack_rtt_multiplier | 1.5 | 重传超时 = RTT × multiplier |
| max_nack_list_size | 100 | 单次 NACK 最多携带多少个序列号 |

#### RTCP NACK 包格式 (RFC 4585)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|  FMT=1  |    PT=205     |          length               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  SSRC of packet sender                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  SSRC of media source                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           PID (丢失包seq)      |         BLP (位图)           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

**2.3 验收标准：**
- 用 clumsy 模拟 5% 随机丢包，画面无明显花屏
- 日志可见 NACK 发送和重传包接收

---

### Phase 2.4：PLI / FIR 关键帧请求

**预计时间：1 天**

#### 触发条件

| 场景 | 请求类型 | 说明 |
|------|---------|------|
| 首次连接 / 切换视频源 | PLI | 请求发送端立即生成 IDR 帧 |
| 解码器连续报错 | PLI | 参考帧丢失导致无法解码 |
| Jitter Buffer 检测到关键帧丢失 | FIR | 强制生成新的关键帧序列 |
| 长时间未收到 IDR | PLI | 超过 N 秒未收到关键帧 |

#### RTCP PLI (RFC 4585, FMT=1, PT=206)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P| FMT=1   |    PT=206     |          length=2             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  SSRC of sender                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  SSRC of media                                |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

#### 核心接口

```cpp
class KeyframeRequester {
public:
    void requestPLI();
    void requestFIR();
    void onDecodeFailed();            // 解码失败时自动判断是否需要请求
    void setMinInterval(uint32_t ms); // 防止过于频繁请求（默认 1000ms）
};
```

**2.4 验收标准：**
- 接收端启动后 < 500ms 内收到首个关键帧开始解码
- 模拟丢掉关键帧后，能自动恢复画面

---

### Phase 2.5：FEC 解码（前向纠错）

**预计时间：2~3 天**

#### 说明

FEC (Forward Error Correction) 允许接收端在不重传的情况下恢复丢失的包。WebRTC 常用两种 FEC：

| FEC 方案 | RFC | 说明 | 适用场景 |
|----------|-----|------|---------|
| ULP FEC | RFC 5109 | 基于 XOR 的冗余包 | 通用，aiortc 默认支持 |
| FlexFEC | RFC 8627 | Google 扩展，2D XOR | 更强纠错能力 |

二阶段先实现 **ULP FEC (RFC 5109)** 的解码端。

#### FEC 恢复流程

```
接收到的 RTP 包分两类：
  ├─ 媒体包 (PT=96 等，正常视频数据)
  └─ FEC 包 (PT=127 等，冗余数据)

恢复流程：
1. 收到 FEC 包 → 解析 FEC Header，获取保护的媒体包 seq 列表
2. 检查对应的媒体包是否已收到
3. 如果有 1 个媒体包丢失 → 用 FEC + 其余已收到的媒体包 XOR 恢复
4. 恢复的包注入 Jitter Buffer
```

#### 核心接口

```cpp
class FecDecoder {
public:
    // 收到媒体包时调用
    void onMediaPacket(const RtpPacket& packet);

    // 收到 FEC 包时调用，返回恢复出的媒体包列表
    std::vector<RtpPacket> onFecPacket(const RtpPacket& fecPacket);

    struct Stats {
        uint32_t fec_packets_received;
        uint32_t packets_recovered;
        uint32_t recovery_failures;  // FEC 无法恢复（丢失 > 1 个）
    };
    Stats getStats() const;
};
```

**2.5 验收标准：**
- 确认发送端确实发送了 FEC 包（抓包验证）
- 模拟 5% 丢包下，FEC 恢复率 > 60%
- 日志可见 FEC 恢复成功的包计数

---

### Phase 2.6：GCC 拥塞反馈（REMB / TWCC）

**预计时间：3~5 天**

#### 说明

GCC (Google Congestion Control) 是 WebRTC 的带宽估计算法，接收端需要反馈网络状况给发送端，让发送端动态调整码率。

有两种反馈机制：

| 机制 | 方向 | RTCP 类型 | 说明 |
|------|------|-----------|------|
| REMB | 接收端→发送端 | RTCP APP (PT=206, FMT=15) | 告知发送端"我估计可用带宽为 X bps" |
| TWCC | 接收端→发送端 | RTCP Transport-CC (PT=205, FMT=15) | 逐包反馈到达时间，发送端做带宽估计 |

#### 实现策略

先实现 **REMB**（较简单），后续按需迁移到 **TWCC**：

```
接收端带宽估计流程 (REMB)：
1. 统计接收码率（滑动窗口，1s）
2. 观察丢包率 + 延迟变化趋势
3. 估计可用带宽：
   - 丢包 < 2% & 延迟稳定 → 上探 (×1.08)
   - 丢包 2%~10% → 保持
   - 丢包 > 10% 或延迟增长 → 下调 (×0.85)
4. 通过 RTCP REMB 发送给发送端
5. 发送端据此调整编码码率
```

#### TWCC 反馈（进阶）

```
TWCC 流程：
1. 发送端在 RTP 扩展头中加入 transport-wide sequence number
2. 接收端记录每个包的到达时间
3. 接收端定期（~100ms）发送 RTCP Transport-CC 反馈包
4. 包含：base seq, packet status (received/lost), recv delta (到达时间差)
5. 发送端用 Trendline Filter 估计带宽
```

#### 核心接口

```cpp
class CongestionController {
public:
    // 每收到一个 RTP 包调用
    void onRtpReceived(uint16_t seq, uint32_t timestamp,
                       size_t size, int64_t arrival_time_ms);

    // 周期调用（~1s），获取 REMB 估计值
    uint32_t getEstimatedBandwidthBps() const;

    // 生成 RTCP REMB 包
    std::vector<uint8_t> buildRembPacket(uint32_t sender_ssrc,
                                          uint32_t media_ssrc) const;

    // --- TWCC (进阶) ---
    void onRtpReceivedTWCC(uint16_t transport_seq, int64_t arrival_time_us);
    std::vector<uint8_t> buildTransportCCFeedback();

    struct Stats {
        uint32_t estimated_bw_bps;
        float loss_rate;
        float avg_rtt_ms;
        float jitter_ms;
    };
    Stats getStats() const;
};
```

**2.6 验收标准：**
- REMB 反馈正常发送（抓包可见 RTCP REMB）
- 网络变差时发送端码率下降，网络恢复后码率回升
- QML 界面可实时显示当前估计带宽

---

## 五、项目目录结构

```
MOCTS_Q20B/                     (或独立项目目录)
├── CMakeLists.txt              # 顶层 CMake
├── third_party/
│   ├── libdatachannel/         # git submodule
│   ├── ffmpeg/                 # 预编译库 (Windows: dll+lib+headers)
│   ├── nlohmann_json/          # 头文件 only
│   └── ixwebsocket/            # WebSocket 客户端 (或用 QWebSocket)
│
├── src/
│   ├── main.cpp                # Qt 入口
│   ├── signaling/
│   │   ├── SignalingClient.h/cpp    # WebSocket 信令客户端
│   │   └── SignalingMessage.h       # JSON 消息定义
│   │
│   ├── webrtc/
│   │   ├── PeerConnectionManager.h/cpp  # libdatachannel 封装
│   │   └── SdpUtils.h/cpp              # SDP 解析工具
│   │
│   ├── rtp/
│   │   ├── RtpPacket.h/cpp          # RTP 包解析
│   │   ├── RtpDepacketizer.h/cpp    # H.264 RTP 解包 (FU-A, STAP-A)
│   │   ├── JitterBuffer.h/cpp       # 抖动缓冲 (Phase 2.2)
│   │   ├── NackGenerator.h/cpp      # NACK 生成 (Phase 2.3)
│   │   ├── KeyframeRequester.h/cpp  # PLI/FIR (Phase 2.4)
│   │   ├── FecDecoder.h/cpp         # ULP FEC 解码 (Phase 2.5)
│   │   └── CongestionController.h/cpp # GCC 反馈 (Phase 2.6)
│   │
│   ├── decoder/
│   │   ├── VideoDecoder.h/cpp       # FFmpeg 解码器封装
│   │   └── HardwareAccel.h/cpp      # 硬件加速 (DXVA2/MediaCodec)
│   │
│   └── render/
│       ├── VideoFrameProvider.h/cpp  # QML 视频帧提供者
│       └── YuvRenderer.h/cpp        # OpenGL YUV 渲染
│
├── qml/
│   ├── main.qml                # 主界面
│   └── StatsOverlay.qml        # 统计信息浮层
│
└── tests/
    ├── test_jitter_buffer.cpp
    ├── test_rtp_depacketizer.cpp
    ├── test_nack.cpp
    ├── test_fec.cpp
    └── test_congestion.cpp
```

---

## 六、开发环境搭建

### Windows 开发环境

| 工具 | 版本 | 用途 |
|------|------|------|
| Visual Studio | 2019 / 2022 | MSVC 编译器 |
| Qt | 5.14.2 (MSVC 2017 64-bit) | UI 框架 |
| CMake | 3.16+ | 构建系统 |
| FFmpeg | 6.x / 7.x (预编译 shared) | 视频解码 |
| libdatachannel | 0.21+ (CMake submodule) | WebRTC 协议栈 |
| Wireshark | 最新 | 抓包分析 RTP/RTCP |
| clumsy | 0.2+ | 模拟丢包 / 延迟 / 抖动 |

### FFmpeg Windows 预编译获取

```bash
# 推荐使用 shared build (含 dll + lib + headers)
# 下载地址: https://github.com/BtbN/FFmpeg-Builds/releases
# 选择: ffmpeg-n6.1-latest-win64-lgpl-shared.zip

# 解压后目录结构：
# ffmpeg/
# ├── bin/    (*.dll)
# ├── lib/    (*.lib)
# └── include/ (libavcodec/ libavformat/ ...)
```

### libdatachannel 引入方式

```cmake
# CMakeLists.txt
include(FetchContent)
FetchContent_Declare(
    libdatachannel
    GIT_REPOSITORY https://github.com/nicedatachannels/libdatachannel.git
    GIT_TAG        v0.21.1
)
option(NO_EXAMPLES "Disable examples" ON)
option(NO_TESTS "Disable tests" ON)
FetchContent_MakeAvailable(libdatachannel)

target_link_libraries(${PROJECT_NAME} PRIVATE LibDataChannel::LibDataChannel)
```

---

## 七、信令服务器改造

在一阶段 `server.js` 基础上新增原生 WebSocket 端点：

```javascript
// === 新增 WebSocket 端点（二阶段 C++ 客户端使用）===
const WebSocket = require('ws');
const wss = new WebSocket.Server({ noServer: true });

server.on('upgrade', (request, socket, head) => {
    if (request.url === '/ws') {
        wss.handleUpgrade(request, socket, head, (ws) => {
            wss.emit('connection', ws, request);
        });
    }
});

wss.on('connection', (ws) => {
    console.log('[WS] 新连接');
    let currentRoom = null;

    ws.on('message', (raw) => {
        const msg = JSON.parse(raw);
        switch (msg.type) {
            case 'join':
                currentRoom = msg.room;
                // 通知 Socket.IO 房间里的其他人
                io.to(currentRoom).emit('peer-joined', 'ws-client');
                // 也监听 Socket.IO 房间的消息
                break;
            case 'answer':
                io.to(currentRoom).emit('answer', {
                    sdp: msg.sdp, from: 'ws-client'
                });
                break;
            case 'ice-candidate':
                io.to(currentRoom).emit('ice-candidate', {
                    candidate: msg.candidate, from: 'ws-client'
                });
                break;
        }
    });

    // 转发 Socket.IO 事件给 WebSocket 客户端
    const forwardOffer = (data) => {
        ws.send(JSON.stringify({ type: 'offer', sdp: data.sdp }));
    };
    const forwardIce = (data) => {
        ws.send(JSON.stringify({ type: 'ice-candidate', candidate: data.candidate }));
    };

    // 监听 Socket.IO 的 offer/ice 事件（需要适配房间逻辑）
    io.on('connection', (socket) => {
        socket.on('offer', (data) => {
            if (data.room === currentRoom) forwardOffer(data);
        });
        socket.on('ice-candidate', (data) => {
            if (data.room === currentRoom) forwardIce(data);
        });
    });

    ws.on('close', () => console.log('[WS] 断开'));
});
```

> **注**：以上为示意代码，实际需要处理好房间映射和多客户端场景。

---

## 八、关键技术难点与应对

### 1. H.264 RTP 解包

| NAL 类型 | 说明 | 处理方式 |
|----------|------|---------|
| 单包 NAL (type 1-23) | 一个 RTP 包 = 一个 NAL Unit | 直接提取 |
| STAP-A (type 24) | 一个 RTP 包 = 多个小 NAL（如 SPS+PPS） | 按长度前缀拆分 |
| FU-A (type 28) | 一个大 NAL 分片到多个 RTP 包 | 按 S/E 标志位重组 |

```
FU-A 头部结构：
[RTP Header] [FU Indicator: 1 byte] [FU Header: 1 byte] [FU Payload]

FU Indicator: F|NRI|Type=28
FU Header:    S|E|R|NAL_Type
  S=1: 分片起始
  E=1: 分片结束
```

### 2. FFmpeg 解码线程模型

```
┌──────────────┐     ┌───────────────┐     ┌──────────────┐
│  网络线程     │     │  解码线程      │     │  渲染线程     │
│ (libdatacha) │     │ (FFmpeg)      │     │  (Qt GUI)    │
│              │     │               │     │              │
│  RTP 收包    ├────►│  avcodec_send ├────►│  QML 更新    │
│  Jitter Buf  │  Q1 │  avcodec_recv │  Q2 │  纹理上屏    │
└──────────────┘     └───────────────┘     └──────────────┘
                Q1: 线程安全帧队列          Q2: 线程安全帧队列
```

### 3. QML 高效渲染

避免 CPU 拷贝，直接 GPU 上传：

```cpp
// 方案 1: QQuickFramebufferObject + OpenGL 纹理
// 方案 2: QVideoSink (Qt 6) / QAbstractVideoSurface (Qt 5)
// 方案 3: 自定义 QSGNode + shader (最高性能)

// Qt 5.14 推荐: 继承 QQuickItem + QSGSimpleTextureNode
// 在 updatePaintNode() 中用 glTexSubImage2D 上传 YUV 数据
```

---

## 九、测试与验证方法

### 网络模拟工具

| 工具 | 平台 | 用途 |
|------|------|------|
| clumsy | Windows | 模拟丢包、延迟、抖动、重复、乱序 |
| Wireshark | 全平台 | 抓包分析 RTP/RTCP 包 |
| tc (netem) | Linux | 精确网络模拟（WSL 可用） |

### 各阶段测试场景

| 阶段 | 测试场景 | 验证方法 |
|------|---------|---------|
| 2.1 | 基础视频 | 目视画面是否正常 |
| 2.2 | 20ms 抖动 | clumsy 设置 Lag，观察画面平滑度 |
| 2.3 | 5% 丢包 | clumsy 设置 Drop，观察花屏情况 |
| 2.4 | 关键帧丢失 | 手动 drop IDR 帧，观察恢复时间 |
| 2.5 | 5% 丢包 + FEC | 对比开启/关闭 FEC 的画面质量 |
| 2.6 | 动态限速 | clumsy 设置 Bandwidth，观察码率自适应 |

### 延迟测量

```
方法：发送端画面叠加高精度时间戳（毫秒），接收端用另一个摄像头同时拍两个屏幕
目标：端到端延迟 < 100ms（优于一阶段的 130ms）
```

---

## 十、后续移植：Android

当 Windows 验证完成后，移植到 Android 的主要变更：

| 模块 | Windows | Android | 变更量 |
|------|---------|---------|--------|
| libdatachannel | MSVC 编译 | NDK 交叉编译 | CMake toolchain 文件 |
| FFmpeg | 预编译 dll | NDK 交叉编译 so | 需要编译脚本 |
| 视频渲染 | OpenGL/D3D | OpenGL ES | shader 适配 |
| 硬件解码 | DXVA2/D3D11VA | MediaCodec | 解码器后端切换 |
| Qt 部署 | .exe | .apk (androiddeployqt) | Qt for Android 配置 |

### Android FFmpeg 交叉编译参考

```bash
./configure \
    --target-os=android \
    --arch=aarch64 \
    --enable-cross-compile \
    --cc=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang \
    --enable-shared \
    --disable-static \
    --enable-decoder=h264 \
    --enable-hwaccel=h264_mediacodec \
    --enable-mediacodec \
    --enable-jni
```

---

## 十一、里程碑时间线总览

```
Week 1:  [====== Phase 2.1 ======]  基础视频通路
Week 2:  [=== 2.2 ===][== 2.3 ==]  Jitter Buffer + NACK
Week 3:  [2.4][ ==== 2.5 ==== ]     PLI/FIR + FEC
Week 4:  [ ====== Phase 2.6 ====== ] GCC 拥塞反馈
Week 5:  [ 集成测试 + 性能调优 ]     端到端 < 100ms 验证
Week 6+: [ Android 移植 ]           交叉编译 + 硬件解码
```

---

## 十二、参考资料

| 资料 | 链接 |
|------|------|
| libdatachannel 文档 | https://github.com/nicedatachannels/libdatachannel |
| RFC 3550 (RTP) | https://tools.ietf.org/html/rfc3550 |
| RFC 6184 (H.264 RTP) | https://tools.ietf.org/html/rfc6184 |
| RFC 4585 (NACK/PLI) | https://tools.ietf.org/html/rfc4585 |
| RFC 5109 (ULP FEC) | https://tools.ietf.org/html/rfc5109 |
| draft-ietf-rmcat-gcc (GCC) | https://tools.ietf.org/html/draft-ietf-rmcat-gcc |
| FFmpeg 解码 API | https://ffmpeg.org/doxygen/trunk/group__lavc__decoding.html |
| WebRTC For The Curious | https://webrtcforthecurious.com/ |
