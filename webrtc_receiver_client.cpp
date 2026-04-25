#include "webrtc_receiver_client.h"

#include "latency_trace.h"
#include "webrtc_video_renderer.h"

#include <QDebug>
#include <QMetaObject>
#include <QPointer>
#include <QMutexLocker>

#ifdef Q_OS_ANDROID
#include <QJniObject>
#endif

#ifndef DEFAULT_SIGNALING_ADDR
#define DEFAULT_SIGNALING_ADDR "192.168.3.20:8765"
#endif

namespace {

QString effectiveSignalAddr(const QString &addr)
{
    const QString trimmed = addr.trimmed();
    return trimmed.isEmpty() ? QStringLiteral(DEFAULT_SIGNALING_ADDR) : trimmed;
}

}  // namespace

WebRTCReceiverClient::WebRTCReceiverClient(QObject *parent)
    : QObject(parent)
{
}

WebRTCReceiverClient::~WebRTCReceiverClient()
{
    disconnect();
    clearPendingFrame();
}

void WebRTCReceiverClient::setDeviceId(const QString &deviceId)
{
    if (m_deviceId == deviceId) {
        return;
    }
    m_deviceId = deviceId;
    Q_EMIT deviceIdChanged();
}

void WebRTCReceiverClient::setDeviceSecret(const QString &deviceSecret)
{
    if (m_deviceSecret == deviceSecret) {
        return;
    }
    m_deviceSecret = deviceSecret;
    Q_EMIT deviceSecretChanged();
}

void WebRTCReceiverClient::setStreamIndex(int streamIndex)
{
    if (m_streamIndex == streamIndex) {
        return;
    }
    m_streamIndex = streamIndex;
    Q_EMIT streamIndexChanged();
}

void WebRTCReceiverClient::requestPermissionAndConnect(const QString &addr)
{
    connectToSignaling(addr);
}

void WebRTCReceiverClient::connectToSignaling(const QString &addr)
{
    disconnect();
    demo::latency_trace::reset();

    const QString signalAddr = effectiveSignalAddr(addr);
    Q_EMIT statusChanged(QStringLiteral("正在初始化 libRoboFlow..."));

    rflow_err_t err = configureAndInitSdk(signalAddr);
    if (err != RFLOW_OK) {
        Q_EMIT statusChanged(formatSdkError(QStringLiteral("初始化 SDK 失败"), err));
        cleanupSdk();
        return;
    }

    err = connectSdk();
    if (err != RFLOW_OK) {
        Q_EMIT statusChanged(formatSdkError(QStringLiteral("连接 SDK 失败"), err));
        cleanupSdk();
        return;
    }

    err = openStream();
    if (err != RFLOW_OK) {
        Q_EMIT statusChanged(formatSdkError(QStringLiteral("打开流失败"), err));
        cleanupSdk();
        return;
    }

    Q_EMIT statusChanged(QStringLiteral("SDK 已连接，等待视频流..."));
}

void WebRTCReceiverClient::disconnect()
{
    closeStream();
    cleanupSdk();
    clearPendingFrame();
    resetConnectionStats();
    m_overwrittenPendingFrames = 0;
    m_deliveredFrames = 0;
    demo::latency_trace::reset();

    if (m_videoSink) {
        m_videoSink->clearVideoTrack();
    }

    Q_EMIT statusChanged(QStringLiteral("已断开"));
}

void WebRTCReceiverClient::runVerificationDiagnostic()
{
#ifdef Q_OS_ANDROID
    const int sdkInt = QJniObject::getStaticField<jint>("android/os/Build$VERSION", "SDK_INT");
    const QJniObject releaseObj =
        QJniObject::getStaticObjectField("android/os/Build$VERSION", "RELEASE", "Ljava/lang/String;");
    const QString release = releaseObj.isValid() ? releaseObj.toString() : QStringLiteral("unknown");
    qInfo().noquote() << QStringLiteral("[RFlowDiag] Android %1 / API %2").arg(release).arg(sdkInt);
#else
    qInfo() << "[RFlowDiag] Desktop diagnostic";
#endif
    qInfo().noquote() << QStringLiteral("[RFlowDiag] device_id=\"%1\" stream_index=%2 sdk_initialized=%3 connected=%4")
                             .arg(m_deviceId)
                             .arg(m_streamIndex)
                             .arg(m_sdkInitialized ? QStringLiteral("true") : QStringLiteral("false"))
                             .arg(m_connected ? QStringLiteral("true") : QStringLiteral("false"));
}

