# WebRTC 验证方案：完整搭建指南

> 日期：2026-02-28
> 目标：在两台 Windows 电脑之间通过 WebRTC 实现摄像头实时视频传输

---

## 一、整体架构

```
电脑A (IP: 192.168.x.A)                    电脑B (IP: 192.168.x.B)
┌─────────────────────────────┐            ┌─────────────────────────┐
│                             │            │                         │
│  ① 信令服务器 (Node.js)     │◄─Socket.io─►  ③ Qt + QWebEngine     │
│     localhost:3000          │    信令      │     加载 receiver.html  │
│     提供 receiver.html      │             │     显示视频画面        │
│                             │            │                         │
│  ② Python aiortc 发送端     │──WebRTC P2P──►  (WebEngine 内置的     │
│     采集摄像头              │   UDP 直连    │   libwebrtc 接收)      │
│     编码 H.264/VP8          │  视频数据     │                       │
│                             │            │                         │
└─────────────────────────────┘            └─────────────────────────┘
```

### 建立连接的完整过程

```
时间线：

1.  [电脑A] 启动信令服务器 (Node.js :3000)
2.  [电脑A] 启动 Python 发送端 → 连接信令服务器 → 加入房间 "video-room"
3.  [电脑B] 启动 Qt 应用 → QWebEngine 加载 http://电脑A:3000/receiver.html
4.  [电脑B] receiver.html 中的 JS 连接信令服务器 → 加入房间 "video-room"
5.  [电脑A] 信令服务器通知 Python："有接收端加入了"
6.  [电脑A] Python 创建 WebRTC Offer (SDP) → 通过信令服务器转发给电脑B
7.  [电脑B] JS 收到 Offer → 创建 Answer (SDP) → 通过信令服务器转发给电脑A
8.  [双方]  交换 ICE Candidate（网络路径探测）
9.  [双方]  建立 P2P UDP 直连（DTLS 加密握手）
10. [电脑A] Python 开始发送摄像头视频（RTP over SRTP）
11. [电脑B] WebEngine 中的 libwebrtc 接收、解码、显示
```

### 原理说明

- **信令服务器**：仅负责帮助两端交换连接信息（SDP、ICE），不传输视频数据
- **SDP (Session Description Protocol)**：描述媒体能力（支持哪些编码、分辨率等）
- **ICE (Interactive Connectivity Establishment)**：探测两端之间的最佳网络路径
- **DTLS/SRTP**：加密层，保证媒体数据传输安全
- **P2P**：视频数据直接在两台电脑之间传输，不经过信令服务器

---

## 二、环境准备清单

### 电脑A 需要安装的软件

| 序号 | 软件 | 版本 | 用途 | 下载地址 |
|------|------|------|------|---------|
| 1 | Node.js | 18 LTS | 运行信令服务器 | https://nodejs.org/zh-cn/ |
| 2 | Python | 3.9~3.11 | 运行摄像头采集端 | https://www.python.org/downloads/ |
| 3 | FFmpeg | 6.x/7.x | aiortc 依赖的编解码库 | https://www.gyan.dev/ffmpeg/builds/ |
| 4 | 摄像头 | — | 视频源 | 内置或 USB 摄像头 |

### 电脑B 需要安装的软件

| 序号 | 软件 | 版本 | 用途 | 下载地址 |
|------|------|------|------|---------|
| 1 | Qt | 5.14.2（含 WebEngine） | Qt 应用开发 | Qt 在线安装器 |
| 2 | Visual Studio | 2019 Community | C++ 编译器 | https://visualstudio.microsoft.com/ |
| 3 | Qt Creator | 随 Qt 安装 | IDE | — |

### 网络要求

- 两台电脑在同一局域网
- 互相能 ping 通
- 关闭防火墙 或 开放端口 3000(TCP) + 49152-65535(UDP)

---

## 三、电脑A 搭建步骤

### Step A1：安装 Node.js

1. 下载 Node.js 18 LTS（Windows Installer .msi）
2. 双击安装，全部默认下一步
3. 打开 CMD 验证：

```bash
node --version
# 应输出 v18.x.x 或更高

npm --version
# 应输出 9.x.x 或更高
```

### Step A2：安装 Python

1. 下载 Python 3.10.x（Windows Installer 64-bit）
2. 安装时 ✅ 勾选 **"Add Python to PATH"**
3. 打开 CMD 验证：

```bash
python --version
# 应输出 Python 3.10.x

pip --version
# 应输出 pip 2x.x
```

