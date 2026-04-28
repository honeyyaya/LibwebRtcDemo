#include "webrtc_webview_receiver.h"

#include "webchannel_websocket_transport.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QMetaObject>
#include <QStandardPaths>
#include <QWebChannel>
#include <QWebSocket>
#include <QWebSocketServer>

#ifndef DEFAULT_SIGNALING_ADDR
#define DEFAULT_SIGNALING_ADDR "192.168.3.20:8765?stream_id=demo_device:0"
#endif

// --- WebViewSignalingBridge ---

WebViewSignalingBridge::WebViewSignalingBridge(WebRtcWebViewReceiver *owner)
    : QObject(owner), m_owner(owner) {
  setObjectName(QStringLiteral("signalingBridge"));
}

void WebViewSignalingBridge::sendAnswer(const QString &sdp) {
  if (!m_owner)
    return;
  QMetaObject::invokeMethod(m_owner, "onBridgeAnswer", Qt::QueuedConnection, Q_ARG(QString, sdp));
}

void WebViewSignalingBridge::sendIceCandidate(const QString &mid, int mlineIndex,
                                              const QString &candidate) {
  if (!m_owner)
    return;
  QMetaObject::invokeMethod(m_owner, "onBridgeIce", Qt::QueuedConnection, Q_ARG(QString, mid),
                            Q_ARG(int, mlineIndex), Q_ARG(QString, candidate));
}

void WebViewSignalingBridge::jsLog(const QString &msg) {
  qDebug().noquote() << "[WebView-JS]" << msg;
}

void WebViewSignalingBridge::reportWebRtcTiming(const QString &jsonLine) {
  qDebug().noquote() << "[WebRTC-TIMING]" << jsonLine;
}

void WebViewSignalingBridge::reportPlaybackStats(const QString &jsonLine) {
  qDebug().noquote() << "[WebRTC-STATS]" << jsonLine;
}

void WebViewSignalingBridge::notifyPeerState(const QString &state) {
  if (!m_owner)
    return;
  QMetaObject::invokeMethod(m_owner, "onBridgePeerState", Qt::QueuedConnection, Q_ARG(QString, state));
}

// --- WebRtcWebViewReceiver ---

WebRtcWebViewReceiver::WebRtcWebViewReceiver(QObject *parent) : QObject(parent) {
  m_bridge = new WebViewSignalingBridge(this);
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
  if (!installWebBundleToAppData())
    qWarning() << "[WebView] 未能把页面从 qrc 解压到本地，WebView 可能无法打开";
#endif
  ensureWebChannelServer();
  Q_EMIT pageUrlChanged();
}

WebRtcWebViewReceiver::~WebRtcWebViewReceiver() {
  disconnect();
  closeWebClient();
  if (m_wsServer) {
    m_wsServer->close();
    m_wsServer->deleteLater();
    m_wsServer = nullptr;
  }
}

bool WebRtcWebViewReceiver::ensureWebChannelServer() {
  if (m_wsServer)
    return m_channelPort != 0;
  m_wsServer = new QWebSocketServer(QStringLiteral("webrtc-qwebchannel"), QWebSocketServer::NonSecureMode,
                                    this);
  if (!m_wsServer->listen(QHostAddress::LocalHost, 0)) {
    qWarning() << "QWebSocketServer listen failed:" << m_wsServer->errorString();
    m_wsServer->deleteLater();
    m_wsServer = nullptr;
    m_channelPort = 0;
    return false;
  }
  m_channelPort = m_wsServer->serverPort();
  connect(m_wsServer, &QWebSocketServer::newConnection, this, &WebRtcWebViewReceiver::onWebSocketClient);
  Q_EMIT webChannelPortChanged();
  Q_EMIT pageUrlChanged();
  return true;
}

void WebRtcWebViewReceiver::closeWebClient() {
  if (m_channel) {
    delete m_channel;
    m_channel = nullptr;
  }
  if (m_clientSocket) {
    m_clientSocket->disconnect(this);
    m_clientSocket->abort();
    m_clientSocket->deleteLater();
    m_clientSocket = nullptr;
  }
}

void WebRtcWebViewReceiver::onWebSocketClient() {
  if (!m_wsServer)
    return;
  QWebSocket *sock = m_wsServer->nextPendingConnection();
  if (!sock)
    return;
  closeWebClient();
  m_clientSocket = sock;
  connect(sock, &QWebSocket::disconnected, this, &WebRtcWebViewReceiver::closeWebClient);

  auto *transport = new WebChannelWebSocketTransport(sock, sock);
  m_channel = new QWebChannel(this);
  m_channel->registerObject(QStringLiteral("signalingBridge"), m_bridge);
  m_channel->connectTo(transport);
}