void WebRTCReceiverClient::setVideoRenderer(QObject *renderer)
{
    setVideoSink(renderer);
}

void WebRTCReceiverClient::setVideoSink(QObject *sink)
{
    if (m_videoRenderer == sink) {
        return;
    }
    if (m_videoSink) {
        m_videoSink->clearVideoTrack();
    }
    m_videoRenderer = sink;
    m_videoSink = qobject_cast<VideoFrameSink *>(sink);
}

void WebRTCReceiverClient::onConnectStateThunk(rflow_connect_state_t state,
                                               rflow_err_t reason,
                                               void *userdata)
{
    auto *self = static_cast<WebRTCReceiverClient *>(userdata);
    if (!self) {
        return;
    }
    QPointer<WebRTCReceiverClient> guard(self);
    QMetaObject::invokeMethod(
        self,
        [guard, state, reason]() {
            if (guard) {
                guard->handleConnectState(state, reason);
            }
        },
        Qt::QueuedConnection);
}

void WebRTCReceiverClient::onStreamStateThunk(librflow_stream_handle_t,
                                              rflow_stream_state_t state,
                                              rflow_err_t reason,
                                              void *userdata)
{
    auto *self = static_cast<WebRTCReceiverClient *>(userdata);
    if (!self) {
        return;
    }
    QPointer<WebRTCReceiverClient> guard(self);
    QMetaObject::invokeMethod(
        self,
        [guard, state, reason]() {
            if (guard) {
                guard->handleStreamState(state, reason);
            }
        },
        Qt::QueuedConnection);
}

void WebRTCReceiverClient::onVideoFrameThunk(librflow_stream_handle_t,
                                             librflow_video_frame_t frame,
                                             void *userdata)
{
    auto *self = static_cast<WebRTCReceiverClient *>(userdata);
    if (!self || !frame) {
        return;
    }

    const quint32 frameId = librflow_video_frame_get_seq(frame);
    demo::latency_trace::recordSdkCallback(frameId,
                                           librflow_video_frame_get_pts_ms(frame),
                                           librflow_video_frame_get_utc_ms(frame),
                                           librflow_video_frame_get_width(frame),
                                           librflow_video_frame_get_height(frame),
                                           librflow_video_frame_get_data_size(frame));

    librflow_video_frame_t retained = librflow_video_frame_retain(frame);
    if (!retained) {
        return;
    }

    {
        QMutexLocker locker(&self->m_pendingFrameMutex);
        if (self->m_pendingFrame) {
            ++self->m_overwrittenPendingFrames;
            if (self->m_overwrittenPendingFrames <= 5 || (self->m_overwrittenPendingFrames % 60) == 0) {
                qInfo().noquote()
                    << QStringLiteral("[RenderQueue] overwrite pending frame total=%1 incoming_frame=%2")
                           .arg(self->m_overwrittenPendingFrames)
                           .arg(frameId);
            }
            librflow_video_frame_release(self->m_pendingFrame);
        }
        self->m_pendingFrame = retained;
    }

    self->schedulePendingFrameDelivery();
}

void WebRTCReceiverClient::onStreamStatsThunk(librflow_stream_stats_t stats, void *userdata)
{
    Q_UNUSED(stats);
    Q_UNUSED(userdata);
}

rflow_err_t WebRTCReceiverClient::configureAndInitSdk(const QString &addr)
{
    librflow_global_config_t globalConfig = librflow_global_config_create();
    librflow_signal_config_t signalConfig = librflow_signal_config_create();
    if (!globalConfig || !signalConfig) {
        if (signalConfig) {
            librflow_signal_config_destroy(signalConfig);
        }
        if (globalConfig) {
            librflow_global_config_destroy(globalConfig);
        }
        return RFLOW_ERR_NO_MEM;
    }

    const QByteArray signalUrl = effectiveSignalAddr(addr).toUtf8();

    rflow_err_t err = librflow_signal_config_set_url(signalConfig, signalUrl.constData());
    if (err == RFLOW_OK) {
        err = librflow_global_config_set_signal(globalConfig, signalConfig);
    }
    if (err == RFLOW_OK) {
        err = librflow_set_global_config(globalConfig);
    }

    librflow_signal_config_destroy(signalConfig);
    librflow_global_config_destroy(globalConfig);

    if (err != RFLOW_OK) {
        return err;
    }

    err = librflow_init();
    if (err == RFLOW_OK) {
        m_sdkInitialized = true;
    }
    return err;
}

