#include "webrtc_receiver_client.h"

#include "latency_trace.h"
#include "webrtc_video_renderer.h"

#include <QDebug>
#include <QMetaObject>
#include <QPointer>
#include <QMutexLocker>

#include <cstdio>

#ifdef Q_OS_ANDROID
#include <QJniObject>
#endif

#ifndef DEFAULT_SIGNALING_ADDR
#define DEFAULT_SIGNALING_ADDR "192.168.3.20:8765"
#endif

static const char *rflowLogLevelTag(rflow_log_level_t level)
{
    switch (level) {
    case RFLOW_LOG_TRACE:
        return "TRACE";
    case RFLOW_LOG_DEBUG:
        return "DEBUG";
    case RFLOW_LOG_INFO:
        return "INFO";
    case RFLOW_LOG_WARN:
        return "WARN";
    case RFLOW_LOG_ERROR:
        return "ERROR";
    case RFLOW_LOG_FATAL:
        return "FATAL";
    default:
        return "?";
    }
}

extern "C" {

static void rflowConsoleLogCallback(rflow_log_level_t level, const char *msg, void *userdata)
{
    /* 必须使用完整指针 msg：*msg 只是首字符 char，若以 '[' 开头会永远只打印 "[" */
    const char *text = msg ? msg : "";
    Q_UNUSED(userdata);
#if defined(Q_OS_ANDROID)
    const QString payload = QString::fromUtf8(text);
    const QString prefix =
        QStringLiteral("[RoboFlow/%1]").arg(QString::fromLatin1(rflowLogLevelTag(level)));
    switch (level) {
    case RFLOW_LOG_WARN:
        qWarning().noquote() << prefix << payload;
        break;
    case RFLOW_LOG_ERROR:
    case RFLOW_LOG_FATAL:
        qCritical().noquote() << prefix << payload;
        break;
    case RFLOW_LOG_TRACE:
    case RFLOW_LOG_DEBUG:
        qDebug().noquote() << prefix << payload;
        break;
    default:
        qInfo().noquote() << prefix << payload;
        break;
    }
#else
    std::fprintf(stderr, "[RFlow/%s] %s\n", rflowLogLevelTag(level), text);
    std::fflush(stderr);
#endif
}

} // extern "C"

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
    m_statsPollTimer.setInterval(1000);
    connect(&m_statsPollTimer, &QTimer::timeout, this, &WebRTCReceiverClient::pollStreamStats);
}

