 #include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtQml>

#include "webrtc_receiver_client.h"
#include "webrtc_video_renderer.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    // 手动注册 WebRTCVideoRenderer，确保 QML 可正常 import LibwebRtcDemo 1.0
    qmlRegisterType<WebRTCVideoRenderer>("LibwebRtcDemo", 1, 0, "WebRTCVideoRenderer");

    WebRTCReceiverClient receiverClient;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("receiverClient", &receiverClient);

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



