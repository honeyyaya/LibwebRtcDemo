#include "webrtcwrapper.h"
#include <QDebug>

WebRTCWrapper::WebRTCWrapper(QObject *parent)
    : QObject(parent)
{
}

WebRTCWrapper::~WebRTCWrapper()
{
    cleanup();
}

bool WebRTCWrapper::initialize()
{
    if (m_initialized) {
        appendLog("WebRTC already initialized");
        return true;
    }

    appendLog("Calling LibWebRTC::Initialize()...");

    bool ok = libwebrtc::LibWebRTC::Initialize();
    appendLog(QString("Initialize() = %1").arg(ok ? "true" : "false"));

    if (!ok) {
        appendLog("Initialize FAILED");
        return false;
    }

    m_factory = libwebrtc::LibWebRTC::CreateRTCPeerConnectionFactory();
    bool factoryOk = m_factory.get() != nullptr;
    appendLog(QString("PeerConnectionFactory created = %1").arg(factoryOk ? "yes" : "no"));

    if (!factoryOk) {
        appendLog("CreateRTCPeerConnectionFactory FAILED");
        libwebrtc::LibWebRTC::Terminate();
        return false;
    }

    m_initialized = true;
    appendLog("WebRTC init OK!");
    Q_EMIT initializedChanged();
    return true;
}

void WebRTCWrapper::cleanup()
{
    if (!m_initialized)
        return;

    m_factory = nullptr;
#ifdef Q_OS_ANDROID
    // Android 上 Terminate() 与 worker_thread 存在竞态，导致 webrtc_voice_engine Check failed: adm_
    appendLog("WebRTC cleanup (Android: skip Terminate to avoid adm_ race)");
#else
    libwebrtc::LibWebRTC::Terminate();
    appendLog("WebRTC terminated");
#endif
    m_initialized = false;
    Q_EMIT initializedChanged();
}

bool WebRTCWrapper::isInitialized() const
{
    return m_initialized;
}

QString WebRTCWrapper::getVersion() const
{
    return QStringLiteral("LibWebRTC Demo v1.0");
}

QString WebRTCWrapper::statusLog() const
{
    return m_statusLog;
}

void WebRTCWrapper::appendLog(const QString &msg)
{
    qDebug() << "[WebRTC]" << msg;
    m_statusLog += msg + "\n";
    Q_EMIT statusLogChanged();
}
