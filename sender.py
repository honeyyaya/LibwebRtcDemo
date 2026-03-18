import asyncio
import sys
import socketio
from aiortc import RTCPeerConnection, RTCSessionDescription
from aiortc.contrib.media import MediaPlayer

# ========== 配置 ==========
SIGNALING_URL = "http://localhost:3000"
ROOM = "video-room"
# 若客户端崩溃，可降低分辨率和帧率减轻解码压力
VIDEO_SIZE = "640x480"   # 调试用可改为 "640x480"，正常 "1920x1080"
VIDEO_FPS = 30           # 调试用可改为 30，正常 60
# ==========================

sio = socketio.AsyncClient(reconnection=True)
pc = None       # 每次连接重新创建
player = None


def get_camera():
    """获取摄像头（Windows DirectShow）"""
    camera_names = [
        'KS2A418-2.0',      
        'Integrated Webcam',
        'Integrated Camera',
        'HD Webcam',
        'USB2.0 HD UVC WebCam',
    ]
    for name in camera_names:
        try:
            p = MediaPlayer(
                f'video={name}',
                format='dshow',
                options={
                    'video_size': VIDEO_SIZE,
                    'framerate': str(VIDEO_FPS),
                    'vcodec': 'mjpeg',
                }
            )
            print(f"✅ 摄像头打开成功: {name}")
            return p
        except Exception:
            continue
    print("❌ 无法打开摄像头！请运行以下命令查看设备名称：")
    print("   ffmpeg -list_devices true -f dshow -i dummy")
    sys.exit(1)


async def create_peer_connection():
    """每次新连接时创建新的 RTCPeerConnection"""
    global pc

    # 关闭旧连接
    if pc is not None:
        await pc.close()

    pc = RTCPeerConnection()

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

    # 注：aiortc 可能不触发 icecandidate（候选打包在 SDP 中），若触发则转发
    @pc.on("icecandidate")
    async def on_ice_candidate(event):
        if event.candidate:
            await sio.emit("ice-candidate", {
                "room": ROOM,
                "candidate": {
                    "candidate": event.candidate.candidate,
                    "sdpMid": event.candidate.sdpMid,
                    "sdpMLineIndex": event.candidate.sdpMLineIndex,
                },
            })
            print("📤 ICE 候选已发送")

    return pc


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
    await create_peer_connection()

    player = get_camera()
    if player is None or player.video is None:
        print("⚠️ 摄像头未就绪")
        return

    pc.addTrack(player.video)
    print("📷 摄像头轨道已添加")

    offer = await pc.createOffer()

    # 在 video m-line 后插入带宽（仅一次，避免重复/错位导致 SDP 解析失败）
    lines = offer.sdp.split('\r\n')
    new_lines = []
    for line in lines:
        new_lines.append(line)
        if line.startswith('m=video '):
            new_lines.append('b=AS:10000')  # 10 Mbps
    sdp = '\r\n'.join(new_lines)

    modified_offer = RTCSessionDescription(sdp=sdp, type=offer.type)
    await pc.setLocalDescription(modified_offer)

    await sio.emit('offer', {
        'room': ROOM,
        'sdp': {
            'type': pc.localDescription.type,
            'sdp': pc.localDescription.sdp
        }
    })
    print("📤 Offer 已发送 (码率: 10Mbps)")


@sio.on('answer')
async def on_answer(data):
    """收到接收端的 Answer"""
    if pc is None or pc.signalingState != "have-local-offer":
        print(f"⚠️ 忽略 Answer（当前状态: {pc.signalingState if pc else 'None'}）")
        return

    print("📥 收到 Answer")
    answer = RTCSessionDescription(
        sdp=data['sdp']['sdp'],
        type=data['sdp']['type']
    )
    await pc.setRemoteDescription(answer)
    print("✅ 远端描述已设置，等待 ICE 连接...")


@sio.on("ice-candidate")
async def on_ice_candidate(data):
    """接收客户端的 ICE 候选并添加到连接"""
    if pc is None or "candidate" not in data:
        return
    try:
        from aiortc import RTCIceCandidate
        c = data["candidate"]
        candidate = RTCIceCandidate(
            candidate=c.get("candidate", ""),
            sdpMid=c.get("sdpMid"),
            sdpMLineIndex=c.get("sdpMLineIndex"),
        )
        await pc.addIceCandidate(candidate)
        print("📥 ICE 候选已添加")
    except Exception as e:
        print(f"⚠️ 添加 ICE 候选失败: {e}")


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
        if pc:
            await pc.close()
        await sio.disconnect()


if __name__ == "__main__":
    asyncio.run(main())