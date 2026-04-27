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

QString backendToString(rflow_video_frame_backend_t backend)
{
    switch (backend) {
    case RFLOW_VIDEO_FRAME_BACKEND_CPU_PLANAR:
        return QStringLiteral("CPU_PLANAR");
    case RFLOW_VIDEO_FRAME_BACKEND_GPU_EXTERNAL:
        return QStringLiteral("GPU_EXTERNAL");
    case RFLOW_VIDEO_FRAME_BACKEND_HARDWARE_BUFFER:
        return QStringLiteral("HARDWARE_BUFFER");
    case RFLOW_VIDEO_FRAME_BACKEND_UNKNOWN:
    default:
        return QStringLiteral("UNKNOWN");
    }
}

QString nativeHandleToString(rflow_native_handle_type_t handleType)
{
    switch (handleType) {
    case RFLOW_NATIVE_HANDLE_ANDROID_OES_TEXTURE:
        return QStringLiteral("ANDROID_OES_TEXTURE");
    case RFLOW_NATIVE_HANDLE_ANDROID_HARDWARE_BUFFER:
        return QStringLiteral("ANDROID_HARDWARE_BUFFER");
    case RFLOW_NATIVE_HANDLE_NONE:
    default:
        return QStringLiteral("NONE");
    }
}

QString connectStateToString(rflow_connect_state_t state)
{
    switch (state) {
    case RFLOW_CONN_IDLE:
        return QStringLiteral("IDLE");
    case RFLOW_CONN_CONNECTING:
        return QStringLiteral("CONNECTING");
    case RFLOW_CONN_CONNECTED:
        return QStringLiteral("CONNECTED");
    case RFLOW_CONN_DISCONNECTED:
        return QStringLiteral("DISCONNECTED");
    case RFLOW_CONN_FAILED:
        return QStringLiteral("FAILED");
    default:
        return QStringLiteral("UNKNOWN");
    }
}

QString streamStateToString(rflow_stream_state_t state)
{
    switch (state) {
    case RFLOW_STREAM_IDLE:
        return QStringLiteral("IDLE");
    case RFLOW_STREAM_OPENING:
        return QStringLiteral("OPENING");
    case RFLOW_STREAM_OPENED:
        return QStringLiteral("OPENED");
    case RFLOW_STREAM_CLOSING:
        return QStringLiteral("CLOSING");
    case RFLOW_STREAM_CLOSED:
        return QStringLiteral("CLOSED");
    case RFLOW_STREAM_FAILED:
        return QStringLiteral("FAILED");
    default:
        return QStringLiteral("UNKNOWN");
    }
}

QString errorToString(rflow_err_t err)
{
    switch (err) {
    case RFLOW_OK:
        return QStringLiteral("RFLOW_OK");
    case RFLOW_ERR_FAIL:
        return QStringLiteral("RFLOW_ERR_FAIL");
    case RFLOW_ERR_PARAM:
        return QStringLiteral("RFLOW_ERR_PARAM");
    case RFLOW_ERR_NOT_SUPPORT:
        return QStringLiteral("RFLOW_ERR_NOT_SUPPORT");
    case RFLOW_ERR_STATE:
        return QStringLiteral("RFLOW_ERR_STATE");
    case RFLOW_ERR_NO_MEM:
        return QStringLiteral("RFLOW_ERR_NO_MEM");
    case RFLOW_ERR_TIMEOUT:
        return QStringLiteral("RFLOW_ERR_TIMEOUT");
    case RFLOW_ERR_BUSY:
        return QStringLiteral("RFLOW_ERR_BUSY");
    case RFLOW_ERR_NOT_FOUND:
        return QStringLiteral("RFLOW_ERR_NOT_FOUND");
    case RFLOW_ERR_TRUNCATED:
        return QStringLiteral("RFLOW_ERR_TRUNCATED");
    case RFLOW_ERR_CONN_FAIL:
        return QStringLiteral("RFLOW_ERR_CONN_FAIL");
    case RFLOW_ERR_CONN_AUTH:
        return QStringLiteral("RFLOW_ERR_CONN_AUTH");
    case RFLOW_ERR_CONN_LICENSE:
        return QStringLiteral("RFLOW_ERR_CONN_LICENSE");
    case RFLOW_ERR_CONN_NETWORK:
        return QStringLiteral("RFLOW_ERR_CONN_NETWORK");
    case RFLOW_ERR_CONN_KICKED:
        return QStringLiteral("RFLOW_ERR_CONN_KICKED");
    case RFLOW_ERR_STREAM_NOT_EXIST:
        return QStringLiteral("RFLOW_ERR_STREAM_NOT_EXIST");
    case RFLOW_ERR_STREAM_ALREADY_OPEN:
        return QStringLiteral("RFLOW_ERR_STREAM_ALREADY_OPEN");
    case RFLOW_ERR_STREAM_CODEC_UNSUPP:
        return QStringLiteral("RFLOW_ERR_STREAM_CODEC_UNSUPP");
    case RFLOW_ERR_STREAM_ENCODER_FAIL:
        return QStringLiteral("RFLOW_ERR_STREAM_ENCODER_FAIL");
    case RFLOW_ERR_STREAM_NO_SUBSCRIBER:
        return QStringLiteral("RFLOW_ERR_STREAM_NO_SUBSCRIBER");
    default:
        return QStringLiteral("RFLOW_ERR_UNKNOWN");
    }
}

}  // namespace

