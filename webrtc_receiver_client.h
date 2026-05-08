#ifndef WEBRTC_RECEIVER_CLIENT_H
#define WEBRTC_RECEIVER_CLIENT_H

#include <atomic>

#include <QObject>
#include <QMutex>
#include <QString>
#include <QTimer>

#include "video_frame_sink.h"

extern "C" {
#include "rflow/Client/librflow_client_api.h"
}

class WebRTCVideoRenderer;

class WebRTCReceiverClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double rttCurrentMs READ rttCurrentMs NOTIFY connectionStatsChanged)
    Q_PROPERTY(double decodeJitterBufferMs READ decodeJitterBufferMs NOTIFY connectionStatsChanged)
    Q_PROPERTY(bool hasDecodeJitterBuffer READ hasDecodeJitterBuffer NOTIFY connectionStatsChanged)
    Q_PROPERTY(bool hasConnectionStats READ hasConnectionStats NOTIFY connectionStatsChanged)
    Q_PROPERTY(QString deviceId READ deviceId WRITE setDeviceId NOTIFY deviceIdChanged)
    Q_PROPERTY(QString deviceSecret READ deviceSecret WRITE setDeviceSecret NOTIFY deviceSecretChanged)
    Q_PROPERTY(int streamIndex READ streamIndex WRITE setStreamIndex NOTIFY streamIndexChanged)
public:
    explicit WebRTCReceiverClient(QObject *parent = nullptr);
    ~WebRTCReceiverClient() override;

    double rttCurrentMs() const { return m_rttCurrentMs; }
    double decodeJitterBufferMs() const { return m_decodeJitterBufferMs; }
    bool hasDecodeJitterBuffer() const { return m_hasDecodeJitterBuffer; }
    bool hasConnectionStats() const { return m_hasConnectionStats; }

    QString deviceId() const { return m_deviceId; }
    void setDeviceId(const QString &deviceId);

    QString deviceSecret() const { return m_deviceSecret; }
    void setDeviceSecret(const QString &deviceSecret);

    int streamIndex() const { return m_streamIndex; }
    void setStreamIndex(int streamIndex);

    Q_INVOKABLE void connectToSignaling(const QString &addr = QString());
    Q_INVOKABLE void requestPermissionAndConnect(const QString &addr = QString());
    Q_INVOKABLE void disconnect();
    Q_INVOKABLE void runVerificationDiagnostic();
    Q_INVOKABLE void setVideoRenderer(QObject *renderer);
    Q_INVOKABLE void setVideoSink(QObject *sink);

    /** 仅供 SDK log 线程通过 QMetaObject 投递解码「抖动缓冲」数值（Parse [DecodeStats]） */
    Q_INVOKABLE void applyDecodePipelineJitterBufferMs(double ms);

    /** 左上角 HUD 文本（建议在 QML 中约 1s 刷新读取） */
    Q_INVOKABLE QString overlayTelemetryText() const;

Q_SIGNALS:
    void statusChanged(const QString &status);
    void connectionStatsChanged();
    void deviceIdChanged();
    void deviceSecretChanged();
    void streamIndexChanged();

private:
    static void onConnectStateThunk(rflow_connect_state_t state, rflow_err_t reason, void *userdata);
    static void onStreamStateThunk(librflow_stream_handle_t handle,
                                   rflow_stream_state_t state,
                                   rflow_err_t reason,
                                   void *userdata);
    static void onVideoFrameThunk(librflow_stream_handle_t handle,
                                  librflow_video_frame_t frame,
                                  void *userdata);
    rflow_err_t configureAndInitSdk(const QString &addr);
    rflow_err_t connectSdk();
    rflow_err_t openStream();
    void closeStream();
    void cleanupSdk();

    void handleConnectState(rflow_connect_state_t state, rflow_err_t reason);
    void handleStreamState(rflow_stream_state_t state, rflow_err_t reason);
    void handleStreamStats(librflow_stream_stats_t stats);
    void schedulePendingFrameDelivery();
    void deliverPendingFrame();
    void pollStreamStats();
    void clearPendingFrame();
    void resetConnectionStats();
    QString formatSdkError(const QString &prefix, rflow_err_t err) const;

    QObject *m_videoRenderer = nullptr;
    VideoFrameSink *m_videoSink = nullptr;

    bool m_sdkInitialized = false;
    bool m_connected = false;
    librflow_stream_handle_t m_streamHandle = nullptr;

    QString m_deviceId;
    QString m_deviceSecret;
    int m_streamIndex = 0;

    mutable QMutex m_pendingFrameMutex;
    librflow_video_frame_t m_pendingFrame = nullptr;
    bool m_frameDeliveryPosted = false;
    quint64 m_overwrittenPendingFrames = 0;
    quint64 m_deliveredFrames = 0;
    bool m_hasReceivedVideoFrame = false;
    bool m_hasReceivedVideoCallback = false;
    bool m_hasReportedRendererMissing = false;
    std::atomic<uint32_t> m_lastFrameWidth{0};
    std::atomic<uint32_t> m_lastFrameHeight{0};

    double m_statsFps = 0.0;
    double m_statsBitrateKbps = 0.0;
    double m_statsNetworkJitterMs = 0.0;
    uint32_t m_statsDurationMs = 0;
    uint64_t m_statsInboundPkts = 0;
    uint32_t m_statsLostPkts = 0;
    uint32_t m_statsFreezeCount = 0;
    uint32_t m_statsDecodeFailCount = 0;

    double m_rttCurrentMs = 0.0;
    /** DecodeStats「抖动缓冲」ms，取自 SDK 日志文本（非 stream_stats） */
    double m_decodeJitterBufferMs = 0.0;
    bool m_hasDecodeJitterBuffer = false;
    bool m_hasConnectionStats = false;
    QTimer m_statsPollTimer;
};

#endif
