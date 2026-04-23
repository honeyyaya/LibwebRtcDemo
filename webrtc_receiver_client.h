#ifndef WEBRTC_RECEIVER_CLIENT_H
#define WEBRTC_RECEIVER_CLIENT_H

#include <QObject>
#include <QMutex>
#include <QString>

extern "C" {
#include "rflow/Client/librflow_client_api.h"
}

class WebRTCVideoRenderer;

class WebRTCReceiverClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double rttCurrentMs READ rttCurrentMs NOTIFY connectionStatsChanged)
    Q_PROPERTY(double rttAvgMs READ rttAvgMs NOTIFY connectionStatsChanged)
    Q_PROPERTY(double jitterBufferMs READ jitterBufferMs NOTIFY connectionStatsChanged)
    Q_PROPERTY(bool hasConnectionStats READ hasConnectionStats NOTIFY connectionStatsChanged)
    Q_PROPERTY(QString deviceId READ deviceId WRITE setDeviceId NOTIFY deviceIdChanged)
    Q_PROPERTY(QString deviceSecret READ deviceSecret WRITE setDeviceSecret NOTIFY deviceSecretChanged)
    Q_PROPERTY(int streamIndex READ streamIndex WRITE setStreamIndex NOTIFY streamIndexChanged)
public:
    explicit WebRTCReceiverClient(QObject *parent = nullptr);
    ~WebRTCReceiverClient() override;

    double rttCurrentMs() const { return m_rttCurrentMs; }
    double rttAvgMs() const { return m_rttAvgMs; }
    double jitterBufferMs() const { return m_jitterBufferMs; }
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
    static void onStreamStatsThunk(librflow_stream_stats_t stats, void *userdata);

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
    void clearPendingFrame();
    void resetConnectionStats();
    QString formatSdkError(const QString &prefix, rflow_err_t err) const;

    QObject *m_videoRenderer = nullptr;

    bool m_sdkInitialized = false;
    bool m_connected = false;
    librflow_stream_handle_t m_streamHandle = nullptr;

    QString m_deviceId;
    QString m_deviceSecret;
    int m_streamIndex = 0;

    mutable QMutex m_pendingFrameMutex;
    librflow_video_frame_t m_pendingFrame = nullptr;
    bool m_frameDeliveryPosted = false;

    double m_rttCurrentMs = 0.0;
    double m_rttAvgMs = 0.0;
    double m_jitterBufferMs = 0.0;
    bool m_hasConnectionStats = false;
};

#endif
