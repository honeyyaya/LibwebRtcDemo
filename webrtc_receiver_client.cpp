#include "webrtc_receiver_client.h"
#include "webrtc_video_renderer.h"
#include <QJsonArray>
#include <iostream>
#include <memory>
#include <QWebSocket>
#include <QDebug>
#include <QThread>
#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QJniEnvironment>
#endif
// 默认信令地址：与 receiver.html 配套
// receiver.html 部署在 http://192.168.3.176:3000/receiver.html 时使用此地址
#ifndef DEFAULT_SIGNALING_URL
#define DEFAULT_SIGNALING_URL "ws://192.168.3.176:3000/socket.io/?EIO=4&transport=websocket"
#endif

// 验证日志宏：便于在 logcat 中过滤 "VERIFY"
#define VERIFY_LOG(tag, msg) qDebug() << "[VERIFY-" tag "]" << msg

// =============================================================================
// PeerConnectionObserver：实现 libwebrtc 的回调接口
// 类似 Qt 的信号槽，libwebrtc 通过虚函数回调通知状态和媒体
// =============================================================================
class WebRTCReceiverClient::PeerConnectionObserver
    : public libwebrtc::RTCPeerConnectionObserver
{
public:
    explicit PeerConnectionObserver(WebRTCReceiverClient *client)
        : m_client(client) {}

    void OnSignalingState(libwebrtc::RTCSignalingState state) override {
        (void)state;
    }

    void OnPeerConnectionState(libwebrtc::RTCPeerConnectionState state) override {
        if (state == libwebrtc::RTCPeerConnectionStateConnected) {
            QMetaObject::invokeMethod(m_client, [this]() {
                Q_EMIT m_client->statusChanged("WebRTC 已连接，正在接收视频");
            }, Qt::QueuedConnection);
        }
    }

    // ICE 收集状态：gathering -> complete（关键！用于等待后再发 Answer）
    void OnIceGatheringState(libwebrtc::RTCIceGatheringState state) override {
        if (state == libwebrtc::RTCIceGatheringStateComplete && m_onIceComplete) {
            auto cb = m_onIceComplete;
            m_onIceComplete = nullptr;
            QMetaObject::invokeMethod(m_client, [cb]() { cb(); }, Qt::QueuedConnection);
        }
    }

    void OnIceCandidate(libwebrtc::scoped_refptr<libwebrtc::RTCIceCandidate> candidate) override {
        (void)candidate;
    }

    // ★ 核心：收到远端视频/音频轨道时触发，对应 receiver.html 的 ontrack
    void OnTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpTransceiver> transceiver) override {
        if (transceiver->media_type() != libwebrtc::RTCMediaType::VIDEO)
            return;
        auto receiver = transceiver->receiver();
        if (!receiver) return;
        auto track = receiver->track();
        if (!track || track->kind().std_string()!= "video") return;

        auto *videoTrackPtr = static_cast<libwebrtc::RTCVideoTrack*>(track.get());
        libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack> videoTrack(videoTrackPtr);

        QMetaObject::invokeMethod(m_client, [this, videoTrack]() {
            Q_EMIT m_client->remoteVideoTrackReady(videoTrack);
        }, Qt::QueuedConnection);
    }

    void OnAddStream(libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream> stream) override {
        (void)stream;
    }
    void OnRemoveStream(libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream> stream) override {
        (void)stream;
    }
    void OnDataChannel(libwebrtc::scoped_refptr<libwebrtc::RTCDataChannel> dc) override {
        (void)dc;
    }
    void OnRenegotiationNeeded() override {}
    void  OnAddTrack(libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>> streams,
                    libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver> receiver) override {
        (void)streams;
        (void)receiver;
    }

    void OnRemoveTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver> receiver) override {
        (void)receiver;
    }
    void OnIceConnectionState(libwebrtc::RTCIceConnectionState state) override {
        if (state == libwebrtc::RTCIceConnectionStateFailed) {
            QMetaObject::invokeMethod(m_client, [this]() {
                Q_EMIT m_client->statusChanged("ICE 连接失败");
            }, Qt::QueuedConnection);
        }
    }

    std::function<void()> m_onIceComplete;

private:
    WebRTCReceiverClient *m_client;
};

// =============================================================================
// WebRTCReceiverClient 实现
// =============================================================================

