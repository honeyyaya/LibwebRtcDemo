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
    Q_PROPERTY(bool hasConnectionStats READ hasConnectionStats NOTIFY connectionStatsChanged)
    Q_PROPERTY(QString deviceId READ deviceId WRITE setDeviceId NOTIFY deviceIdChanged)
    Q_PROPERTY(QString deviceSecret READ deviceSecret WRITE setDeviceSecret NOTIFY deviceSecretChanged)
    Q_PROPERTY(int streamIndex READ streamIndex WRITE setStreamIndex NOTIFY streamIndexChanged)
public:
    explicit WebRTCReceiverClient(QObject *parent = nullptr);
    ~WebRTCReceiverClient() override;

    double rttCurrentMs() const { return m_rttCurrentMs; }
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
    void pollStreamStats();
    void resetConnectionStats();
    QString formatSdkError(const QString &prefix, rflow_err_t err) const;

    QObject *m_videoRenderer = nullptr;
    /// 渲染 sink 在 GUI 线程通过 setVideoSink 写入；SDK 回调线程在 onVideoFrameThunk 中读取。
    /// 必须在重新绑定/解绑前先停 SDK 流，否则 SDK 线程可能 in-flight 调用旧 sink。
    std::atomic<VideoFrameSink *> m_videoSink{nullptr};

    bool m_sdkInitialized = false;
    bool m_connected = false;
    librflow_stream_handle_t m_streamHandle = nullptr;

    QString m_deviceId;
    QString m_deviceSecret;
    int m_streamIndex = 0;

    std::atomic<quint64> m_droppedUnsupportedFrames{0};
    std::atomic<quint64> m_droppedNoSinkFrames{0};
    std::atomic<quint64> m_deliveredFrames{0};
    std::atomic<bool> m_hasReceivedVideoFrame{false};
    std::atomic<bool> m_hasReceivedVideoCallback{false};
    std::atomic<bool> m_hasReportedRendererMissing{false};
    std::atomic<uint32_t> m_lastFrameWidth{0};
    std::atomic<uint32_t> m_lastFrameHeight{0};

    double m_statsFps = 0.0;
    double m_statsBitrateKbps = 0.0;
    /** 来自 librflow_stream_stats_get_jitter_ms（HUD 上的「抖动缓存」即此字段） */
    double m_statsJitterMs = 0.0;
    uint32_t m_statsDurationMs = 0;
    uint64_t m_statsInboundPkts = 0;
    uint32_t m_statsLostPkts = 0;
    uint32_t m_statsFreezeCount = 0;
    uint32_t m_statsDecodeFailCount = 0;

    double m_rttCurrentMs = 0.0;
    bool m_hasConnectionStats = false;
    QTimer m_statsPollTimer;
};

#endif