rflow_err_t WebRTCReceiverClient::connectSdk()
{
    librflow_connect_info_t info = librflow_connect_info_create();
    librflow_connect_cb_t cb = librflow_connect_cb_create();
    if (!info || !cb) {
        if (cb) {
            librflow_connect_cb_destroy(cb);
        }
        if (info) {
            librflow_connect_info_destroy(info);
        }
        return RFLOW_ERR_NO_MEM;
    }

    rflow_err_t err = librflow_connect_cb_set_on_state(cb, &WebRTCReceiverClient::onConnectStateThunk);
    if (err == RFLOW_OK) {
        err = librflow_connect_cb_set_userdata(cb, this);
    }

    const QByteArray deviceIdUtf8 = m_deviceId.toUtf8();
    if (err == RFLOW_OK && !m_deviceId.isEmpty()) {
        err = librflow_connect_info_set_device_id(info, deviceIdUtf8.constData());
    }

    const QByteArray deviceSecretUtf8 = m_deviceSecret.toUtf8();
    if (err == RFLOW_OK && !m_deviceSecret.isEmpty()) {
        err = librflow_connect_info_set_device_secret(info, deviceSecretUtf8.constData());
    }

    if (err == RFLOW_OK) {
        err = librflow_connect(info, cb);
    }

    librflow_connect_cb_destroy(cb);
    librflow_connect_info_destroy(info);

    if (err == RFLOW_OK) {
        m_connected = true;
    }
    return err;
}

rflow_err_t WebRTCReceiverClient::openStream()
{
    librflow_stream_cb_t cb = librflow_stream_cb_create();
    if (!cb) {
        return RFLOW_ERR_NO_MEM;
    }

    rflow_err_t err = librflow_stream_cb_set_on_state(cb, &WebRTCReceiverClient::onStreamStateThunk);
    if (err == RFLOW_OK) {
        err = librflow_stream_cb_set_on_video(cb, &WebRTCReceiverClient::onVideoFrameThunk);
    }
    if (err == RFLOW_OK) {
        err = librflow_stream_cb_set_userdata(cb, this);
    }
    if (err == RFLOW_OK) {
        err = librflow_open_stream(m_streamIndex, nullptr, cb, &m_streamHandle);
    }

    librflow_stream_cb_destroy(cb);
    return err;
}

void WebRTCReceiverClient::closeStream()
{
    if (!m_streamHandle) {
        return;
    }
    librflow_close_stream(m_streamHandle);
    m_streamHandle = nullptr;
}

void WebRTCReceiverClient::cleanupSdk()
{
    if (m_connected) {
        librflow_disconnect();
        m_connected = false;
    }
    if (m_sdkInitialized) {
        librflow_uninit();
        m_sdkInitialized = false;
    }
}

void WebRTCReceiverClient::handleConnectState(rflow_connect_state_t state, rflow_err_t reason)
{
    switch (state) {
    case RFLOW_CONN_CONNECTING:
        Q_EMIT statusChanged(QStringLiteral("正在连接设备..."));
        break;
    case RFLOW_CONN_CONNECTED:
        Q_EMIT statusChanged(QStringLiteral("连接成功，正在打开流..."));
        break;
    case RFLOW_CONN_DISCONNECTED:
        m_connected = false;
        Q_EMIT statusChanged(QStringLiteral("连接已断开"));
        break;
    case RFLOW_CONN_FAILED:
        m_connected = false;
        Q_EMIT statusChanged(formatSdkError(QStringLiteral("连接失败"), reason));
        break;
    case RFLOW_CONN_IDLE:
    default:
        break;
    }
}

void WebRTCReceiverClient::handleStreamState(rflow_stream_state_t state, rflow_err_t reason)
{
    switch (state) {
    case RFLOW_STREAM_OPENING:
        Q_EMIT statusChanged(QStringLiteral("正在拉流..."));
        break;
    case RFLOW_STREAM_OPENED:
        Q_EMIT statusChanged(QStringLiteral("已开始接收视频"));
        break;
    case RFLOW_STREAM_CLOSING:
        Q_EMIT statusChanged(QStringLiteral("正在关闭流..."));
        break;
    case RFLOW_STREAM_CLOSED:
        m_streamHandle = nullptr;
        Q_EMIT statusChanged(QStringLiteral("流已关闭"));
        break;
    case RFLOW_STREAM_FAILED:
        m_streamHandle = nullptr;
        Q_EMIT statusChanged(formatSdkError(QStringLiteral("流失败"), reason));
        break;
    case RFLOW_STREAM_IDLE:
    default:
        break;
    }
}