WebRTCReceiverClient::WebRTCReceiverClient(QObject *parent)
    : QObject(parent)
{
    m_observer = std::make_unique<PeerConnectionObserver>(this);
    connect(this, &WebRTCReceiverClient::remoteVideoTrackReady, this, [this](libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack> track) {
        if (m_videoRenderer) {
            auto *r = qobject_cast<WebRTCVideoRenderer *>(m_videoRenderer);
            if (r) r->setVideoTrack(track);
        }
    });
#ifdef Q_OS_ANDROID
    // VERIFY-4: 系统兼容性 - 输出 Android 版本、API 等级、ABI，便于排查 libwebrtc 编译目标与运行环境不匹配
    int sdkInt = QJniObject::getStaticField<jint>("android/os/Build$VERSION", "SDK_INT");
    QJniObject releaseObj = QJniObject::getStaticObjectField("android/os/Build$VERSION", "RELEASE", "Ljava/lang/String;");
    QString release = releaseObj.isValid() ? releaseObj.toString() : "unknown";
    QJniObject abiObj = QJniObject::getStaticObjectField("android/os/Build", "CPU_ABI", "Ljava/lang/String;");
    QString abi = abiObj.isValid() ? abiObj.toString() : "unknown";
    VERIFY_LOG("4", QString("系统信息: Android %1, API %2, ABI %3").arg(release).arg(sdkInt).arg(abi));
    runVerificationDiagnostic();  // 启动时自动输出四项验证，便于 logcat 过滤 VERIFY
#endif
}

WebRTCReceiverClient::~WebRTCReceiverClient()
{
    VERIFY_LOG("5", "析构开始，执行安全关闭顺序");
    disconnect();
    if (m_peerConnection) {
        VERIFY_LOG("5", "关闭 PeerConnection 并取消观察者");
        m_peerConnection->Close();
        m_peerConnection->DeRegisterRTCPeerConnectionObserver();
        m_peerConnection = nullptr;
    }
    m_constraints = nullptr;
    // 等待 worker_thread 等后台任务完成，避免 ADM 被提前释放导致 VoiceEngine check adm_ 失败
    VERIFY_LOG("5", "等待 WebRTC worker_thread 停止(约 800ms)...");
    QThread::msleep(800);
    VERIFY_LOG("5", "释放 PeerConnectionFactory...");
    m_factory = nullptr;
    QThread::msleep(500);
#ifdef Q_OS_ANDROID
    // 方向2/4 修复：Android 上 Terminate() 与 worker_thread 存在竞态，调用会导致 webrtc_voice_engine Check failed: adm_
    // 让进程退出时由系统回收，避免析构阶段的 ADM/worker 竞争
    if (m_webrtcInitialized) {
        VERIFY_LOG("5", "[Android] 跳过 LibWebRTC::Terminate() 以避免 adm_ 竞态崩溃");
        m_webrtcInitialized = false;
    }
#else
    if (m_webrtcInitialized) {
        VERIFY_LOG("5", "调用 LibWebRTC::Terminate()");
        libwebrtc::LibWebRTC::Terminate();
        m_webrtcInitialized = false;
    }
#endif
    VERIFY_LOG("5", "析构完成");
}

