#ifndef WEBRTC_RECEIVER_CLIENT_H
#define WEBRTC_RECEIVER_CLIENT_H

#include <QObject>
#include <QTimer>
#include <memory>
#include <functional>

#include "signaling_client.h"

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

class WebRTCReceiverClient : public QObject
{
    Q_OBJECT
public:
    explicit WebRTCReceiverClient(QObject *parent = nullptr);
    ~WebRTCReceiverClient();

    Q_INVOKABLE void connectToSignaling(const QString &addr = QString());
    Q_INVOKABLE void requestPermissionAndConnect(const QString &addr = QString());
    Q_INVOKABLE void disconnect();
    Q_INVOKABLE void runVerificationDiagnostic();
    Q_INVOKABLE void setVideoRenderer(QObject *renderer);

Q_SIGNALS:
    void statusChanged(const QString &status);
    void remoteVideoTrackReady(libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack> track);

private:
    // ========== WebRTC 层 ==========
    void initWebRTC();
    void createPeerConnection();
    void handleOffer(const std::string &type, const std::string &sdp);
    void handleRemoteIceCandidate(const std::string &mid, int mline_index,
                                  const std::string &candidate);

    // ========== RTCPeerConnectionObserver 实现 ==========
    class PeerConnectionObserver;
    std::unique_ptr<PeerConnectionObserver> m_observer;

    // ========== 信令层 (TCP + JSON-per-line) ==========
    std::unique_ptr<webrtc_demo::SignalingClient> m_signaling;

    // ========== libwebrtc 核心对象 ==========
    libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnectionFactory> m_factory;
    libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> m_peerConnection;
    libwebrtc::scoped_refptr<libwebrtc::RTCMediaConstraints> m_constraints;

    bool m_webrtcInitialized = false;
    QObject *m_videoRenderer = nullptr;
};

#endif // WEBRTC_RECEIVER_CLIENT_H
