#ifndef WEBRTCWRAPPER_H
#define WEBRTCWRAPPER_H

#include <QObject>
#include <QString>

#include "libwebrtc.h"

class WebRTCWrapper : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool initialized READ isInitialized NOTIFY initializedChanged)
    Q_PROPERTY(QString version READ getVersion CONSTANT)
    Q_PROPERTY(QString statusLog READ statusLog NOTIFY statusLogChanged)

public:
    explicit WebRTCWrapper(QObject *parent = nullptr);
    ~WebRTCWrapper();

    Q_INVOKABLE bool initialize();
    Q_INVOKABLE void cleanup();

    bool isInitialized() const;
    QString getVersion() const;
    QString statusLog() const;

Q_SIGNALS:
    void initializedChanged();
    void statusLogChanged();

private:
    void appendLog(const QString &msg);

    bool m_initialized = false;
    libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnectionFactory> m_factory;
    QString m_statusLog;
};

#endif // WEBRTCWRAPPER_H