void WebRTCReceiverClient::runVerificationDiagnostic()
{
    VERIFY_LOG("DIAG", "========== 逐项验证诊断开始 (排除方向5) ==========");
#ifdef Q_OS_ANDROID
    // ========== 方向1: 权限验证 ==========
    VERIFY_LOG("1", "--- 方向1: 权限 (连接前必须 RECORD_AUDIO=0) ---");
    VERIFY_LOG("1", "WebRTC ADM 需要 RECORD_AUDIO，否则 CreateADM 可能失败或析构时 Check failed: adm_");
    QJniObject activity = QJniObject::callStaticObjectMethod(
        "org/qtproject/qt/android/QtNative", "activity", "()Landroid/app/Activity;");
    if (!activity.isValid()) {
        VERIFY_LOG("1", "Activity 无效，无法校验权限 [可能导致后续 ADM 失败]");
    } else {
        QJniObject permObj = QJniObject::getStaticObjectField("android/Manifest$permission",
            "RECORD_AUDIO", "Ljava/lang/String;");
        int result = activity.callMethod<jint>("checkSelfPermission", "(Ljava/lang/String;)I",
            permObj.object<jstring>());
        VERIFY_LOG("1", QString("RECORD_AUDIO=%1 (0=已授权, -1=未授权, 其他=异常)").arg(result));
        if (result != 0) {
            VERIFY_LOG("1", ">>> 验证失败: 无录音权限，请点击「连接接收」先请求权限 <<<");
        } else {
            VERIFY_LOG("1", ">>> 方向1 通过: 权限已授予 <<<");
        }
    }

    // ========== 方向2: 延迟初始化 ==========
    VERIFY_LOG("2", "--- 方向2: 延迟初始化 (不在应用启动时 Initialize) ---");
    VERIFY_LOG("2", QString("当前 m_webrtcInitialized=%1").arg(m_webrtcInitialized ? "true" : "false"));
    VERIFY_LOG("2", "WebRTC 仅在【收到 Offer】时 initWebRTC，不在构造/启动时调用");
    if (m_webrtcInitialized) {
        VERIFY_LOG("2", ">>> 方向2 已通过: WebRTC/ADM 已初始化 <<<");
    } else {
        VERIFY_LOG("2", "尚未初始化，首次收到 Offer 时会调用 LibWebRTC::Initialize()");
        VERIFY_LOG("2", "若 Initialize/CreateFactory 失败，logcat 会看到 VERIFY-2 失败或 audio_device_impl Failed to create...");
    }

    // ========== 方向3: 初始化时机 ==========
    VERIFY_LOG("3", "--- 方向3: 初始化时机 (Activity 就绪后再连接) ---");
    VERIFY_LOG("3", "主界面 Component.onCompleted 后延迟 1.5s 才允许点击「连接接收」，确保 Activity 完全就绪");
    VERIFY_LOG("3", ">>> 方向3 已实现: 启动后按钮禁用 1.5s，状态显示「等待 Activity 就绪」<<<");

    // ========== 方向4: NDK/API 系统兼容性 ==========
    VERIFY_LOG("4", "--- 方向4: NDK/API 与系统兼容性 ---");
    int sdkInt = QJniObject::getStaticField<jint>("android/os/Build$VERSION", "SDK_INT");
    QJniObject releaseObj = QJniObject::getStaticObjectField("android/os/Build$VERSION", "RELEASE", "Ljava/lang/String;");
    QString release = releaseObj.isValid() ? releaseObj.toString() : "unknown";
    QJniObject abiObj = QJniObject::getStaticObjectField("android/os/Build", "CPU_ABI", "Ljava/lang/String;");
    QString abi = abiObj.isValid() ? abiObj.toString() : "unknown";
    VERIFY_LOG("4", QString("设备: Android %1, API %2, ABI %3").arg(release).arg(sdkInt).arg(abi));
    VERIFY_LOG("4", "确认 libwebrtc 编译目标 ABI 与设备一致 (如 lib/arm64-v8a 对应 arm64)");
    if (abi.contains("arm64") || abi.contains("aarch64")) {
        VERIFY_LOG("4", ">>> 方向4 ABI 匹配 arm64，若 ADM 仍失败可查 libwebrtc 的 NDK/API 版本 <<<");
    } else if (abi.contains("arm") || abi.contains("x86")) {
        VERIFY_LOG("4", QString(">>> 方向4 ABI %1，确认 libwebrtc 编译目标一致 <<<").arg(abi));
    } else {
        VERIFY_LOG("4", QString(">>> 方向4 未知 ABI %1 <<<").arg(abi));
    }

    // ========== 方向6: 设备/ROM 兼容 ==========
    VERIFY_LOG("6", "--- 方向6: 设备/ROM 兼容 ---");
    VERIFY_LOG("6", "若方向1~4 均通过仍出现 Failed to create platform specific ADM:");
    VERIFY_LOG("6", ">>> 尝试换一台设备或官方模拟器，部分定制ROM对 OpenSL/AAudio 支持不完整 <<<");

    // ========== 系统是否支持 audio capture ==========
    VERIFY_LOG("AUDIO", "--- 系统是否支持 Audio Capture (麦克风采集) ---");
    {
        QJniObject act = QJniObject::callStaticObjectMethod(
            "org/qtproject/qt/android/QtNative", "activity", "()Landroid/app/Activity;");
        if (act.isValid()) {
            QJniObject ctx = act.callObjectMethod("getApplicationContext", "()Landroid/content/Context;");
            if (ctx.isValid()) {
                QJniObject pm = ctx.callObjectMethod("getPackageManager", "()Landroid/content/pm/PackageManager;");
                if (pm.isValid()) {
                    bool hasMic = pm.callMethod<jboolean>("hasSystemFeature", "(Ljava/lang/String;)Z",
                        QJniObject::getStaticObjectField("android/content/pm/PackageManager",
                            "FEATURE_MICROPHONE", "Ljava/lang/String;").object<jstring>());
                    VERIFY_LOG("AUDIO", QString("【Audio Capture】硬件麦克风(FEATURE_MICROPHONE): %1")
                        .arg(hasMic ? "支持" : "不支持"));
                }
            }
        }
    }

    // ========== 音频格式比对: 扬声器 vs WebRTC 采集 ==========
    VERIFY_LOG("AUDIO", "--- 扬声器(输出) vs WebRTC 采集格式 ---");
    // 系统是否支持 audio capture (PackageManager.FEATURE_MICROPHONE)
    QJniObject actForAudio = QJniObject::callStaticObjectMethod(
        "org/qtproject/qt/android/QtNative", "activity", "()Landroid/app/Activity;");
    if (actForAudio.isValid()) {
        QJniObject context = actForAudio.callObjectMethod("getApplicationContext", "()Landroid/content/Context;");
        if (context.isValid()) {
            // 0. 系统是否支持 audio capture（硬件声明）
            QJniObject pm = context.callObjectMethod("getPackageManager", "()Landroid/content/pm/PackageManager;");
            if (pm.isValid()) {
                QJniObject micFeature = QJniObject::getStaticObjectField("android/content/pm/PackageManager",
                    "FEATURE_MICROPHONE", "Ljava/lang/String;");
                bool hasMic = pm.callMethod<jboolean>("hasSystemFeature", "(Ljava/lang/String;)Z",
                    micFeature.isValid() ? micFeature.object<jstring>() : nullptr);
                VERIFY_LOG("AUDIO", QString("【Audio Capture 支持】android.hardware.microphone=%1 (系统声明麦克风硬件)").arg(hasMic ? "是" : "否"));
            }
            QJniObject audioServiceStr = QJniObject::getStaticObjectField("android/content/Context",
                "AUDIO_SERVICE", "Ljava/lang/String;");
            QJniObject audioManager = context.callObjectMethod("getSystemService",
                "(Ljava/lang/String;)Ljava/lang/Object;", audioServiceStr.object<jstring>());
            if (audioManager.isValid()) {
                // 1. 扬声器(输出)采样率
                QJniObject outRateProp = QJniObject::getStaticObjectField("android/media/AudioManager",
                    "PROPERTY_OUTPUT_SAMPLE_RATE", "Ljava/lang/String;");
                QJniObject outRateVal = audioManager.callObjectMethod("getProperty",
                    "(Ljava/lang/String;)Ljava/lang/String;", outRateProp.object<jstring>());
                QString speakerRate = outRateVal.isValid() ? outRateVal.toString() : "null";
                VERIFY_LOG("AUDIO", QString("【Android 扬声器】采样率: %1 Hz (OUTPUT_SAMPLE_RATE)").arg(speakerRate));

                // 2. 输出帧缓冲
                QJniObject outBufProp = QJniObject::getStaticObjectField("android/media/AudioManager",
                    "PROPERTY_OUTPUT_FRAMES_PER_BUFFER", "Ljava/lang/String;");
                QJniObject outBufVal = audioManager.callObjectMethod("getProperty",
                    "(Ljava/lang/String;)Ljava/lang/String;", outBufProp.object<jstring>());
                QString speakerBuf = outBufVal.isValid() ? outBufVal.toString() : "null";
                VERIFY_LOG("AUDIO", QString("【Android 扬声器】帧缓冲: %1 frames").arg(speakerBuf));

                // 3. 采集(麦克风)格式支持检测: AudioRecord.getMinBufferSize 测试 48kHz/16kHz 是否可用
                jclass audioRecordClass = QJniEnvironment().findClass("android/media/AudioRecord");
                if (audioRecordClass) {
                    int enc = 2; // AudioFormat.ENCODING_PCM_16BIT
                    int chIn = 16; // AudioFormat.CHANNEL_IN_MONO
                    int buf48 = QJniObject::callStaticMethod<jint>("android/media/AudioRecord", "getMinBufferSize",
                        "(III)I", 48000, chIn, enc);
                    int buf16 = QJniObject::callStaticMethod<jint>("android/media/AudioRecord", "getMinBufferSize",
                        "(III)I", 16000, chIn, enc);
                    VERIFY_LOG("AUDIO", QString("【Android 采集】48kHz mono 16bit 支持: minBuf=%1 (>0=支持)").arg(buf48));
                    VERIFY_LOG("AUDIO", QString("【Android 采集】16kHz mono 16bit 支持: minBuf=%1 (>0=支持)").arg(buf16));
                }
            } else {
                VERIFY_LOG("AUDIO", "无法获取 AudioManager");
            }
        } else {
            VERIFY_LOG("AUDIO", "无法获取 Context");
        }
    } else {
        VERIFY_LOG("AUDIO", "Activity 无效，无法查询音频格式");
    }
    VERIFY_LOG("AUDIO", "【WebRTC 采集】常见格式: 48kHz 或 16kHz, 16-bit PCM, 1ch(mono)");
    VERIFY_LOG("AUDIO", "【WebRTC 播放】常见格式: 48kHz, 16-bit PCM, 2ch(stereo)");
    VERIFY_LOG("AUDIO", ">>> 若 Android 扬声器 != 48kHz/44.1kHz 或采集 minBuf<=0，可能与 WebRTC 默认格式冲突 <<<");
#else
    VERIFY_LOG("1", "--- 方向1: 权限 --- 非 Android，跳过");
    VERIFY_LOG("2", "--- 方向2: 延迟初始化 --- m_webrtcInitialized=" + QString(m_webrtcInitialized ? "true" : "false"));
    VERIFY_LOG("3", "--- 方向3: 禁用音频 --- 同上，需编译 null ADM");
    VERIFY_LOG("4", "--- 方向4: NDK/API --- 非 Android，跳过");
    VERIFY_LOG("6", "--- 方向6: 设备兼容 --- 非 Android，跳过");
#endif
    VERIFY_LOG("DIAG", "========== 逐项验证诊断结束 (logcat 过滤 VERIFY 可只看验证日志) ==========");
}

