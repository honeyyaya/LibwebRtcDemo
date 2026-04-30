#include <QGuiApplication>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtQml>

#include "webrtc_receiver_client.h"
#include "webrtc_video_renderer.h"

namespace {

QString GraphicsApiName(QSGRendererInterface::GraphicsApi api)
{
    if (api == QSGRendererInterface::Unknown)
        return QStringLiteral("Unknown");
    if (api == QSGRendererInterface::Software)
        return QStringLiteral("Software");
    if (api == QSGRendererInterface::OpenVG)
        return QStringLiteral("OpenVG");
    if (api == QSGRendererInterface::OpenGL)
        return QStringLiteral("OpenGL/OpenGLRhi");
    if (api == QSGRendererInterface::Direct3D11)
        return QStringLiteral("Direct3D11/Direct3D11Rhi");
    if (api == QSGRendererInterface::Vulkan)
        return QStringLiteral("Vulkan/VulkanRhi");
    if (api == QSGRendererInterface::Metal)
        return QStringLiteral("Metal/MetalRhi");
    if (api == QSGRendererInterface::Null)
        return QStringLiteral("Null/NullRhi");
    if (api == QSGRendererInterface::Direct3D12)
        return QStringLiteral("Direct3D12");

    return QStringLiteral("Other(%1)").arg(static_cast<int>(api));
}

void EnableSceneGraphGeneralLogging()
{
    QByteArray rules = qgetenv("QT_LOGGING_RULES");
    const QByteArray rule = "qt.scenegraph.general=true";
    if (!rules.contains(rule)) {
        if (!rules.isEmpty() && !rules.endsWith(';'))
            rules.append(';');
        rules.append(rule);
        qputenv("QT_LOGGING_RULES", rules);
    }
}

}

int main(int argc, char *argv[])
{
    EnableSceneGraphGeneralLogging();
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    qInfo().noquote()
        << QString("[QtSG] requested render_loop=%1 vsync=%2 logging_rules=%3 graphics_api=%4")
               .arg(QString::fromUtf8(qEnvironmentVariableIsSet("QSG_RENDER_LOOP")
                                          ? qgetenv("QSG_RENDER_LOOP")
                                          : QByteArrayLiteral("<default>")))
               .arg(QString::fromUtf8(qEnvironmentVariableIsSet("QSG_VSYNC_ENABLED")
                                          ? qgetenv("QSG_VSYNC_ENABLED")
                                          : QByteArrayLiteral("<default>")))
               .arg(QString::fromUtf8(qgetenv("QT_LOGGING_RULES")))
               .arg(GraphicsApiName(QSGRendererInterface::OpenGL));
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
            if (auto *window = qobject_cast<QQuickWindow *>(obj)) {
                QObject::connect(
                    window,
                    &QQuickWindow::sceneGraphInitialized,
                    window,
                    [window]() {
                        qInfo().noquote()
                            << QString("[QtSG] sceneGraphInitialized actual_api=%1")
                                   .arg(GraphicsApiName(window->rendererInterface()->graphicsApi()));
                    },
                    Qt::DirectConnection);
            }
        },
        Qt::QueuedConnection);
    engine.load(url);

    return app.exec();
}