bool WebRtcWebViewReceiver::installWebBundleToAppData() {
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
  const QString base =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/webrtc_webview/");
  if (!QDir().mkpath(base)) {
    qWarning() << "[WebView] mkpath failed:" << base;
    return false;
  }
  static const struct {
    const char *qrc;
    const char *name;
  } kFiles[] = {
      { ":/LibwebRtcDemo/webreceiver.html", "webreceiver.html" },
      { ":/LibwebRtcDemo/qwebchannel.js", "qwebchannel.js" },
  };
  for (const auto &f : kFiles) {
    const QString dest = base + QString::fromLatin1(f.name);
    QFile::remove(dest);
    if (!QFile::copy(QString::fromLatin1(f.qrc), dest)) {
      qWarning() << "[WebView] QFile::copy failed" << f.qrc << "->" << dest;
      return false;
    }
    QFile df(dest);
    df.setPermissions(QFile::ReadUser | QFile::WriteUser | QFile::ReadOwner | QFile::WriteOwner);
  }
  m_localWebReceiverPath = base + QStringLiteral("webreceiver.html");
  qDebug().noquote() << "[WebView] 本地页面:" << m_localWebReceiverPath;
  return true;
#else
  return true;
#endif
}

QUrl WebRtcWebViewReceiver::pageUrl() const {
  QUrl u;
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
  if (!m_localWebReceiverPath.isEmpty())
    u = QUrl::fromLocalFile(m_localWebReceiverPath);
  else
    u = QUrl(QStringLiteral("qrc:/LibwebRtcDemo/webreceiver.html"));
#else
  u = QUrl(QStringLiteral("qrc:/LibwebRtcDemo/webreceiver.html"));
#endif
  if (m_channelPort != 0)
    u.setQuery(QStringLiteral("ws_port=%1").arg(m_channelPort));
  return u;
}

void WebRtcWebViewReceiver::setStatus(const QString &s) {
  if (m_status == s)
    return;
  m_status = s;
  Q_EMIT statusChanged();
}

void WebRtcWebViewReceiver::onBridgeAnswer(const QString &sdp) {
  if (!m_signaling || sdp.isEmpty())
    return;
  m_signaling->SendAnswer(sdp.toStdString());
  setStatus(QStringLiteral("页面: Answer 已发出，ICE 交换中…"));
}

void WebRtcWebViewReceiver::onBridgeIce(const QString &mid, int mlineIndex, const QString &candidate) {
  if (!m_signaling || candidate.isEmpty())
    return;
  m_signaling->SendIceCandidate(mid.toStdString(), mlineIndex, candidate.toStdString());
}

void WebRtcWebViewReceiver::onBridgePeerState(const QString &state) {
  if (state == QStringLiteral("connected"))
    setStatus(QStringLiteral("页面: 已连接（系统 WebView / Chromium 解码）"));
  else if (state == QStringLiteral("failed") || state == QStringLiteral("disconnected"))
    setStatus(QStringLiteral("页面: 连接中断 (%1)").arg(state));
}

void WebRtcWebViewReceiver::wireSignalingCallbacks() {
  if (!m_signaling)
    return;
  m_signaling->SetOnOffer([this](const std::string & /*type*/, const std::string &sdp) {
    const QString qsdp = QString::fromStdString(sdp);
    QMetaObject::invokeMethod(
        this,
        [this, qsdp]() {
          Q_EMIT m_bridge->offerReceived(qsdp);
          setStatus(QStringLiteral("页面: 已收到 Offer 并下发"));
        },
        Qt::QueuedConnection);
  });
  m_signaling->SetOnIce([this](const std::string &mid, int mline_index,
                                const std::string &candidate) {
    const QString qmid = QString::fromStdString(mid);
    const QString qcand = QString::fromStdString(candidate);
    QMetaObject::invokeMethod(
        this,
        [this, qmid, mline_index, qcand]() {
          Q_EMIT m_bridge->iceCandidateReceived(qmid, mline_index, qcand);
        },
        Qt::QueuedConnection);
  });
  m_signaling->SetOnError([this](const std::string &err) {
    const QString qerr = QString::fromStdString(err);
    QMetaObject::invokeMethod(
        this,
        [this, qerr]() { setStatus(QStringLiteral("信令错误: %1").arg(qerr)); },
        Qt::QueuedConnection);
  });
}

void WebRtcWebViewReceiver::requestPermissionAndConnect(const QString &addr) {
  disconnect();
  ensureWebChannelServer();

  std::string serverAddr = addr.isEmpty() ? DEFAULT_SIGNALING_ADDR : addr.toStdString();
  m_signaling = std::make_unique<webrtc_demo::SignalingClient>(serverAddr, "subscriber");
  wireSignalingCallbacks();

  setStatus(QStringLiteral("正在连接信令…"));
  if (!m_signaling->Start()) {
    setStatus(QStringLiteral("连接信令失败"));
    m_signaling.reset();
    return;
  }
  setStatus(QStringLiteral("信令已连接 (subscriber)，等待 Offer…"));
}

void WebRtcWebViewReceiver::disconnect() {
  if (m_bridge)
    Q_EMIT m_bridge->signalingClosed();
  if (m_signaling) {
    m_signaling->Stop();
    m_signaling.reset();
  }
  setStatus(QStringLiteral("未连接"));
}