void WebRTCReceiverClient::requestPermissionAndConnect(const QString &url)
{
#ifdef Q_OS_ANDROID
    // VERIFY-1: 权限验证 - WebRTC ADM 需要 RECORD_AUDIO，否则可能引发 webrtc_voice_engine Check failed: adm_
    VERIFY_LOG("1", "检查 RECORD_AUDIO 权限...");
    QJniObject activity = QJniObject::callStaticObjectMethod(
        "org/qtproject/qt/android/QtNative", "activity", "()Landroid/app/Activity;");
    if (!activity.isValid()) {
        VERIFY_LOG("1", "Activity 无效，跳过权限检查直接连接(可能导致后续 ADM 失败)");
        connectToSignaling(url);
        return;
    }
    QJniObject permObj = QJniObject::getStaticObjectField("android/Manifest$permission",
        "RECORD_AUDIO", "Ljava/lang/String;");
    const int granted = 0; // PackageManager.PERMISSION_GRANTED
    int result = activity.callMethod<jint>("checkSelfPermission", "(Ljava/lang/String;)I",
        permObj.object<jstring>());
    VERIFY_LOG("1", QString("RECORD_AUDIO 权限: result=%1 (0=已授权)").arg(result));
    if (result == granted) {
        VERIFY_LOG("1", "权限已授权，继续连接");
        connectToSignaling(url);
        return;
    }
    // 需要请求权限：调用 requestPermissions，然后用定时器轮询
    QJniEnvironment env;
    jclass stringClass = env.findClass("java/lang/String");
    jobjectArray permissions = env->NewObjectArray(1, stringClass, nullptr);
    env->SetObjectArrayElement(permissions, 0, permObj.object<jstring>());
    VERIFY_LOG("1", "请求 RECORD_AUDIO 权限，等待用户授权...");
    activity.callMethod<void>("requestPermissions", "([Ljava/lang/String;I)V", permissions, 10001);
    env->DeleteLocalRef(permissions);
    // 轮询等待用户授权，最多 30 秒
    auto *checkTimer = new QTimer(this);
    QObject::connect(checkTimer, &QTimer::timeout, this, [this, checkTimer, url, permObj, activity]() {
        int r = activity.callMethod<jint>("checkSelfPermission", "(Ljava/lang/String;)I",
            permObj.object<jstring>());
        if (r == 0) {
            VERIFY_LOG("1", "用户已授权 RECORD_AUDIO，继续连接");
            checkTimer->stop();
            checkTimer->deleteLater();
            connectToSignaling(url);
        }
    });
    checkTimer->start(500);
    QTimer::singleShot(30000, this, [checkTimer]() {
        if (checkTimer->isActive()) {
            checkTimer->stop();
            checkTimer->deleteLater();
        }
    });
#else
    connectToSignaling(url);
#endif
}

