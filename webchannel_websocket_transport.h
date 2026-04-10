#ifndef WEBCHANNEL_WEBSOCKET_TRANSPORT_H
#define WEBCHANNEL_WEBSOCKET_TRANSPORT_H

#include <QJsonObject>
#include <QWebChannelAbstractTransport>
#include <QWebSocket>

/// 将 QWebChannel 帧走 WebSocket 文本帧，供 Qt WebView（无内置 WebChannel 传输）使用。
class WebChannelWebSocketTransport : public QWebChannelAbstractTransport {
  Q_OBJECT
 public:
  explicit WebChannelWebSocketTransport(QWebSocket *socket, QObject *parent = nullptr);

  void sendMessage(const QJsonObject &message) override;

 private:
  QWebSocket *m_socket = nullptr;
};

#endif
