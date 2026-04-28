#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtGlobal>
#include <QtWebView/qtwebviewfunctions.h>

#include "webrtc_webview_receiver.h"

static void initSceneGraphForLowerVideoLatency()
{
    if (!qgetenv("QSG_RENDER_LOOP").isEmpty()) {
        return;
    }
    qputenv("QSG_RENDER_LOOP", "basic");
}

int main(int argc, char *argv[])
{
    initSceneGraphForLowerVideoLatency();
    QGuiApplication app(argc, argv);
    QtWebView::initialize();

    WebRtcWebViewReceiver receiver;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("webrtcReceiver", &receiver);

    const QUrl url(QStringLiteral("qrc:/LibwebRtcDemo/main.qml"));
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreated,
        &app,
        [url](QObject *obj, const QUrl &objUrl) {
            if (!obj && url == objUrl) {
                QCoreApplication::exit(-1);
            }
        },
        Qt::QueuedConnection);
    engine.load(url);

    return app.exec();
}