void WebRTCReceiverClient::connectToSignaling(const QString &url)
{
    if (m_webSocket) {
        m_webSocket->close();
        m_webSocket->deleteLater();
        m_webSocket = nullptr;
    }
    m_socketIOConnected = false;

    m_signalingUrl = url.isEmpty() ? QStringLiteral(DEFAULT_SIGNALING_URL) : url;

    m_webSocket = new QWebSocket();
    connect(m_webSocket, &QWebSocket::connected, this, &WebRTCReceiverClient::onWebSocketConnected);
    connect(m_webSocket, &QWebSocket::disconnected, this, &WebRTCReceiverClient::onWebSocketDisconnected);
    connect(m_webSocket, &QWebSocket::errorOccurred, this, &WebRTCReceiverClient::onWebSocketError);
    connect(m_webSocket, &QWebSocket::textMessageReceived, this, &WebRTCReceiverClient::onWebSocketTextMessageReceived);

    Q_EMIT statusChanged("正在连接信令服务器...");
    m_webSocket->open(QUrl(m_signalingUrl));
}

void WebRTCReceiverClient::disconnect()
{
    if (m_videoRenderer) {
        auto *r = qobject_cast<WebRTCVideoRenderer *>(m_videoRenderer);
        if (r) r->clearVideoTrack();
    }
    if (m_webSocket) {
        m_webSocket->close();
        m_webSocket = nullptr;
    }
    m_socketIOConnected = false;
}