void WebRTCReceiverClient::handleStreamStats(librflow_stream_stats_t stats)
{
    // m_rttCurrentMs = static_cast<double>(librflow_stream_stats_get_rtt_ms(stats));
    // m_rttAvgMs = m_rttCurrentMs;
    // m_jitterBufferMs = static_cast<double>(librflow_stream_stats_get_jitter_ms(stats));
    m_hasConnectionStats = true;
    Q_EMIT connectionStatsChanged();
}

void WebRTCReceiverClient::schedulePendingFrameDelivery()
{
    bool shouldPost = false;
    {
        QMutexLocker locker(&m_pendingFrameMutex);
        if (!m_frameDeliveryPosted) {
            m_frameDeliveryPosted = true;
            shouldPost = true;
        }
    }

    if (!shouldPost) {
        return;
    }

    QPointer<WebRTCReceiverClient> guard(this);
    QMetaObject::invokeMethod(
        this,
        [guard]() {
            if (guard) {
                guard->deliverPendingFrame();
            }
        },
        Qt::QueuedConnection);
}

void WebRTCReceiverClient::deliverPendingFrame()
{
    librflow_video_frame_t frame = nullptr;
    {
        QMutexLocker locker(&m_pendingFrameMutex);
        frame = m_pendingFrame;
        m_pendingFrame = nullptr;
        m_frameDeliveryPosted = false;
    }

    if (frame) {
        const rflow_codec_t codec = librflow_video_frame_get_codec(frame);
        const uint32_t width = librflow_video_frame_get_width(frame);
        const uint32_t height = librflow_video_frame_get_height(frame);
        const quint32 frameId = librflow_video_frame_get_seq(frame);
        const uint32_t planeCount = librflow_video_frame_get_plane_count(frame);
        demo::latency_trace::recordUiDispatch(frameId);

        const bool supportedFrame =
            ((codec == RFLOW_CODEC_I420 && planeCount >= 3) || (codec == RFLOW_CODEC_NV12 && planeCount >= 2)) &&
            width > 0 && height > 0;

        if (supportedFrame) {
            if (m_videoSink) {
                ++m_deliveredFrames;
                if (m_deliveredFrames <= 5 || (m_deliveredFrames % 120) == 0) {
                    qInfo().noquote()
                        << QStringLiteral("[RenderQueue] delivered=%1 overwritten=%2 frame=%3 codec=%4")
                               .arg(m_deliveredFrames)
                               .arg(m_overwrittenPendingFrames)
                               .arg(frameId)
                               .arg(codec);
                }
                m_videoSink->presentFrame(frame);
                frame = nullptr;
            }
        } else {
            qWarning().noquote()
                << QStringLiteral("Unsupported frame from libRoboFlow: codec=%1 planes=%2 size=%3x%4")
                       .arg(codec)
                       .arg(planeCount)
                       .arg(width)
                       .arg(height);
        }

        if (frame) {
            librflow_video_frame_release(frame);
        }
    }

    bool repost = false;
    {
        QMutexLocker locker(&m_pendingFrameMutex);
        if (m_pendingFrame && !m_frameDeliveryPosted) {
            m_frameDeliveryPosted = true;
            repost = true;
        }
    }

    if (!repost) {
        return;
    }

    QPointer<WebRTCReceiverClient> guard(this);
    QMetaObject::invokeMethod(
        this,
        [guard]() {
            if (guard) {
                guard->deliverPendingFrame();
            }
        },
        Qt::QueuedConnection);
}

void WebRTCReceiverClient::clearPendingFrame()
{
    QMutexLocker locker(&m_pendingFrameMutex);
    if (m_pendingFrame) {
        librflow_video_frame_release(m_pendingFrame);
        m_pendingFrame = nullptr;
    }
    m_frameDeliveryPosted = false;
}

void WebRTCReceiverClient::resetConnectionStats()
{
    m_rttCurrentMs = 0.0;
    m_rttAvgMs = 0.0;
    m_jitterBufferMs = 0.0;
    if (m_hasConnectionStats) {
        m_hasConnectionStats = false;
        Q_EMIT connectionStatsChanged();
    }
}

QString WebRTCReceiverClient::formatSdkError(const QString &prefix, rflow_err_t err) const
{
    QString message = QStringLiteral("%1: err=%2").arg(prefix).arg(err);
    const char *lastError = librflow_get_last_error();
    if (lastError && lastError[0] != '\0') {
        message += QStringLiteral(" - %1").arg(QString::fromUtf8(lastError));
    }
    return message;
}
