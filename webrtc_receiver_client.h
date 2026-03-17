#ifndef WEBRTC_RECEIVER_CLIENT_H
#define WEBRTC_RECEIVER_CLIENT_H

#include <QObject>
#include <QWebSocket>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <memory>
#include <functional>

// libwebrtc 头文件
#include "libwebrtc.h"
#include "rtc_peerconnection.h"
#include "rtc_peerconnection_factory.h"
#include "rtc_session_description.h"
#include "rtc_ice_candidate.h"
#include "rtc_mediaconstraints.h"
#include "rtc_types.h"
#include "rtc_rtp_transceiver.h"
#include "rtc_media_stream.h"
#include "rtc_video_track.h"

/**
 * @brief WebRTC 接收端客户端
 *
 * 职责：
 * 1. 通过 WebSocket 连接信令服务器（兼容 Socket.IO 协议）
 * 2. 加入房间，等待服务端用户A发送 Offer
 * 3. 收到 Offer 后创建 Answer，等待 ICE 收集完成
 * 4. 通过信令发送 Answer
 * 5. 建立 P2P 后通过 OnTrack 接收视频流
 */
class WebRTCReceiverClient : public QObject
{
    Q_OBJECT
public:
    explicit WebRTCReceiverClient(QObject *parent = nullptr);
    ~WebRTCReceiverClient();

    // 连接信令服务器，默认 ws://localhost:3000/socket.io/?EIO=4&transport=websocket
    Q_INVOKABLE void connectToSignaling(const QString &url = QString());
    // Android：先请求 RECORD_AUDIO 再连接，解决 webrtc_voice_engine.cc Check failed: adm_
    Q_INVOKABLE void requestPermissionAndConnect(const QString &url = QString());
    Q_INVOKABLE void disconnect();
    // 四项验证诊断：输出 1 权限 2 ADM 初始化 3 禁用音频 4 系统兼容性，便于 logcat 过滤 "VERIFY"
    Q_INVOKABLE void runVerificationDiagnostic();
    // 设置视频渲染器（QML 传入 WebRTCVideoRenderer 实例），收到远端轨道时自动绑定
    Q_INVOKABLE void setVideoRenderer(QObject *renderer);

Q_SIGNALS:
    // 状态变化，供 QML 显示
    void statusChanged(const QString &status);
    // 收到远端视频轨道（QML 可绑定到渲染器）
    void remoteVideoTrackReady(libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack> track);

private:
    // ========== 信令层（Socket.IO 协议）==========
    void onWebSocketConnected();
    void onWebSocketDisconnected();
    void onWebSocketError(QAbstractSocket::SocketError error);
    void onWebSocketTextMessageReceived(const QString &message);
    void handleSocketIOMessage(const QString &message);
    void sendSocketIOConnect();
    void sendSocketIOEvent(const QString &event, const QJsonValue &data);
    void sendJoinRoom();
    void sendAnswer(const QString &type, const QString &sdp);

    // ========== WebRTC 层 ==========
    void initWebRTC();
    void createPeerConnection();
    void handleOffer(const QString &type, const QString &sdp);
    void waitForIceGatheringComplete(std::function<void()> callback);

    // ========== RTCPeerConnectionObserver 实现 ==========
    class PeerConnectionObserver;
    std::unique_ptr<PeerConnectionObserver> m_observer;

    // ========== 成员变量 ==========
    QWebSocket *m_webSocket = nullptr;
    QString m_room = "video-room";
    QString m_signalingUrl;
    bool m_socketIOConnected = false;

    // libwebrtc 核心对象
    libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnectionFactory> m_factory;
    libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> m_peerConnection;
    libwebrtc::scoped_refptr<libwebrtc::RTCMediaConstraints> m_constraints;

    bool m_webrtcInitialized = false;
    QObject *m_videoRenderer = nullptr;
};

#endif // WEBRTC_RECEIVER_CLIENT_H
