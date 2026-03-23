#include "webrtc_receiver_client.h"
#include "webrtc_video_renderer.h"
#include <iostream>
#include <memory>
#include <QDebug>
#include <QThread>
#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QJniEnvironment>
#endif

#ifndef DEFAULT_SIGNALING_ADDR
#define DEFAULT_SIGNALING_ADDR "192.168.3.20:8765"
#endif

#define VERIFY_LOG(tag, msg) qDebug() << "[VERIFY-" tag "]" << msg

// =============================================================================
// PeerConnectionObserver：实现 libwebrtc 的回调接口
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
                m_client->startStatsTimer();
            }, Qt::QueuedConnection);
        } else if (state == libwebrtc::RTCPeerConnectionStateFailed ||
                   state == libwebrtc::RTCPeerConnectionStateDisconnected ||
                   state == libwebrtc::RTCPeerConnectionStateClosed) {
            QMetaObject::invokeMethod(m_client, [this]() {
                m_client->stopStatsTimer();
            }, Qt::QueuedConnection);
        }
    }

    void OnIceGatheringState(libwebrtc::RTCIceGatheringState state) override {
        if (state == libwebrtc::RTCIceGatheringStateComplete) {
            qDebug() << "[Signaling] ICE 收集完成";
        }
    }

    // trickle ICE：每收集到一个本地候选就立即通过信令发送给对端
    void OnIceCandidate(libwebrtc::scoped_refptr<libwebrtc::RTCIceCandidate> candidate) override {
        if (!candidate) return;
        std::string mid = candidate->sdp_mid().std_string();
        int mline_index = candidate->sdp_mline_index();
        std::string sdp = candidate->candidate().std_string();
        QMetaObject::invokeMethod(m_client, [this, mid, mline_index, sdp]() {
            if (m_client->m_signaling) {
                m_client->m_signaling->SendIceCandidate(mid, mline_index, sdp);
            }
        }, Qt::QueuedConnection);
    }

    void OnTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpTransceiver> transceiver) override {
        if (transceiver->media_type() != libwebrtc::RTCMediaType::VIDEO)
            return;
        auto receiver = transceiver->receiver();
        if (!receiver) return;

        //receiver->SetJitterBufferMinimumDelay(0.02);

        auto track = receiver->track();
        if (!track || track->kind().std_string() != "video") return;

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
    void OnAddTrack(libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>> streams,
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
    int sdkInt = QJniObject::getStaticField<jint>("android/os/Build$VERSION", "SDK_INT");
    QJniObject releaseObj = QJniObject::getStaticObjectField("android/os/Build$VERSION", "RELEASE", "Ljava/lang/String;");
    QString release = releaseObj.isValid() ? releaseObj.toString() : "unknown";
    QJniObject abiObj = QJniObject::getStaticObjectField("android/os/Build", "CPU_ABI", "Ljava/lang/String;");
    QString abi = abiObj.isValid() ? abiObj.toString() : "unknown";
    VERIFY_LOG("4", QString("系统信息: Android %1, API %2, ABI %3").arg(release).arg(sdkInt).arg(abi));
    runVerificationDiagnostic();
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
    VERIFY_LOG("5", "等待 WebRTC worker_thread 停止(约 800ms)...");
    QThread::msleep(800);
    VERIFY_LOG("5", "释放 PeerConnectionFactory...");
    m_factory = nullptr;
    QThread::msleep(500);
#ifdef Q_OS_ANDROID
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

    VERIFY_LOG("2", "--- 方向2: 延迟初始化 (不在应用启动时 Initialize) ---");
    VERIFY_LOG("2", QString("当前 m_webrtcInitialized=%1").arg(m_webrtcInitialized ? "true" : "false"));
    VERIFY_LOG("2", "WebRTC 仅在【收到 Offer】时 initWebRTC，不在构造/启动时调用");
    if (m_webrtcInitialized) {
        VERIFY_LOG("2", ">>> 方向2 已通过: WebRTC/ADM 已初始化 <<<");
    } else {
        VERIFY_LOG("2", "尚未初始化，首次收到 Offer 时会调用 LibWebRTC::Initialize()");
        VERIFY_LOG("2", "若 Initialize/CreateFactory 失败，logcat 会看到 VERIFY-2 失败或 audio_device_impl Failed to create...");
    }

    VERIFY_LOG("3", "--- 方向3: 初始化时机 (Activity 就绪后再连接) ---");
    VERIFY_LOG("3", "主界面 Component.onCompleted 后延迟 1.5s 才允许点击「连接接收」，确保 Activity 完全就绪");
    VERIFY_LOG("3", ">>> 方向3 已实现: 启动后按钮禁用 1.5s，状态显示「等待 Activity 就绪」<<<");

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

    VERIFY_LOG("6", "--- 方向6: 设备/ROM 兼容 ---");
    VERIFY_LOG("6", "若方向1~4 均通过仍出现 Failed to create platform specific ADM:");
    VERIFY_LOG("6", ">>> 尝试换一台设备或官方模拟器，部分定制ROM对 OpenSL/AAudio 支持不完整 <<<");

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

    VERIFY_LOG("AUDIO", "--- 扬声器(输出) vs WebRTC 采集格式 ---");
    QJniObject actForAudio = QJniObject::callStaticObjectMethod(
        "org/qtproject/qt/android/QtNative", "activity", "()Landroid/app/Activity;");
    if (actForAudio.isValid()) {
        QJniObject context = actForAudio.callObjectMethod("getApplicationContext", "()Landroid/content/Context;");
        if (context.isValid()) {
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
                QJniObject outRateProp = QJniObject::getStaticObjectField("android/media/AudioManager",
                    "PROPERTY_OUTPUT_SAMPLE_RATE", "Ljava/lang/String;");
                QJniObject outRateVal = audioManager.callObjectMethod("getProperty",
                    "(Ljava/lang/String;)Ljava/lang/String;", outRateProp.object<jstring>());
                QString speakerRate = outRateVal.isValid() ? outRateVal.toString() : "null";
                VERIFY_LOG("AUDIO", QString("【Android 扬声器】采样率: %1 Hz (OUTPUT_SAMPLE_RATE)").arg(speakerRate));

                QJniObject outBufProp = QJniObject::getStaticObjectField("android/media/AudioManager",
                    "PROPERTY_OUTPUT_FRAMES_PER_BUFFER", "Ljava/lang/String;");
                QJniObject outBufVal = audioManager.callObjectMethod("getProperty",
                    "(Ljava/lang/String;)Ljava/lang/String;", outBufProp.object<jstring>());
                QString speakerBuf = outBufVal.isValid() ? outBufVal.toString() : "null";
                VERIFY_LOG("AUDIO", QString("【Android 扬声器】帧缓冲: %1 frames").arg(speakerBuf));

                jclass audioRecordClass = QJniEnvironment().findClass("android/media/AudioRecord");
                if (audioRecordClass) {
                    int enc = 2;
                    int chIn = 16;
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

void WebRTCReceiverClient::requestPermissionAndConnect(const QString &addr)
{
#ifdef Q_OS_ANDROID
    VERIFY_LOG("1", "检查 RECORD_AUDIO 权限...");
    QJniObject activity = QJniObject::callStaticObjectMethod(
        "org/qtproject/qt/android/QtNative", "activity", "()Landroid/app/Activity;");
    if (!activity.isValid()) {
        VERIFY_LOG("1", "Activity 无效，跳过权限检查直接连接(可能导致后续 ADM 失败)");
        connectToSignaling(addr);
        return;
    }
    QJniObject permObj = QJniObject::getStaticObjectField("android/Manifest$permission",
        "RECORD_AUDIO", "Ljava/lang/String;");
    const int granted = 0;
    int result = activity.callMethod<jint>("checkSelfPermission", "(Ljava/lang/String;)I",
        permObj.object<jstring>());
    VERIFY_LOG("1", QString("RECORD_AUDIO 权限: result=%1 (0=已授权)").arg(result));
    if (result == granted) {
        VERIFY_LOG("1", "权限已授权，继续连接");
        connectToSignaling(addr);
        return;
    }
    QJniEnvironment env;
    jclass stringClass = env.findClass("java/lang/String");
    jobjectArray permissions = env->NewObjectArray(1, stringClass, nullptr);
    env->SetObjectArrayElement(permissions, 0, permObj.object<jstring>());
    VERIFY_LOG("1", "请求 RECORD_AUDIO 权限，等待用户授权...");
    activity.callMethod<void>("requestPermissions", "([Ljava/lang/String;I)V", permissions, 10001);
    env->DeleteLocalRef(permissions);
    auto *checkTimer = new QTimer(this);
    QObject::connect(checkTimer, &QTimer::timeout, this, [this, checkTimer, addr, permObj, activity]() {
        int r = activity.callMethod<jint>("checkSelfPermission", "(Ljava/lang/String;)I",
            permObj.object<jstring>());
        if (r == 0) {
            VERIFY_LOG("1", "用户已授权 RECORD_AUDIO，继续连接");
            checkTimer->stop();
            checkTimer->deleteLater();
            connectToSignaling(addr);
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
    connectToSignaling(addr);
#endif
}

// =============================================================================
// 信令层：使用 TCP + JSON-per-line 协议（对应 signaling_client.cpp）
// =============================================================================

void WebRTCReceiverClient::connectToSignaling(const QString &addr)
{
    if (m_signaling) {
        m_signaling->Stop();
        m_signaling.reset();
    }

    std::string serverAddr = addr.isEmpty()
        ? DEFAULT_SIGNALING_ADDR
        : addr.toStdString();

    m_signaling = std::make_unique<webrtc_demo::SignalingClient>(serverAddr, "subscriber");

    m_signaling->SetOnOffer([this](const std::string &type, const std::string &sdp) {
        QMetaObject::invokeMethod(this, [this, type, sdp]() {
            handleOffer(type, sdp);
        }, Qt::QueuedConnection);
    });

    m_signaling->SetOnIce([this](const std::string &mid, int mline_index, const std::string &candidate) {
        QMetaObject::invokeMethod(this, [this, mid, mline_index, candidate]() {
            handleRemoteIceCandidate(mid, mline_index, candidate);
        }, Qt::QueuedConnection);
    });

    m_signaling->SetOnError([this](const std::string &err) {
        QString msg = QString::fromStdString(err);
        QMetaObject::invokeMethod(this, [this, msg]() {
            Q_EMIT statusChanged(QString("信令错误: %1").arg(msg));
        }, Qt::QueuedConnection);
    });

    Q_EMIT statusChanged("正在连接信令服务器...");

    if (!m_signaling->Start()) {
        Q_EMIT statusChanged("连接信令服务器失败");
        m_signaling.reset();
        return;
    }

    Q_EMIT statusChanged("已连接信令服务器 (role=receiver)，等待 Offer...");
}

void WebRTCReceiverClient::disconnect()
{
    stopStatsTimer();
    if (m_videoRenderer) {
        auto *r = qobject_cast<WebRTCVideoRenderer *>(m_videoRenderer);
        if (r) r->clearVideoTrack();
    }
    if (m_signaling) {
        m_signaling->Stop();
        m_signaling.reset();
    }
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

// =============================================================================
// WebRTC 层
// =============================================================================

void WebRTCReceiverClient::initWebRTC()
{
    if (m_webrtcInitialized) {
        VERIFY_LOG("2", "WebRTC 已初始化，跳过");
        return;
    }
#ifdef Q_OS_ANDROID
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

void WebRTCReceiverClient::handleOffer(const std::string &type, const std::string &sdp)
{
    Q_EMIT statusChanged("收到 Offer，正在创建 Answer...");

    initWebRTC();
    createPeerConnection();

    m_peerConnection->SetRemoteDescription(
        libwebrtc::string(sdp.c_str()), libwebrtc::string(type.c_str()),
        [this]() {
            qDebug() << "[P2pPlayer] SetRemoteDescription OK, CreateAnswer";
            m_peerConnection->CreateAnswer(
                [this](const libwebrtc::string &sdp, const libwebrtc::string &type) {
                    m_peerConnection->SetLocalDescription(
                        sdp, type,
                        [this, sdp]() {
                            m_signaling->SendAnswer(sdp.std_string());
                            Q_EMIT statusChanged("Answer 已发送，ICE 候选交换中...");
                        },
                        [this](const char *err) {
                            Q_EMIT statusChanged(QString("SetLocalDescription 失败: %1").arg(err));
                        });
                },
                [this](const char *err) {
                    Q_EMIT statusChanged(QString("CreateAnswer 失败: %1").arg(err));
                },
                libwebrtc::RTCMediaConstraints::Create());
        },
        [this](const char *err) {
            Q_EMIT statusChanged(QString("SetRemoteDescription 失败: %1").arg(err));
        });
}

void WebRTCReceiverClient::handleRemoteIceCandidate(const std::string &mid, int mline_index,
                                                     const std::string &candidate)
{
    if (!m_peerConnection) return;
    m_peerConnection->AddCandidate(libwebrtc::string(mid.c_str()), mline_index,
                                   libwebrtc::string(candidate.c_str()));
}

// =============================================================================
// GetStats 定时采集：每 3 秒打印一次解码/网络关键指标
// =============================================================================

void WebRTCReceiverClient::startStatsTimer()
{
    if (m_statsTimer)
        return;
    m_prevFramesDecoded = 0;
    m_prevTotalDecodeTime = 0.0;
    m_prevFramesReceived = 0;
    m_prevFramesDropped = 0;

    m_statsTimer = new QTimer(this);
    m_statsTimer->setInterval(3000);
    connect(m_statsTimer, &QTimer::timeout, this, [this]() {
        if (!m_peerConnection)
            return;
        m_peerConnection->GetStats(
            [this](const libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::MediaRTCStats>> reports) {
                for (const auto &stat : reports.std_vector()) {
                    std::string type = stat->type().std_string();
                    if (type != "inbound-rtp")
                        continue;

                    bool isVideo = false;
                    uint32_t framesDecoded = 0;
                    double totalDecodeTime = 0.0;
                    double fps = 0.0;
                    uint32_t framesReceived = 0;
                    uint32_t framesDropped = 0;
                    double jitterBufferDelay = 0.0;
                    uint64_t jitterBufferEmitted = 0;
                    int frameWidth = 0, frameHeight = 0;
                    uint64_t bytesReceived = 0;
                    std::string decoderImpl;

                    for (const auto &m : stat->Members().std_vector()) {
                        if (!m->IsDefined())
                            continue;
                        std::string name = m->GetName().std_string();
                        if (name == "kind" || name == "mediaType") {
                            if (m->ValueString().std_string() == "video")
                                isVideo = true;
                        } else if (name == "framesDecoded") {
                            framesDecoded = m->ValueUint32();
                        } else if (name == "totalDecodeTime") {
                            totalDecodeTime = m->ValueDouble();
                        } else if (name == "framesPerSecond") {
                            fps = m->ValueDouble();
                        } else if (name == "framesReceived") {
                            framesReceived = m->ValueUint32();
                        } else if (name == "framesDropped") {
                            framesDropped = m->ValueUint32();
                        } else if (name == "jitterBufferDelay") {
                            jitterBufferDelay = m->ValueDouble();
                        } else if (name == "jitterBufferEmittedCount") {
                            jitterBufferEmitted = m->ValueUint64();
                        } else if (name == "frameWidth") {
                            frameWidth = static_cast<int>(m->ValueUint32());
                        } else if (name == "frameHeight") {
                            frameHeight = static_cast<int>(m->ValueUint32());
                        } else if (name == "bytesReceived") {
                            bytesReceived = m->ValueUint64();
                        } else if (name == "decoderImplementation") {
                            decoderImpl = m->ValueString().std_string();
                        }
                    }

                    if (!isVideo)
                        continue;

                    uint32_t deltaFrames = framesDecoded - m_prevFramesDecoded;
                    double deltaTime = totalDecodeTime - m_prevTotalDecodeTime;
                    double avgDecodeMs = (deltaFrames > 0) ? (deltaTime / deltaFrames * 1000.0) : 0.0;
                    uint32_t deltaDropped = framesDropped - m_prevFramesDropped;
                    double avgJitterMs = (jitterBufferEmitted > 0)
                        ? (jitterBufferDelay / jitterBufferEmitted * 1000.0) : 0.0;

                    QMetaObject::invokeMethod(this, [=]() {
                        qDebug().noquote() << QString(
                            "[DecodeStats] %1x%2 | 解码器: %3 | fps: %4 | "
                            "解码帧: +%5 (总%6) | 平均解码: %7 ms/帧 | "
                            "丢帧: +%8 (总%9) | 抖动缓冲: %10 ms | "
                            "接收: %11 KB")
                            .arg(frameWidth).arg(frameHeight)
                            .arg(QString::fromStdString(decoderImpl))
                            .arg(fps, 0, 'f', 1)
                            .arg(deltaFrames).arg(framesDecoded)
                            .arg(avgDecodeMs, 0, 'f', 2)
                            .arg(deltaDropped).arg(framesDropped)
                            .arg(avgJitterMs, 0, 'f', 1)
                            .arg(bytesReceived / 1024);
                    }, Qt::QueuedConnection);

                    m_prevFramesDecoded = framesDecoded;
                    m_prevTotalDecodeTime = totalDecodeTime;
                    m_prevFramesReceived = framesReceived;
                    m_prevFramesDropped = framesDropped;
                    break;
                }
            },
            [](const char *error) {
                qWarning() << "[DecodeStats] GetStats failed:" << error;
            });
    });
    m_statsTimer->start();
    qDebug() << "[DecodeStats] 统计定时器已启动 (每 3s)";
}

void WebRTCReceiverClient::stopStatsTimer()
{
    if (m_statsTimer) {
        m_statsTimer->stop();
        m_statsTimer->deleteLater();
        m_statsTimer = nullptr;
        qDebug() << "[DecodeStats] 统计定时器已停止";
    }
}