void WebRTCReceiverClient::setVideoRenderer(QObject *renderer)
{
    if (m_videoRenderer == renderer)
        return;
    if (m_videoRenderer) {
        auto *r = qobject_cast<WebRTCVideoRenderer *>(m_videoRenderer);
        if (r) r->clearVideoTrack();
    }
    m_videoRenderer = renderer;
}

void WebRTCReceiverClient::onWebSocketConnected()
{
    Q_EMIT statusChanged("WebSocket 已连接，正在建立 Socket.IO 会话...");
    // Socket.IO 连接后，服务器会先发 "0" 或 "0{...}"，我们收到后发 "40" 完成连接
    // 若首包不是 "0" 开头，也尝试发 "40"（兼容不同版本）
}

void WebRTCReceiverClient::onWebSocketDisconnected()
{
    m_socketIOConnected = false;
    Q_EMIT statusChanged("信令连接已断开");
}

void WebRTCReceiverClient::onWebSocketError(QAbstractSocket::SocketError error)
{
    QString errName;
    switch (error) {
    case QAbstractSocket::ConnectionRefusedError: errName = "连接被拒绝"; break;
    case QAbstractSocket::RemoteHostClosedError: errName = "远端关闭连接"; break;
    case QAbstractSocket::HostNotFoundError: errName = "主机未找到"; break;
    case QAbstractSocket::SocketTimeoutError: errName = "连接超时"; break;
    case QAbstractSocket::SocketAccessError: errName = "套接字访问错误"; break;
    case QAbstractSocket::SocketResourceError: errName = "套接字资源错误"; break;
    case QAbstractSocket::NetworkError: errName = "网络错误(无法访问,检查网络/防火墙/明文流量)"; break;
    case QAbstractSocket::TemporaryError: errName = "临时错误"; break;
    default: errName = QString("错误码%1").arg(static_cast<int>(error)); break;
    }
    QString detail = m_webSocket ? m_webSocket->errorString() : QString();
    QString msg = detail.isEmpty() ? QString("信令连接错误: %1").arg(errName)
                                  : QString("信令连接错误: %1 (%2)").arg(errName, detail);
    Q_EMIT statusChanged(msg);
}

void WebRTCReceiverClient::onWebSocketTextMessageReceived(const QString &message)
{
    handleSocketIOMessage(message);
}