### Step A3：安装 FFmpeg

1. 下载 `ffmpeg-release-essentials.zip`（从 https://www.gyan.dev/ffmpeg/builds/）
2. 解压到 `C:\ffmpeg\`
3. 添加环境变量：系统属性 → 环境变量 → Path → 新建 → `C:\ffmpeg\bin`
4. 打开**新的** CMD 验证：

```bash
ffmpeg -version
# 应输出 ffmpeg version 6.x 或 7.x
```

### Step A4：创建信令服务器

```bash
mkdir C:\webrtc-demo\signaling-server
cd C:\webrtc-demo\signaling-server
npm init -y
npm install express socket.io
```

创建文件 `C:\webrtc-demo\signaling-server\server.js`：

```javascript
const express = require('express');
const http = require('http');
const path = require('path');
const { Server } = require('socket.io');

const app = express();
const server = http.createServer(app);
const io = new Server(server, {
    cors: { origin: "*" }
});

// 提供静态文件（receiver.html）
app.use(express.static(path.join(__dirname, 'public')));

// 信令逻辑
io.on('connection', (socket) => {
    console.log(`[连接] ${socket.id}`);

    socket.on('join', (room) => {
        socket.join(room);
        socket.to(room).emit('peer-joined', socket.id);
        console.log(`[加入房间] ${socket.id} → ${room}`);
    });

    socket.on('offer', (data) => {
        console.log(`[Offer] ${socket.id} → room: ${data.room}`);
        socket.to(data.room).emit('offer', {
            sdp: data.sdp,
            from: socket.id
        });
    });

    socket.on('answer', (data) => {
        console.log(`[Answer] ${socket.id} → room: ${data.room}`);
        socket.to(data.room).emit('answer', {
            sdp: data.sdp,
            from: socket.id
        });
    });

    socket.on('ice-candidate', (data) => {
        socket.to(data.room).emit('ice-candidate', {
            candidate: data.candidate,
            from: socket.id
        });
    });

    socket.on('disconnect', () => {
        console.log(`[断开] ${socket.id}`);
    });
});

