#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "webrtcwrapper.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    WebRTCWrapper webrtc;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("webrtc", &webrtc);

    const QUrl url(QStringLiteral("qrc:/LibwebRtcDemo/main.qml"));
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreated,
        &app,
        [url](QObject *obj, const QUrl &objUrl) {
            if (!obj && url == objUrl)
                QCoreApplication::exit(-1);
        },
        Qt::QueuedConnection);
    engine.load(url);

    return app.exec();
}