// 解析 Socket.IO / Engine.IO 协议
// 格式: "0"=open, "40"=socket.io connect, "42["event",data]"=event
void WebRTCReceiverClient::handleSocketIOMessage(const QString &message)
{
    if (message.isEmpty()) return;

    QChar typeChar = message[0];
    QString payload = message.mid(1);

    if (typeChar == '0') {
        // Engine.IO open - 发送 Socket.IO connect
        sendSocketIOConnect();
    } else if (typeChar == '4' && payload.size() >= 1) {
        QChar subType = payload[0];
        QString subPayload = payload.mid(1);
        if (subType == '0') {
            // Socket.IO connected
            m_socketIOConnected = true;
            Q_EMIT statusChanged("已连接信令服务器，等待发送端加入...");
            sendJoinRoom();
        } else if (subType == '2') {
            // Socket.IO event: "42["event",data]"
            QJsonDocument doc = QJsonDocument::fromJson(subPayload.toUtf8());
            if (!doc.isArray()) return;
            QJsonArray arr = doc.array();
            if (arr.size() < 1) return;
            QString event = arr[0].toString();
            QJsonValue data = arr.size() >= 2 ? arr[1] : QJsonValue();

            if (event == "offer") {
                QJsonObject sdpObj = data.toObject();
                if (sdpObj.contains("sdp")) {
                    QJsonObject inner = sdpObj["sdp"].toObject();
                    QString type = inner["type"].toString();
                    QString sdp = inner["sdp"].toString();
                    handleOffer(type, sdp);
                } else {
                    // 兼容直接 {type, sdp} 格式
                    QString type = sdpObj["type"].toString();
                    QString sdp = sdpObj["sdp"].toString();
                    handleOffer(type, sdp);
                }
            } else if (event == "peer-joined") {
                Q_EMIT statusChanged("发送端已加入，等待 Offer...");
            }
        }
    } else if (typeChar == '2') {
        // Engine.IO ping - 回复 pong
        if (m_webSocket && m_webSocket->state() == QAbstractSocket::ConnectedState) {
            m_webSocket->sendTextMessage(QStringLiteral("3"));
        }
    }
}

void WebRTCReceiverClient::sendSocketIOConnect()
{
    // "40" = Socket.IO 连接到默认 namespace
    if (m_webSocket && m_webSocket->state() == QAbstractSocket::ConnectedState) {
        m_webSocket->sendTextMessage(QStringLiteral("40"));
    }
}

void WebRTCReceiverClient::sendSocketIOEvent(const QString &event, const QJsonValue &data)
{
    if (!m_webSocket || m_webSocket->state() != QAbstractSocket::ConnectedState)
        return;
    QJsonArray arr;
    arr.append(event);
    arr.append(data);
    QString msg = QStringLiteral("42") + QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    m_webSocket->sendTextMessage(msg);
}

void WebRTCReceiverClient::sendJoinRoom()
{
    sendSocketIOEvent("join", m_room);
}

void WebRTCReceiverClient::sendAnswer(const QString &type, const QString &sdp)
{
    QJsonObject sdpObj;
    sdpObj["type"] = type;
    sdpObj["sdp"] = sdp;
    QJsonObject data;
    data["room"] = m_room;
    data["sdp"] = sdpObj;
    sendSocketIOEvent("answer", data);
}