WebRTCReceiverClient::WebRTCReceiverClient(QObject *parent)
    : QObject(parent)
{
    m_statsPollTimer.setInterval(3000);
    connect(&m_statsPollTimer, &QTimer::timeout, this, &WebRTCReceiverClient::pollStreamStats);
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
    const QString requestedDeviceId =
        m_deviceId.trimmed().isEmpty() ? QStringLiteral(RFLOW_DEFAULT_DEVICE_ID) : m_deviceId.trimmed();
    qInfo().noquote() << QStringLiteral("[RFlowConnect] start signal=%1 device_id=\"%2\" stream_index=%3 secret_set=%4")
                             .arg(signalAddr)
                             .arg(requestedDeviceId)
                             .arg(m_streamIndex)
                             .arg(m_deviceSecret.isEmpty() ? QStringLiteral("false") : QStringLiteral("true"));
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
    m_statsPollTimer.stop();
    closeStream();
    cleanupSdk();
    clearPendingFrame();
    resetConnectionStats();
    m_overwrittenPendingFrames = 0;
    m_deliveredFrames = 0;
    m_hasReceivedVideoFrame = false;
    m_hasReceivedVideoCallback = false;
    m_hasReportedRendererMissing = false;
    m_lastFrameWidth = 0;
    m_lastFrameHeight = 0;
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
    m_hasReportedRendererMissing = false;
    qInfo().noquote() << QStringLiteral("[RenderQueue] video sink assigned=%1")
                             .arg(m_videoSink ? QStringLiteral("true") : QStringLiteral("false"));
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
    self->m_lastFrameWidth = librflow_video_frame_get_width(frame);
    self->m_lastFrameHeight = librflow_video_frame_get_height(frame);
    demo::latency_trace::recordSdkCallback(frameId,
                                           librflow_video_frame_get_pts_ms(frame),
                                           librflow_video_frame_get_utc_ms(frame),
                                           self->m_lastFrameWidth,
                                           self->m_lastFrameHeight,
                                           librflow_video_frame_get_data_size(frame));

    if (!self->m_hasReceivedVideoCallback) {
        self->m_hasReceivedVideoCallback = true;
        qInfo().noquote() << QStringLiteral("[RenderQueue] first video callback frame=%1").arg(frameId);
        QPointer<WebRTCReceiverClient> guard(self);
        QMetaObject::invokeMethod(
            self,
            [guard]() {
                if (guard && !guard->m_hasReceivedVideoFrame) {
                    guard->statusChanged(QStringLiteral("已收到视频回调，等待渲染"));
                }
            },
            Qt::QueuedConnection);
    }

    librflow_video_frame_t retained = librflow_video_frame_retain(frame);
    if (!retained) {
        return;
    }

    {
        QMutexLocker locker(&self->m_pendingFrameMutex);
        if (self->m_pendingFrame) {
            ++self->m_overwrittenPendingFrames;
            if (self->m_overwrittenPendingFrames <= 3 || (self->m_overwrittenPendingFrames % 300) == 0) {
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
    auto *self = static_cast<WebRTCReceiverClient *>(userdata);
    if (!self || !stats) {
        return;
    }

    librflow_stream_stats_t retained = librflow_stream_stats_retain(stats);
    if (!retained) {
        return;
    }

    QPointer<WebRTCReceiverClient> guard(self);
    QMetaObject::invokeMethod(
        self,
        [guard, retained]() {
            if (guard) {
                guard->handleStreamStats(retained);
            }
            librflow_stream_stats_release(retained);
        },
        Qt::QueuedConnection);
}

rflow_err_t WebRTCReceiverClient::configureAndInitSdk(const QString &addr)
{
    qInfo().noquote() << QStringLiteral("[RFlowInit] configure signal=%1").arg(effectiveSignalAddr(addr));
    librflow_global_config_t globalConfig = librflow_global_config_create();
    librflow_signal_config_t signalConfig = librflow_signal_config_create();
    if (!globalConfig || !signalConfig) {
        qWarning() << "[RFlowInit] config allocation failed";
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
        qWarning().noquote() << QStringLiteral("[RFlowInit] configure failed err=%1").arg(err);
        return err;
    }

    err = librflow_init();
    if (err == RFLOW_OK) {
        m_sdkInitialized = true;
        qInfo() << "[RFlowInit] librflow_init ok";
    } else {
        qWarning().noquote() << QStringLiteral("[RFlowInit] librflow_init failed err=%1").arg(err);
    }
    return err;
}

rflow_err_t WebRTCReceiverClient::connectSdk()
{
    const QString effectiveDeviceId =
        m_deviceId.trimmed().isEmpty() ? QStringLiteral(RFLOW_DEFAULT_DEVICE_ID) : m_deviceId.trimmed();
    qInfo().noquote() << QStringLiteral("[RFlowConnect] connectSdk device_id=\"%1\" secret_set=%2")
                             .arg(effectiveDeviceId)
                             .arg(m_deviceSecret.isEmpty() ? QStringLiteral("false") : QStringLiteral("true"));
    librflow_connect_info_t info = librflow_connect_info_create();
    librflow_connect_cb_t cb = librflow_connect_cb_create();
    if (!info || !cb) {
        qWarning() << "[RFlowConnect] connect info/callback allocation failed";
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

    const QByteArray deviceIdUtf8 = effectiveDeviceId.toUtf8();
    if (err == RFLOW_OK) {
        err = librflow_connect_info_set_device_id(info, deviceIdUtf8.constData());
    }

    const QByteArray deviceSecretUtf8 = m_deviceSecret.toUtf8();
    if (err == RFLOW_OK) {
        err = librflow_connect_info_set_device_secret(info, deviceSecretUtf8.constData());
    }

    if (err == RFLOW_OK) {
        err = librflow_connect(info, cb);
    }

    librflow_connect_cb_destroy(cb);
    librflow_connect_info_destroy(info);

    if (err == RFLOW_OK) {
        m_connected = true;
        qInfo() << "[RFlowConnect] librflow_connect submitted";
    } else {
        qWarning().noquote() << QStringLiteral("[RFlowConnect] librflow_connect failed err=%1").arg(err);
    }
    return err;
}

rflow_err_t WebRTCReceiverClient::openStream()
{
    qInfo().noquote()
        << QStringLiteral("[RFlowStream] openStream stream_index=%1 preferred_codec=H264")
               .arg(m_streamIndex);
    librflow_stream_cb_t cb = librflow_stream_cb_create();
    librflow_stream_param_t param = librflow_stream_param_create();
    if (!cb || !param) {
        qWarning() << "[RFlowStream] stream callback/param allocation failed";
        if (param) {
            librflow_stream_param_destroy(param);
        }
        if (cb) {
            librflow_stream_cb_destroy(cb);
        }
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
        err = librflow_stream_param_set_preferred_codec(param, RFLOW_CODEC_H264);
    }
    if (err == RFLOW_OK) {
        err = librflow_open_stream(m_streamIndex, param, cb, &m_streamHandle);
    }

    librflow_stream_param_destroy(param);
    librflow_stream_cb_destroy(cb);
    if (err == RFLOW_OK) {
        qInfo().noquote() << QStringLiteral("[RFlowStream] librflow_open_stream submitted handle=%1")
                                 .arg(reinterpret_cast<quintptr>(m_streamHandle));
        m_statsPollTimer.start();
    } else {
        qWarning().noquote() << QStringLiteral("[RFlowStream] librflow_open_stream failed err=%1").arg(err);
    }
    return err;
}

void WebRTCReceiverClient::closeStream()
{
    if (!m_streamHandle) {
        qInfo() << "[RFlowStream] closeStream skipped: no active handle";
        return;
    }
    qInfo().noquote() << QStringLiteral("[RFlowStream] closeStream handle=%1")
                             .arg(reinterpret_cast<quintptr>(m_streamHandle));
    librflow_close_stream(m_streamHandle);
    m_streamHandle = nullptr;
}

void WebRTCReceiverClient::cleanupSdk()
{
    if (m_connected) {
        qInfo() << "[RFlowConnect] disconnect sdk";
        librflow_disconnect();
        m_connected = false;
    }
    if (m_sdkInitialized) {
        qInfo() << "[RFlowInit] uninit sdk";
        librflow_uninit();
        m_sdkInitialized = false;
    }
}

void WebRTCReceiverClient::handleConnectState(rflow_connect_state_t state, rflow_err_t reason)
{
    qInfo().noquote() << QStringLiteral("[RFlowConnect] state=%1 reason=%2 last_error=\"%3\"")
                             .arg(connectStateToString(state))
                             .arg(QStringLiteral("%1(%2)")
                                      .arg(errorToString(reason))
                                      .arg(reason))
                             .arg(QString::fromUtf8(librflow_get_last_error() ? librflow_get_last_error() : ""));
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
    qInfo().noquote() << QStringLiteral("[RFlowStream] state=%1 reason=%2 last_error=\"%3\"")
                             .arg(streamStateToString(state))
                             .arg(QStringLiteral("%1(%2)")
                                      .arg(errorToString(reason))
                                      .arg(reason))
                             .arg(QString::fromUtf8(librflow_get_last_error() ? librflow_get_last_error() : ""));
    switch (state) {
    case RFLOW_STREAM_OPENING:
        m_hasReceivedVideoFrame = false;
        m_hasReceivedVideoCallback = false;
        m_hasReportedRendererMissing = false;
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
        m_hasReceivedVideoFrame = false;
        Q_EMIT statusChanged(QStringLiteral("流已关闭"));
        break;
    case RFLOW_STREAM_FAILED:
        m_streamHandle = nullptr;
        m_hasReceivedVideoFrame = false;
        Q_EMIT statusChanged(formatSdkError(QStringLiteral("流失败"), reason));
        break;
    case RFLOW_STREAM_IDLE:
    default:
        break;
    }
}

void WebRTCReceiverClient::handleStreamStats(librflow_stream_stats_t stats)
{
    if (!stats) {
        return;
    }

    const uint32_t durationMs = librflow_stream_stats_get_duration_ms(stats);
    const uint64_t inboundBytes = librflow_stream_stats_get_in_bound_bytes(stats);
    const uint64_t inboundPkts = librflow_stream_stats_get_in_bound_pkts(stats);
    const uint32_t lostPkts = librflow_stream_stats_get_lost_pkts(stats);
    const uint32_t bitrateKbps = librflow_stream_stats_get_bitrate_kbps(stats);
    const uint32_t rttMs = librflow_stream_stats_get_rtt_ms(stats);
    const uint32_t fps = librflow_stream_stats_get_fps(stats);
    const uint32_t jitterMs = librflow_stream_stats_get_jitter_ms(stats);
    const uint32_t freezeCount = librflow_stream_stats_get_freeze_count(stats);
    const uint32_t decodeFailCount = librflow_stream_stats_get_decode_fail_count(stats);

    m_rttCurrentMs = static_cast<double>(rttMs);
    m_rttAvgMs = m_rttCurrentMs;
    m_jitterBufferMs = static_cast<double>(jitterMs);
    m_hasConnectionStats = true;

    qInfo().noquote()
        << QStringLiteral("[RFlowStats] resolution=%1x%2 fps=%3 bitrate_kbps=%4 webrtc_buffer_ms=%5 "
                          "rtt_ms=%6 lost_pkts=%7 inbound_bytes=%8 inbound_pkts=%9 freeze_count=%10 "
                          "decode_fail_count=%11 duration_ms=%12")
               .arg(m_lastFrameWidth)
               .arg(m_lastFrameHeight)
               .arg(fps)
               .arg(bitrateKbps)
               .arg(jitterMs)
               .arg(rttMs)
               .arg(lostPkts)
               .arg(inboundBytes)
               .arg(inboundPkts)
               .arg(freezeCount)
               .arg(decodeFailCount)
               .arg(durationMs);

    Q_EMIT connectionStatsChanged();
}

void WebRTCReceiverClient::pollStreamStats()
{
    if (!m_streamHandle) {
        return;
    }

    librflow_stream_stats_t stats = nullptr;
    const rflow_err_t err = librflow_stream_get_stats(m_streamHandle, &stats);
    if (err != RFLOW_OK) {
        qWarning().noquote() << QStringLiteral("[RFlowStats] get_stats failed err=%1(%2)")
                                    .arg(errorToString(err))
                                    .arg(err);
        return;
    }

    handleStreamStats(stats);
    if (stats) {
        librflow_stream_stats_release(stats);
    }
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
        const rflow_video_frame_backend_t backend = librflow_video_frame_get_backend(frame);
        const rflow_native_handle_type_t nativeHandleType = librflow_video_frame_get_native_handle_type(frame);
        const rflow_codec_t codec = librflow_video_frame_get_codec(frame);
        const uint32_t width = librflow_video_frame_get_width(frame);
        const uint32_t height = librflow_video_frame_get_height(frame);
        const quint32 frameId = librflow_video_frame_get_seq(frame);
        const uint32_t planeCount = librflow_video_frame_get_plane_count(frame);
        demo::latency_trace::recordUiDispatch(frameId);

        const bool isCpuPlanarBackend =
            backend == RFLOW_VIDEO_FRAME_BACKEND_UNKNOWN || backend == RFLOW_VIDEO_FRAME_BACKEND_CPU_PLANAR;
        const bool supportedCpuFrame =
            ((codec == RFLOW_CODEC_I420 && planeCount >= 3) || (codec == RFLOW_CODEC_NV12 && planeCount >= 2)) &&
            width > 0 && height > 0;
        const bool supportedFrame =
            (isCpuPlanarBackend && supportedCpuFrame) ||
            (backend == RFLOW_VIDEO_FRAME_BACKEND_GPU_EXTERNAL) ||
            (backend == RFLOW_VIDEO_FRAME_BACKEND_HARDWARE_BUFFER);

        if (supportedFrame) {
            if (m_videoSink) {
                if (!m_hasReceivedVideoFrame) {
                    m_hasReceivedVideoFrame = true;
                    Q_EMIT statusChanged(QStringLiteral("已收到视频流"));
                }
                ++m_deliveredFrames;
                if (m_deliveredFrames <= 3 || (m_deliveredFrames % 600) == 0) {
                    qInfo().noquote()
                        << QStringLiteral("[RenderQueue] delivered=%1 overwritten=%2 frame=%3 backend=%4 handle=%5 codec=%6")
                               .arg(m_deliveredFrames)
                               .arg(m_overwrittenPendingFrames)
                               .arg(frameId)
                               .arg(backendToString(backend))
                               .arg(nativeHandleToString(nativeHandleType))
                               .arg(codec);
                }
                m_videoSink->presentFrame(frame);
                frame = nullptr;
            } else if (!m_hasReportedRendererMissing) {
                m_hasReportedRendererMissing = true;
                qWarning().noquote() << QStringLiteral("[RenderQueue] video sink missing while frame=%1").arg(frameId);
                Q_EMIT statusChanged(QStringLiteral("已收到视频流，但渲染器未就绪"));
            }
        } else {
            qWarning().noquote()
                << QStringLiteral("Unsupported frame from libRoboFlow: backend=%1 handle=%2 codec=%3 planes=%4 size=%5x%6")
                       .arg(backendToString(backend))
                       .arg(nativeHandleToString(nativeHandleType))
                       .arg(codec)
                       .arg(planeCount)
                       .arg(width)
                       .arg(height);
            if (!m_hasReceivedVideoFrame) {
                Q_EMIT statusChanged(QStringLiteral("已收到视频流，但当前帧格式不支持渲染"));
            }
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

void WebRTCReceiverClient::emitStatus(const QString &status)
{
    qInfo().noquote() << QStringLiteral("[RFlowStatus] %1").arg(status);
    Q_EMIT statusChanged(status);
}

QString WebRTCReceiverClient::formatSdkError(const QString &prefix, rflow_err_t err) const
{
    QString message = QStringLiteral("%1: %2(%3)").arg(prefix, errorToString(err)).arg(err);
    const char *lastError = librflow_get_last_error();
    if (lastError && lastError[0] != '\0') {
        message += QStringLiteral(" - %1").arg(QString::fromUtf8(lastError));
    }
    return message;
}
