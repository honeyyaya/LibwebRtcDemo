#ifndef WEBRTC_RECEIVER_CLIENT_H
#define WEBRTC_RECEIVER_CLIENT_H

#include <QObject>
#include <QTimer>
#include <memory>
#include <string>
#include <vector>

#include "signaling_client.h"

#include "api/jsep.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "api/set_remote_description_observer_interface.h"

class WebRTCReceiverClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double rttCurrentMs READ rttCurrentMs NOTIFY connectionStatsChanged)
    Q_PROPERTY(double rttAvgMs READ rttAvgMs NOTIFY connectionStatsChanged)
    Q_PROPERTY(double jitterBufferMs READ jitterBufferMs NOTIFY connectionStatsChanged)
    Q_PROPERTY(bool hasConnectionStats READ hasConnectionStats NOTIFY connectionStatsChanged)
public:
    explicit WebRTCReceiverClient(QObject *parent = nullptr);
    ~WebRTCReceiverClient();

    double rttCurrentMs() const { return m_rttCurrentMs; }
    double rttAvgMs() const { return m_rttAvgMs; }
    double jitterBufferMs() const { return m_jitterBufferMs; }
    bool hasConnectionStats() const { return m_hasConnectionStats; }

    Q_INVOKABLE void connectToSignaling(const QString &addr = QString());
    Q_INVOKABLE void requestPermissionAndConnect(const QString &addr = QString());
    Q_INVOKABLE void disconnect();
    Q_INVOKABLE void runVerificationDiagnostic();
    Q_INVOKABLE void setVideoRenderer(QObject *renderer);

Q_SIGNALS:
    void statusChanged(const QString &status);
    void remoteVideoTrackReady(webrtc::scoped_refptr<webrtc::VideoTrackInterface> track);
    void connectionStatsChanged();

private:
    class PeerConnectionObserverImpl;
    friend class PeerConnectionObserverImpl;

    void initWebRTC();
    void createPeerConnection();
    void handleOffer(const std::string &type, const std::string &sdp);
    void handleRemoteIceCandidate(const std::string &mid, int mline_index,
                                  const std::string &candidate);
    void addRemoteIceCandidateNow(const std::string &mid, int mline_index,
                                  const std::string &candidate);
    void flushPendingRemoteIceCandidates();
    void doCreateAnswerAfterSetRemote();

    std::unique_ptr<PeerConnectionObserverImpl> m_observer;

    std::unique_ptr<webrtc_demo::SignalingClient> m_signaling;

    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> m_factory;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> m_peerConnection;

    // SetRemoteDescription 异步完成前 AddIceCandidate 常失败；先排队，就绪后一次性加入。
    struct PendingRemoteIce {
        std::string mid;
        int mline_index = 0;
        std::string candidate;
    };
    std::vector<PendingRemoteIce> m_pendingRemoteIce;
    bool m_remoteDescriptionApplied = false;

    // SetRemote / CreateAnswer / SetLocal：异步完成前用成员持有 scoped_refptr，避免只剩栈上引用被析构导致回调不来。
    webrtc::scoped_refptr<webrtc::SetRemoteDescriptionObserverInterface> m_pendingSetRemoteObserver;
    webrtc::scoped_refptr<webrtc::CreateSessionDescriptionObserver> m_pendingCreateAnswerObserver;
    webrtc::scoped_refptr<webrtc::SetSessionDescriptionObserver> m_pendingSetLocalObserver;

    bool m_webrtcInitialized = false;
    QObject *m_videoRenderer = nullptr;

    QTimer *m_statsTimer = nullptr;
    void startStatsTimer();
    void stopStatsTimer();
    void resetConnectionStats();

    double m_rttCurrentMs = 0.0;
    double m_rttAvgMs = 0.0;
    double m_jitterBufferMs = 0.0;
    bool m_hasConnectionStats = false;

    uint32_t m_prevFramesDecoded = 0;
    double m_prevTotalDecodeTime = 0.0;
    double m_prevTotalProcessingDelay = 0.0;
    double m_prevTotalAssemblyTime = 0.0;
    uint32_t m_prevFramesReceived = 0;
    uint32_t m_prevFramesDropped = 0;
    double m_prevJitterBufferDelay = 0.0;
    uint64_t m_prevJitterBufferEmitted = 0;
};

#endif