void WebRTCReceiverClient::initWebRTC()
{
    if (m_webrtcInitialized) {
        VERIFY_LOG("2", "WebRTC 已初始化，跳过");
        return;
    }
#ifdef Q_OS_ANDROID
    // 方向1 验证：init 前检查权限，避免 ADM 创建失败
    VERIFY_LOG("2", "方向2- init 前检查 RECORD_AUDIO (方向1 关联)...");
    QJniObject activity = QJniObject::callStaticObjectMethod(
        "org/qtproject/qt/android/QtNative", "activity", "()Landroid/app/Activity;");
    if (activity.isValid()) {
        QJniObject permObj = QJniObject::getStaticObjectField("android/Manifest$permission",
            "RECORD_AUDIO", "Ljava/lang/String;");
        int perm = activity.callMethod<jint>("checkSelfPermission", "(Ljava/lang/String;)I",
            permObj.object<jstring>());
        VERIFY_LOG("2", QString("RECORD_AUDIO=%1 (0=有权限，否则 ADM 可能失败)").arg(perm));
        if (perm != 0) {
            VERIFY_LOG("2", ">>> 方向1/2: 无权限时 Initialize 可能成功但 ADM 异常，析构时 Check failed: adm_ <<<");
        }
    }
#endif
    VERIFY_LOG("2", "调用 LibWebRTC::Initialize()...");
    if (!libwebrtc::LibWebRTC::Initialize()) {
        VERIFY_LOG("2", ">>> LibWebRTC::Initialize 失败 (方向2) 可能原因: 权限/ADM/系统不兼容 <<<");
        Q_EMIT statusChanged("WebRTC 初始化失败");
        return;
    }
    VERIFY_LOG("2", "LibWebRTC::Initialize 成功，创建 PeerConnectionFactory(含 VoiceEngine/ADM)...");
    m_factory = libwebrtc::LibWebRTC::CreateRTCPeerConnectionFactory();
    if (!m_factory) {
        VERIFY_LOG("2", ">>> CreateRTCPeerConnectionFactory 失败 (方向2) <<<");
        libwebrtc::LibWebRTC::Terminate();
        Q_EMIT statusChanged("创建 PeerConnectionFactory 失败");
        return;
    }
    VERIFY_LOG("2", "PeerConnectionFactory 创建成功，调用 factory->Initialize()");
    m_factory->Initialize();
    m_constraints = libwebrtc::RTCMediaConstraints::Create();
    m_webrtcInitialized = true;
    VERIFY_LOG("2", ">>> 方向2 通过: WebRTC/ADM 已初始化 <<<");
}

void WebRTCReceiverClient::createPeerConnection()
{
    if (m_peerConnection) {
        m_peerConnection->Close();
        m_peerConnection->DeRegisterRTCPeerConnectionObserver();
    }

    libwebrtc::RTCConfiguration config;
    config.ice_servers[0].uri = libwebrtc::string("stun:stun.l.google.com:19302");

    m_peerConnection = m_factory->Create(config, m_constraints);
    m_peerConnection->RegisterRTCPeerConnectionObserver(m_observer.get());
}

void WebRTCReceiverClient::handleOffer(const QString &type, const QString &sdp)
{
    Q_EMIT statusChanged("收到 Offer，正在收集 ICE 候选...");
    initWebRTC();
    createPeerConnection();

    // Step 1: setRemoteDescription(offer)
    const std::string temp_sdp = sdp.toStdString();
    const std::string temp_type = type.toStdString();

    m_peerConnection->SetRemoteDescription(temp_sdp, temp_type,
        [this]() {
            // Step 2: createAnswer
            m_peerConnection->CreateAnswer(
                [this](const libwebrtc::string &sdp, const libwebrtc::string &type) {
                    // Step 3: setLocalDescription(answer)
                    m_peerConnection->SetLocalDescription(sdp, type,
                        [this]() {
                            // Step 4: 等待 ICE 收集完成再发送（与 receiver.html 一致）
                            waitForIceGatheringComplete([this]() {
                                m_peerConnection->GetLocalDescription(
                                    [this](const char *sdp, const char *type) {
                                        sendAnswer(QString::fromUtf8(type), QString::fromUtf8(sdp));
                                        Q_EMIT statusChanged("Answer 已发送，等待连接...");
                                    },
                                    [this](const char *err) {
                                        Q_EMIT statusChanged(QString("获取 SDP 失败: %1").arg(err));
                                    }
                                );
                            });
                        },
                        [this](const char *err) {
                            Q_EMIT statusChanged(QString("SetLocalDescription 失败: %1").arg(err));
                        }
                    );
                },
                [this](const char *err) {
                    Q_EMIT statusChanged(QString("CreateAnswer 失败: %1").arg(err));
                },
                m_constraints
            );
        },
        [this](const char *err) {
            Q_EMIT statusChanged(QString("SetRemoteDescription 失败: %1").arg(err));
        }
    );
}

void WebRTCReceiverClient::waitForIceGatheringComplete(std::function<void()> callback)
{
    if (m_peerConnection->ice_gathering_state() == libwebrtc::RTCIceGatheringStateComplete) {
        callback();
        return;
    }
    m_observer->m_onIceComplete = callback;
    // 超时保护：5 秒后强制继续（与 receiver.html 一致）
    QTimer::singleShot(5000, this, [this, callback]() {
        if (m_observer->m_onIceComplete) {
            m_observer->m_onIceComplete = nullptr;
            callback();
        }
    });
}
