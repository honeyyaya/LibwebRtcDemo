#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtWebView/qtwebviewfunctions.h>
#include <QtGlobal>

#include "webrtc_webview_receiver.h"

// QSG 默认「threaded」：场景图在单独线程，QQuickFramebufferObject 常见再叠 0～1 帧到合成。
// basic：主线程顺序 sync→render，通常可缩短「update→屏上」相对 threaded 的隐藏帧（大 UI 上可能更吃主线程，可用环境变量改回）
static void initSceneGraphForLowerVideoLatency()
{
    if (!qgetenv("QSG_RENDER_LOOP").isEmpty()) {
        return;  // 用户已显式选择，不覆盖
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
            if (!obj && url == objUrl)
                QCoreApplication::exit(-1);
        },
        Qt::QueuedConnection);
    engine.load(url);

    return app.exec();
}