WebRTCReceiverClient::~WebRTCReceiverClient()
{
    disconnect();
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

QString WebRTCReceiverClient::overlayTelemetryText() const
{
    QString t;
    const uint32_t w = m_lastFrameWidth.load(std::memory_order_relaxed);
    const uint32_t h = m_lastFrameHeight.load(std::memory_order_relaxed);
    if (w > 0 && h > 0) {
        t += QStringLiteral("分辨率：%1×%2\n").arg(w).arg(h);
    } else {
        t += QStringLiteral("分辨率：—\n");
    }

    if (m_hasConnectionStats) {
        t += QStringLiteral("帧率：%1 fps\n").arg(m_statsFps, 0, 'f', 1);
        if (m_statsBitrateKbps >= 1000.0) {
            t += QStringLiteral("码率：%1 Mbps\n").arg(m_statsBitrateKbps / 1000.0, 0, 'f', 2);
        } else {
            t += QStringLiteral("码率：%1 Kbps\n").arg(m_statsBitrateKbps, 0, 'f', 0);
        }
        t += QStringLiteral("抖动缓存：%1 ms\n").arg(m_statsJitterMs, 0, 'f', 1);
        t += QStringLiteral("RTT：%1 ms\n").arg(m_rttCurrentMs, 0, 'f', 1);

        const quint64 denom = static_cast<quint64>(m_statsInboundPkts) + static_cast<quint64>(m_statsLostPkts);
        double lossPct = 0.0;
        if (denom > 0) {
            lossPct = (100.0 * static_cast<double>(m_statsLostPkts)) / static_cast<double>(denom);
        }
        t += QStringLiteral("丢包率：%1%  （丢失 %2 / 收包 %3）\n")
                 .arg(lossPct, 0, 'f', 2)
                 .arg(m_statsLostPkts)
                 .arg(m_statsInboundPkts);
        t += QStringLiteral("卡顿帧：%1  解码失败：%2\n").arg(m_statsFreezeCount).arg(m_statsDecodeFailCount);
        t += QStringLiteral("统计时长：%1 s\n").arg(QString::number(m_statsDurationMs / 1000.0, 'f', 1));
    } else {
        t += QStringLiteral("帧率：—\n码率：—\n抖动缓存：—\nRTT：—\n丢包率：—\n卡顿／解码失败：—\n统计时长：—\n");
    }

    return t.trimmed();
}

void WebRTCReceiverClient::disconnect()
{
    m_statsPollTimer.stop();
    closeStream();
    cleanupSdk();
    resetConnectionStats();
    m_droppedUnsupportedFrames.store(0, std::memory_order_relaxed);
    m_droppedNoSinkFrames.store(0, std::memory_order_relaxed);
    m_deliveredFrames.store(0, std::memory_order_relaxed);
    m_hasReceivedVideoFrame.store(false, std::memory_order_relaxed);
    m_hasReceivedVideoCallback.store(false, std::memory_order_relaxed);
    m_hasReportedRendererMissing.store(false, std::memory_order_relaxed);
    m_lastFrameWidth.store(0, std::memory_order_relaxed);
    m_lastFrameHeight.store(0, std::memory_order_relaxed);
    demo::latency_trace::reset();

    if (auto *sink = m_videoSink.load(std::memory_order_acquire)) {
        sink->clearVideoTrack();
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
    /// 切换 sink 必须发生在 SDK 流尚未开启 / 已停止的时段，否则 SDK 线程可能 in-flight 调用旧 sink。
    /// 当前用法只在 QML Component.onCompleted 一次性绑定，满足该前提。
    if (auto *oldSink = m_videoSink.load(std::memory_order_acquire)) {
        oldSink->clearVideoTrack();
    }
    m_videoRenderer = sink;
    auto *newSink = qobject_cast<VideoFrameSink *>(sink);
    m_videoSink.store(newSink, std::memory_order_release);
    m_hasReportedRendererMissing.store(false, std::memory_order_relaxed);
    qInfo().noquote() << QStringLiteral("[RenderQueue] video sink assigned=%1")
                             .arg(newSink ? QStringLiteral("true") : QStringLiteral("false"));
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
    /// 在 SDK 线程内：retain → 帧格式校验 → 直接 presentFrame 到 sink。
    /// 与 rtc_demo_new 一致，去掉 receiver 中间 mailbox + Qt::QueuedConnection hop，
    /// 让帧最早可在「下一次 vsync 前的 sync 阶段」被渲染线程拉走。
    auto *self = static_cast<WebRTCReceiverClient *>(userdata);
    if (!self || !frame) {
        return;
    }

    const quint32 frameId = librflow_video_frame_get_seq(frame);
    const uint32_t fw = librflow_video_frame_get_width(frame);
    const uint32_t fh = librflow_video_frame_get_height(frame);
    self->m_lastFrameWidth.store(fw, std::memory_order_relaxed);
    self->m_lastFrameHeight.store(fh, std::memory_order_relaxed);
    demo::latency_trace::recordSdkCallback(frameId,
                                           librflow_video_frame_get_pts_ms(frame),
                                           librflow_video_frame_get_utc_ms(frame),
                                           fw,
                                           fh,
                                           librflow_video_frame_get_data_size(frame));

    if (!self->m_hasReceivedVideoCallback.exchange(true, std::memory_order_acq_rel)) {
        qInfo().noquote() << QStringLiteral("[RenderQueue] first video callback frame=%1").arg(frameId);
        QPointer<WebRTCReceiverClient> guard(self);
        QMetaObject::invokeMethod(
            self,
            [guard]() {
                if (guard && !guard->m_hasReceivedVideoFrame.load(std::memory_order_acquire)) {
                    Q_EMIT guard->statusChanged(QStringLiteral("已收到视频回调，等待渲染"));
                }
            },
            Qt::QueuedConnection);
    }

    /// 帧格式校验：与原 deliverPendingFrame 一致，只放支持的帧给 sink。
    const rflow_video_frame_backend_t backend = librflow_video_frame_get_backend(frame);
    const rflow_codec_t codec = librflow_video_frame_get_codec(frame);
    const uint32_t planeCount = librflow_video_frame_get_plane_count(frame);
    const bool isCpuPlanarBackend =
        backend == RFLOW_VIDEO_FRAME_BACKEND_UNKNOWN ||
        backend == RFLOW_VIDEO_FRAME_BACKEND_CPU_PLANAR;
    const bool supportedCpuFrame =
        ((codec == RFLOW_CODEC_I420 && planeCount >= 3) ||
         (codec == RFLOW_CODEC_NV12 && planeCount >= 2)) &&
        fw > 0 && fh > 0;
    const bool supportedFrame = (isCpuPlanarBackend && supportedCpuFrame) ||
                                backend == RFLOW_VIDEO_FRAME_BACKEND_GPU_EXTERNAL ||
                                backend == RFLOW_VIDEO_FRAME_BACKEND_HARDWARE_BUFFER;

    if (!supportedFrame) {
        const quint64 dropped =
            self->m_droppedUnsupportedFrames.fetch_add(1, std::memory_order_relaxed) + 1;
        if (dropped <= 3 || (dropped % 600) == 0) {
            qWarning().noquote()
                << QStringLiteral("Unsupported frame from libRoboFlow: backend=%1 codec=%2 planes=%3 size=%4x%5 dropped_total=%6")
                       .arg(backendToString(backend))
                       .arg(codec)
                       .arg(planeCount)
                       .arg(fw)
                       .arg(fh)
                       .arg(dropped);
        }
        if (!self->m_hasReceivedVideoFrame.load(std::memory_order_acquire)) {
            QPointer<WebRTCReceiverClient> guard(self);
            QMetaObject::invokeMethod(
                self,
                [guard]() {
                    if (guard) {
                        Q_EMIT guard->statusChanged(
                            QStringLiteral("已收到视频流，但当前帧格式不支持渲染"));
                    }
                },
                Qt::QueuedConnection);
        }
        return;
    }

    auto *sink = self->m_videoSink.load(std::memory_order_acquire);
    if (!sink) {
        const quint64 dropped =
            self->m_droppedNoSinkFrames.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!self->m_hasReportedRendererMissing.exchange(true, std::memory_order_acq_rel)) {
            qWarning().noquote()
                << QStringLiteral("[RenderQueue] video sink missing while frame=%1").arg(frameId);
            QPointer<WebRTCReceiverClient> guard(self);
            QMetaObject::invokeMethod(
                self,
                [guard]() {
                    if (guard) {
                        Q_EMIT guard->statusChanged(
                            QStringLiteral("已收到视频流，但渲染器未就绪"));
                    }
                },
                Qt::QueuedConnection);
        }
        if (dropped <= 3 || (dropped % 300) == 0) {
            qWarning().noquote()
                << QStringLiteral("[RenderQueue] no sink, dropped frame=%1 total=%2")
                       .arg(frameId)
                       .arg(dropped);
        }
        return;
    }

    librflow_video_frame_t retained = librflow_video_frame_retain(frame);
    if (!retained) {
        return;
    }

    if (!self->m_hasReceivedVideoFrame.exchange(true, std::memory_order_acq_rel)) {
        QPointer<WebRTCReceiverClient> guard(self);
        QMetaObject::invokeMethod(
            self,
            [guard]() {
                if (guard) {
                    Q_EMIT guard->statusChanged(QStringLiteral("已收到视频流"));
                }
            },
            Qt::QueuedConnection);
    }

    demo::latency_trace::recordUiDispatch(frameId);

    const quint64 delivered = self->m_deliveredFrames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (delivered <= 3 || (delivered % 600) == 0) {
        qInfo().noquote()
            << QStringLiteral("[RenderQueue] delivered=%1 dropped_unsupported=%2 dropped_nosink=%3 frame=%4 backend=%5 codec=%6")
                   .arg(delivered)
                   .arg(self->m_droppedUnsupportedFrames.load(std::memory_order_relaxed))
                   .arg(self->m_droppedNoSinkFrames.load(std::memory_order_relaxed))
                   .arg(frameId)
                   .arg(backendToString(backend))
                   .arg(codec);
    }

    /// presentFrame 接管 retained 这一引用，最终由 renderer 释放。
    sink->presentFrame(retained);
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
        (void) librflow_log_set_level(RFLOW_LOG_DEBUG);
        (void) librflow_log_set_callback(rflowConsoleLogCallback, this);
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
        (void) librflow_log_set_callback(nullptr, nullptr);
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
        m_hasReceivedVideoFrame.store(false, std::memory_order_relaxed);
        m_hasReceivedVideoCallback.store(false, std::memory_order_relaxed);
        m_hasReportedRendererMissing.store(false, std::memory_order_relaxed);
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
        m_hasReceivedVideoFrame.store(false, std::memory_order_relaxed);
        Q_EMIT statusChanged(QStringLiteral("流已关闭"));
        break;
    case RFLOW_STREAM_FAILED:
        m_streamHandle = nullptr;
        m_hasReceivedVideoFrame.store(false, std::memory_order_relaxed);
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
    const uint64_t inboundPkts = librflow_stream_stats_get_in_bound_pkts(stats);
    const uint32_t lostPkts = librflow_stream_stats_get_lost_pkts(stats);
    const uint32_t bitrateKbps = librflow_stream_stats_get_bitrate_kbps(stats);
    const uint32_t rttMs = librflow_stream_stats_get_rtt_ms(stats);
    const uint32_t fps = librflow_stream_stats_get_fps(stats);
    const uint32_t jitterMs = librflow_stream_stats_get_jitter_ms(stats);
    const uint32_t freezeCount = librflow_stream_stats_get_freeze_count(stats);
    const uint32_t decodeFailCount = librflow_stream_stats_get_decode_fail_count(stats);

    m_statsDurationMs = durationMs;
    m_statsInboundPkts = inboundPkts;
    m_statsLostPkts = lostPkts;
    m_statsFps = static_cast<double>(fps);
    m_statsBitrateKbps = static_cast<double>(bitrateKbps);
    m_statsJitterMs = static_cast<double>(jitterMs);
    m_statsFreezeCount = freezeCount;
    m_statsDecodeFailCount = decodeFailCount;

    m_rttCurrentMs = static_cast<double>(rttMs);
    m_hasConnectionStats = true;

    /* 诊断：让 SDK 给的原始值一目了然，方便核对码率/帧率/RTT/抖动是不是 0 */
    static quint64 sStatsLogCount = 0;
    ++sStatsLogCount;
    if (sStatsLogCount <= 3 || (sStatsLogCount % 10) == 0) {
        qInfo().noquote()
            << QStringLiteral("[StreamStatsRaw] #%1 dur=%2ms in_pkts=%3 lost=%4 "
                              "bitrate=%5kbps rtt=%6ms fps=%7 jitter(rtcp)=%8ms "
                              "freeze=%9 decodeFail=%10")
                   .arg(sStatsLogCount)
                   .arg(durationMs)
                   .arg(inboundPkts)
                   .arg(lostPkts)
                   .arg(bitrateKbps)
                   .arg(rttMs)
                   .arg(fps)
                   .arg(jitterMs)
                   .arg(freezeCount)
                   .arg(decodeFailCount);
    }

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

void WebRTCReceiverClient::resetConnectionStats()
{
    m_rttCurrentMs = 0.0;
    m_statsFps = 0.0;
    m_statsBitrateKbps = 0.0;
    m_statsJitterMs = 0.0;
    m_statsDurationMs = 0;
    m_statsInboundPkts = 0;
    m_statsLostPkts = 0;
    m_statsFreezeCount = 0;
    m_statsDecodeFailCount = 0;
    const bool had = m_hasConnectionStats;
    m_hasConnectionStats = false;
    if (had) {
        Q_EMIT connectionStatsChanged();
    }
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
