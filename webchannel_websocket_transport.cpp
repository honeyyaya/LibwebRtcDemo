#include "webchannel_websocket_transport.h"

#include <QAbstractSocket>
#include <QJsonDocument>
#include <QJsonParseError>

WebChannelWebSocketTransport::WebChannelWebSocketTransport(QWebSocket *socket, QObject *parent)
    : QWebChannelAbstractTransport(parent), m_socket(socket) {
  Q_ASSERT(m_socket);
  connect(m_socket, &QWebSocket::textMessageReceived, this,
          [this](const QString &message) {
            QJsonParseError err{};
            const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &err);
            if (err.error == QJsonParseError::NoError && doc.isObject())
              Q_EMIT messageReceived(doc.object(), this);
          });
}

void WebChannelWebSocketTransport::sendMessage(const QJsonObject &message) {
  if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
    return;
  m_socket->sendTextMessage(
      QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
}