const PORT = 3000;
server.listen(PORT, '0.0.0.0', () => {
    console.log(`========================================`);
    console.log(`  信令服务器已启动: http://0.0.0.0:${PORT}`);
    console.log(`  接收端页面: http://本机IP:${PORT}/receiver.html`);
    console.log(`========================================`);
});
```

创建目录和文件 `C:\webrtc-demo\signaling-server\public\receiver.html`：

```html
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>WebRTC 接收端</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { background: #1a1a2e; color: #fff; font-family: 'Segoe UI', sans-serif; }
        #status {
            position: fixed; top: 0; left: 0; right: 0;
            padding: 10px 20px; background: rgba(0,0,0,0.7);
            font-size: 14px; z-index: 10;
        }
        .connected { color: #0f0; }
        .waiting { color: #ff0; }
        .error { color: #f00; }
        video {
            width: 100vw; height: 100vh;
            object-fit: contain; background: #000;
        }
        #info {
            position: fixed; bottom: 10px; left: 10px;
            font-size: 12px; color: #888;
        }
    </style>
</head>
<body>
    <div id="status" class="waiting">⏳ 正在连接信令服务器...</div>
    <video id="remoteVideo" autoplay playsinline></video>
    <div id="info"></div>

    <script src="/socket.io/socket.io.js"></script>
    <script>
    (function() {
        const ROOM = 'video-room';
        const statusEl = document.getElementById('status');
        const videoEl = document.getElementById('remoteVideo');
        const infoEl = document.getElementById('info');

        function setStatus(text, cls) {
            statusEl.textContent = text;
            statusEl.className = cls || '';
        }

        // 1. 连接信令服务器
        const socket = io();

        socket.on('connect', () => {
            setStatus('✅ 已连接信令服务器，等待发送端加入...', 'waiting');
            socket.emit('join', ROOM);
        });

        socket.on('connect_error', (err) => {
            setStatus('❌ 无法连接信令服务器: ' + err.message, 'error');
        });

        // 2. 创建 RTCPeerConnection
        const pc = new RTCPeerConnection({
            iceServers: [{ urls: 'stun:stun.l.google.com:19302' }]
        });

        pc.ontrack = (event) => {
            console.log('收到视频轨道:', event.track.kind);
            videoEl.srcObject = event.streams[0];
            setStatus('🎥 正在接收视频', 'connected');
        };

        pc.onicecandidate = (event) => {
            if (event.candidate) {
                socket.emit('ice-candidate', {
                    room: ROOM,
                    candidate: event.candidate.toJSON()
                });
            }
        };

        pc.oniceconnectionstatechange = () => {
            const state = pc.iceConnectionState;
            infoEl.textContent = 'ICE: ' + state;
            console.log('ICE 状态:', state);
            if (state === 'connected' || state === 'completed') {
                setStatus('🎥 WebRTC 已连接，正在接收视频', 'connected');
            } else if (state === 'disconnected' || state === 'failed') {
                setStatus('❌ WebRTC 连接断开: ' + state, 'error');
            }
        };

        // 3. 信令处理
        socket.on('offer', async (data) => {
            console.log('收到 Offer');
            setStatus('📡 收到 Offer，正在建立连接...', 'waiting');
            try {
                await pc.setRemoteDescription(new RTCSessionDescription(data.sdp));
                const answer = await pc.createAnswer();
                await pc.setLocalDescription(answer);
                socket.emit('answer', {
                    room: ROOM,
                    sdp: { type: answer.type, sdp: answer.sdp }
                });
                console.log('Answer 已发送');
            } catch (err) {
                setStatus('❌ 处理 Offer 失败: ' + err.message, 'error');
                console.error(err);
            }
        });

        socket.on('ice-candidate', async (data) => {
            try {
                if (data.candidate) {
                    await pc.addIceCandidate(new RTCIceCandidate(data.candidate));
                }
            } catch (err) {
                console.error('添加 ICE Candidate 失败:', err);
            }
        });

        socket.on('peer-joined', (peerId) => {
            setStatus('👤 发送端已加入，等待 Offer...', 'waiting');
        });
    })();
    </script>
</body>
</html>
```

### Step A5：创建 Python 发送端

```bash
mkdir C:\webrtc-demo\camera-sender
cd C:\webrtc-demo\camera-sender
pip install aiortc aiohttp python-socketio[asyncio]
```

创建文件 `C:\webrtc-demo\camera-sender\sender.py`：

```python
import asyncio
import sys
import socketio
from aiortc import RTCPeerConnection, RTCSessionDescription
from aiortc.contrib.media import MediaPlayer

# ========== 配置 ==========
SIGNALING_URL = "http://localhost:3000"
ROOM = "video-room"
# ==========================

sio = socketio.AsyncClient(reconnection=True)
pc = RTCPeerConnection()
player = None


def get_camera():
    """获取摄像头（Windows DirectShow）"""
    camera_names = [
        'Integrated Webcam',
        'Integrated Camera',
        'USB Camera',
        'HD Webcam',
        'USB2.0 HD UVC WebCam',
    ]
    for name in camera_names:
        try:
            p = MediaPlayer(
                f'video={name}',
                format='dshow',
                options={'video_size': '640x480', 'framerate': '30'}
            )
            print(f"✅ 摄像头打开成功: {name}")
            return p
        except Exception:
            continue
    print("❌ 无法打开摄像头！请运行以下命令查看设备名称：")
    print("   ffmpeg -list_devices true -f dshow -i dummy")
    sys.exit(1)


@sio.event
async def connect():
    print(f"✅ 已连接信令服务器: {SIGNALING_URL}")
    await sio.emit('join', ROOM)
    print(f"📢 已加入房间: {ROOM}，等待接收端...")


@sio.event
async def disconnect():
    print("❌ 与信令服务器断开连接")


@sio.on('peer-joined')
async def on_peer_joined(peer_id):
    global player
    print(f"👤 接收端加入: {peer_id}")
    player = get_camera()
    if player.video:
        pc.addTrack(player.video)
        print("📷 摄像头轨道已添加")
    offer = await pc.createOffer()
    await pc.setLocalDescription(offer)
    await sio.emit('offer', {
        'room': ROOM,
        'sdp': {
            'type': pc.localDescription.type,
            'sdp': pc.localDescription.sdp
        }
    })
    print("📤 Offer 已发送")


@sio.on('answer')
async def on_answer(data):
    print("📥 收到 Answer")
    answer = RTCSessionDescription(
        sdp=data['sdp']['sdp'],
        type=data['sdp']['type']
    )
    await pc.setRemoteDescription(answer)
    print("✅ 远端描述已设置，等待 ICE 连接...")


@sio.on('ice-candidate')
async def on_ice_candidate(data):
    pass


@pc.on("iceconnectionstatechange")
async def on_ice_state_change():
    state = pc.iceConnectionState
    print(f"🔗 ICE 状态: {state}")
    if state == "connected":
        print("🎉 WebRTC 连接成功！正在发送视频...")
    elif state == "failed":
        print("❌ ICE 连接失败")


@pc.on("connectionstatechange")
async def on_connection_state_change():
    print(f"📶 连接状态: {pc.connectionState}")


async def main():
    print("=" * 50)
    print("  WebRTC 摄像头发送端")
    print(f"  信令服务器: {SIGNALING_URL}")
    print(f"  房间: {ROOM}")
    print("=" * 50)
    try:
        await sio.connect(SIGNALING_URL)
        print("等待中... (按 Ctrl+C 退出)")
        await sio.wait()
    except KeyboardInterrupt:
        print("\n正在关闭...")
    finally:
        await pc.close()
        await sio.disconnect()


if __name__ == "__main__":
    asyncio.run(main())
```

### Step A6：查看摄像头设备名

```bash
ffmpeg -list_devices true -f dshow -i dummy
```

找到你的摄像头名称（如 `"Integrated Webcam"`），确保 `sender.py` 中的 `camera_names` 列表包含它。

### Step A7：开放防火墙

以管理员身份运行 PowerShell：

```powershell
netsh advfirewall firewall add rule name="WebRTC-Signaling" dir=in action=allow protocol=tcp localport=3000
netsh advfirewall firewall add rule name="WebRTC-Media" dir=in action=allow protocol=udp localport=49152-65535
```

---

## 四、电脑B 搭建步骤

### Step B1：安装 Visual Studio 2019

1. 下载 Visual Studio 2019 Community（免费）
2. 安装时勾选 **"使用 C++ 的桌面开发"** 工作负载
3. 确保包含 MSVC v142 编译器 + Windows 10 SDK

### Step B2：安装 Qt 5.14.2

1. 下载 Qt 在线安装器或离线包
2. 安装时必须勾选：
   - ✅ Qt 5.14.2 → MSVC 2017 64-bit
   - ✅ **Qt WebEngine** ← 关键！
   - ✅ Qt WebChannel
   - ✅ Qt Creator
3. 安装完成后打开 Qt Creator，检查编译套件（Kit）

### Step B3：创建 Qt 项目

在 Qt Creator 中创建项目，或手动创建以下文件：

**文件：`WebRTCViewer.pro`**

```qmake
QT += quick webengine
CONFIG += c++17
SOURCES += main.cpp
RESOURCES += qml.qrc
```

**文件：`main.cpp`**

```cpp
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QtWebEngine>

int main(int argc, char *argv[])
{
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QtWebEngine::initialize();

    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;
    const QUrl url(QStringLiteral("qrc:/main.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject *obj, const QUrl &objUrl) {
        if (!obj && url == objUrl)
            QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);
    engine.load(url);
    return app.exec();
}
```

**文件：`main.qml`**

```qml
import QtQuick 2.14
import QtQuick.Controls 2.14
import QtQuick.Layouts 1.14
import QtWebEngine 1.10

ApplicationWindow {
    id: root
    width: 960
    height: 640
    title: "WebRTC 接收端 - 阶段一验证"
    visible: true

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        WebEngineView {
            id: webView
            Layout.fillWidth: true
            Layout.fillHeight: true
            onLoadingChanged: {
                if (loadRequest.status === WebEngineView.LoadSucceededStatus) {
                    statusLabel.text = "✅ 页面加载成功"
                    statusLabel.color = "#4CAF50"
                } else if (loadRequest.status === WebEngineView.LoadFailedStatus) {
                    statusLabel.text = "❌ 页面加载失败: " + loadRequest.errorString
                    statusLabel.color = "#F44336"
                }
            }
            onFeaturePermissionRequested: {
                grantFeaturePermission(securityOrigin, feature, true)
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            color: "#2d2d2d"

            RowLayout {
                anchors.fill: parent
                anchors.margins: 8
                spacing: 8

                Label {
                    id: statusLabel
                    text: "等待连接..."
                    color: "#FFC107"
                    font.pixelSize: 13
                    Layout.preferredWidth: 200
                }
                Item { Layout.fillWidth: true }
                Label { text: "信令服务器:"; color: "#ccc"; font.pixelSize: 13 }
                TextField {
                    id: urlField
                    text: "http://192.168.1.100:3000/receiver.html"
                    Layout.preferredWidth: 350
                    font.pixelSize: 13; selectByMouse: true
                    background: Rectangle { color: "#3d3d3d"; border.color: "#555"; radius: 4 }
                    color: "#fff"
                    onAccepted: connectBtn.clicked()
                }
                Button {
                    id: connectBtn
                    text: "连接"
                    onClicked: {
                        statusLabel.text = "⏳ 正在加载..."
                        statusLabel.color = "#FFC107"
                        webView.url = urlField.text
                    }
                }
                Button { text: "刷新"; onClicked: webView.reload() }
            }
        }
    }
}
```

**文件：`qml.qrc`**

```xml
<RCC>
    <qresource prefix="/">
        <file>main.qml</file>
    </qresource>
</RCC>
```

### Step B4：编译运行

1. Qt Creator 打开 `WebRTCViewer.pro`
2. 选择 Kit：Desktop Qt 5.14.2 MSVC2017 64bit
3. 点击构建（锤子图标）
4. 点击运行（绿色三角）

### Step B5：开放防火墙

```powershell
netsh advfirewall firewall add rule name="WebRTC-Media" dir=in action=allow protocol=udp localport=49152-65535
```

---

## 五、联调运行（严格按顺序！）

### 第 1 步：查看电脑A 的 IP

```bash
ipconfig
# 记录局域网 IP，例如 192.168.1.100
```

### 第 2 步：启动信令服务器（电脑A）

```bash
cd C:\webrtc-demo\signaling-server
node server.js
```

预期输出：
```
========================================
  信令服务器已启动: http://0.0.0.0:3000
  接收端页面: http://本机IP:3000/receiver.html
========================================
```

### 第 3 步：浏览器验证（电脑B）

在电脑B 的 Chrome/Edge 中访问：`http://192.168.1.100:3000/receiver.html`

能看到黑色页面 + 状态文字 → 网络通畅 ✅

### 第 4 步：启动 Qt 应用（电脑B）

1. 运行编译好的 Qt 程序
2. 在地址栏输入 `http://192.168.1.100:3000/receiver.html`（替换为电脑A 实际 IP）
3. 点击"连接"

### 第 5 步：启动 Python 发送端（电脑A）

```bash
cd C:\webrtc-demo\camera-sender
python sender.py
```

预期输出：
```
✅ 已连接信令服务器: http://localhost:3000
📢 已加入房间: video-room，等待接收端...
👤 接收端加入: xxxxxx
✅ 摄像头打开成功: Integrated Webcam
📤 Offer 已发送
📥 收到 Answer
🔗 ICE 状态: connected
🎉 WebRTC 连接成功！正在发送视频...
```

### 第 6 步：确认

电脑B 的 Qt 窗口中应能看到电脑A 的摄像头实时画面 🎉

---

## 六、常见问题排查

| 问题 | 可能原因 | 解决方法 |
|------|---------|---------|
| 电脑B 无法访问 receiver.html | 防火墙阻止 | 电脑A 关闭防火墙或开放 3000 端口 |
| WebEngine 页面空白 | Qt 未安装 WebEngine | 重新安装 Qt，勾选 WebEngine |
| Python 报 "cannot open camera" | 摄像头名称不对 | 运行 `ffmpeg -list_devices true -f dshow -i dummy` |
| ICE 一直是 checking | UDP 被防火墙挡住 | 两台电脑都开放 UDP 49152-65535 |
| 有画面但很卡 | 带宽不足 | sender.py 中改为 320x240 |
| pip install aiortc 失败 | Python 版本不兼容 | 使用 Python 3.9~3.11 |
| Qt 编译报错找不到 webengine | Kit 未正确配置 | 检查 Qt 安装是否勾选了 WebEngine |

---

## 七、文件目录总览

```
电脑A: C:\webrtc-demo\
├── signaling-server\
│   ├── node_modules\          (npm install 自动生成)
│   ├── public\
│   │   └── receiver.html      (接收端网页)
│   ├── server.js              (信令服务器)
│   └── package.json
└── camera-sender\
    └── sender.py              (摄像头发送端)

电脑B: C:\webrtc-demo\
└── WebRTCViewer\
    ├── WebRTCViewer.pro
    ├── main.cpp
    ├── main.qml
    └── qml.qrc
```

---

## 八、后续计划（阶段二）

验证通过后，可以将 QWebEngine 替换为纯 C++17/QML 方案：
- 使用 libdatachannel + FFmpeg（或 Google libwebrtc）
- 实现跨平台支持（Windows + Android）
- 去除对 Chromium 引擎的依赖，减小包体积



