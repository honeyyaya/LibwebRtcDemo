#ifndef WEBRTC_WEBVIEW_RECEIVER_H
#define WEBRTC_WEBVIEW_RECEIVER_H

#include <QObject>
#include <QUrl>
#include <memory>
#include <string>

#include "signaling_client.h"

class QWebChannel;
class QWebSocket;
class QWebSocketServer;
class WebRtcWebViewReceiver;

/// 页面内 getUserMedia / RTCPeerConnection（系统 WebView 的 Chromium 栈）与 C++ TCP 信令桥接。
class WebViewSignalingBridge : public QObject {
  Q_OBJECT
 public:
  explicit WebViewSignalingBridge(WebRtcWebViewReceiver *owner);

 public Q_SLOTS:
  void sendAnswer(const QString &sdp);
  void sendIceCandidate(const QString &mid, int mlineIndex, const QString &candidate);
  void jsLog(const QString &msg);
  void notifyPeerState(const QString &state);
  /// JSON 字符串，各阶段耗时（ms），由页面 performance.now() 统计；logcat 过滤 WebRTC-TIMING
  void reportWebRtcTiming(const QString &jsonLine);
  /// 约每 3s：抖动缓冲、分辨率、帧率、RTT 等（getStats）；logcat 过滤 WebRTC-STATS
  void reportPlaybackStats(const QString &jsonLine);

 Q_SIGNALS:
  void offerReceived(const QString &sdp);
  void iceCandidateReceived(const QString &mid, int mlineIndex, const QString &candidate);
  void signalingClosed();

 private:
  WebRtcWebViewReceiver *m_owner = nullptr;
};

/// Qt WebView + 本地 ws://127.0.0.1 上的 QWebChannel；媒体与 WebRTC 在网页内完成（Android WebView = 谷歌 Chromium）。
class WebRtcWebViewReceiver : public QObject {
  Q_OBJECT
  Q_PROPERTY(WebViewSignalingBridge *signalingBridge READ signalingBridge CONSTANT)
  Q_PROPERTY(QString status READ status NOTIFY statusChanged)
  Q_PROPERTY(quint16 webChannelPort READ webChannelPort NOTIFY webChannelPortChanged)
  /// 桌面为 qrc；Android/iOS 为解压到 AppData 后的 file://（Qt WebView 说明：不支持从 qrc 加载）
  Q_PROPERTY(QUrl pageUrl READ pageUrl NOTIFY pageUrlChanged)
 public:
  explicit WebRtcWebViewReceiver(QObject *parent = nullptr);
  ~WebRtcWebViewReceiver() override;

  WebViewSignalingBridge *signalingBridge() const { return m_bridge; }
  QString status() const { return m_status; }
  quint16 webChannelPort() const { return m_channelPort; }

  Q_INVOKABLE void requestPermissionAndConnect(const QString &addr = QString());
  Q_INVOKABLE void disconnect();

  QUrl pageUrl() const;

 Q_SIGNALS:
  void statusChanged();
  void webChannelPortChanged();
  void pageUrlChanged();

 public Q_SLOTS:
  void onBridgeAnswer(const QString &sdp);
  void onBridgeIce(const QString &mid, int mlineIndex, const QString &candidate);
  void onBridgePeerState(const QString &state);

 private Q_SLOTS:
  void onWebSocketClient();

 private:
  void setStatus(const QString &s);
  void wireSignalingCallbacks();
  bool ensureWebChannelServer();
  void closeWebClient();
  /// Android：从 qrc 复制到可写目录，供 WebView 用 file:// 打开
  bool installWebBundleToAppData();

  WebViewSignalingBridge *m_bridge = nullptr;
  QString m_localWebReceiverPath;
  QWebSocketServer *m_wsServer = nullptr;
  quint16 m_channelPort = 0;
  QWebChannel *m_channel = nullptr;
  QWebSocket *m_clientSocket = nullptr;

  std::unique_ptr<webrtc_demo::SignalingClient> m_signaling;
  QString m_status;
};

#endif
