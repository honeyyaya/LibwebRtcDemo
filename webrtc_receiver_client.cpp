#include "webrtc_receiver_client.h"
#include "webrtc_factory_helper.h"
#include "webrtc_video_renderer.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>

#include <QDebug>
#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/media_types.h"
#include "api/rtc_error.h"
#include "api/rtp_receiver_interface.h"
#include "api/rtp_transceiver_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtc_stats_report.h"
#include "api/stats/rtcstats_objects.h"
#include "system_wrappers/include/field_trial.h"

#ifdef Q_OS_ANDROID
#include <QJniObject>
#endif

#ifndef DEFAULT_SIGNALING_ADDR
#define DEFAULT_SIGNALING_ADDR "192.168.3.20:8765"
#endif

#define VERIFY_LOG(tag, msg) qDebug() << "[VERIFY-" tag "]" << msg

namespace {

webrtc::SdpType SdpTypeFromOfferAnswerString(const std::string &t) {
  auto opt = webrtc::SdpTypeFromString(t);
  if (opt)
    return *opt;
  if (t == "offer")
    return webrtc::SdpType::kOffer;
  if (t == "answer")
    return webrtc::SdpType::kAnswer;
  if (t == "pranswer")
    return webrtc::SdpType::kPrAnswer;
  return webrtc::SdpType::kOffer;
}

class SetRemoteDescObserver : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  explicit SetRemoteDescObserver(std::function<void(webrtc::RTCError)> on_done)
      : on_done_(std::move(on_done)) {}

  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    if (on_done_)
      on_done_(std::move(error));
  }

 private:
  std::function<void(webrtc::RTCError)> on_done_;
};

class CreateAnswerObserver : public webrtc::CreateSessionDescriptionObserver {
 public:
  CreateAnswerObserver(std::function<void(webrtc::RTCError, std::unique_ptr<webrtc::SessionDescriptionInterface>)> cb,
                       std::function<void(webrtc::RTCError)> fail)
      : cb_(std::move(cb)), fail_(std::move(fail)) {}

  void OnSuccess(webrtc::SessionDescriptionInterface *desc) override {
    if (cb_)
      cb_(webrtc::RTCError::OK(), std::unique_ptr<webrtc::SessionDescriptionInterface>(desc));
  }

  void OnFailure(webrtc::RTCError error) override {
    if (fail_)
      fail_(std::move(error));
  }

 private:
  std::function<void(webrtc::RTCError, std::unique_ptr<webrtc::SessionDescriptionInterface>)> cb_;
  std::function<void(webrtc::RTCError)> fail_;
};

class SetLocalDescObserver : public webrtc::SetSessionDescriptionObserver {
 public:
  SetLocalDescObserver(std::function<void()> ok, std::function<void(webrtc::RTCError)> fail)
      : ok_(std::move(ok)), fail_(std::move(fail)) {}

  void OnSuccess() override {
    if (ok_)
      ok_();
  }

  void OnFailure(webrtc::RTCError error) override {
    if (fail_)
      fail_(std::move(error));
  }

 private:
  std::function<void()> ok_;
  std::function<void(webrtc::RTCError)> fail_;
};

class StatsCallback : public webrtc::RTCStatsCollectorCallback {
 public:
  explicit StatsCallback(std::function<void(const webrtc::scoped_refptr<const webrtc::RTCStatsReport> &)> fn)
      : fn_(std::move(fn)) {}

  void OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport> &report) override {
    if (fn_)
      fn_(report);
  }

 private:
  std::function<void(const webrtc::scoped_refptr<const webrtc::RTCStatsReport> &)> fn_;
};

}  // namespace

class WebRTCReceiverClient::PeerConnectionObserverImpl : public webrtc::PeerConnectionObserver {
 public:
  explicit PeerConnectionObserverImpl(WebRTCReceiverClient *client) : m_client(client) {}

  void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override {
    (void)new_state;
  }

  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override {
    (void)data_channel;
  }

  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override {
    const char *name = "?";
    switch (new_state) {
      case webrtc::PeerConnectionInterface::kIceGatheringNew:
        name = "New";
        break;
      case webrtc::PeerConnectionInterface::kIceGatheringGathering:
        name = "Gathering";
        break;
      case webrtc::PeerConnectionInterface::kIceGatheringComplete:
        name = "Complete";
        break;
    }
    qDebug() << "[ICE] IceGatheringState =" << name << "(" << static_cast<int>(new_state) << ")";
  }

  void OnIceCandidate(const webrtc::IceCandidateInterface *candidate) override {
    if (!candidate) {
      qWarning() << "[ICE] OnIceCandidate: candidate 为空";
      return;
    }
    if (!m_client->m_signaling) {
      qWarning() << "[ICE] OnIceCandidate: 信令未连接，无法发出候选";
      return;
    }
    std::string sdp;
    if (!candidate->ToString(&sdp)) {
      qWarning() << "[ICE] OnIceCandidate: ToString 失败";
      return;
    }
    std::string mid = candidate->sdp_mid();
    int mline = candidate->sdp_mline_index();
    qDebug() << "[ICE] 本地候选(将经信令发出) mid=" << QString::fromStdString(mid) << "mline=" << mline
              << "sdp前48字=" << QString::fromStdString(sdp.substr(0, std::min(sdp.size(), size_t(48))));
    QMetaObject::invokeMethod(
        m_client,
        [this, mid, mline, sdp]() {
          if (m_client->m_signaling)
            m_client->m_signaling->SendIceCandidate(mid, mline, sdp);
        },
        Qt::QueuedConnection);
  }

  void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) override {
    using PCS = webrtc::PeerConnectionInterface::PeerConnectionState;
    if (new_state == PCS::kConnected) {
      QMetaObject::invokeMethod(
          m_client,
          [this]() {
            Q_EMIT m_client->statusChanged("WebRTC 已连接，正在接收视频");
            m_client->startStatsTimer();
          },
          Qt::QueuedConnection);
    } else if (new_state == PCS::kFailed || new_state == PCS::kDisconnected ||
               new_state == PCS::kClosed) {
      QMetaObject::invokeMethod(
          m_client,
          [this]() { m_client->stopStatsTimer(); },
          Qt::QueuedConnection);
    }
  }

  void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override {
    if (new_state == webrtc::PeerConnectionInterface::kIceConnectionFailed) {
      QMetaObject::invokeMethod(
          m_client,
          [this]() { Q_EMIT m_client->statusChanged("ICE 连接失败"); },
          Qt::QueuedConnection);
    }
  }

  void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override {
    if (!transceiver || transceiver->media_type() != webrtc::MediaType::VIDEO)
      return;
    auto receiver = transceiver->receiver();
    if (!receiver)
      return;
    // 将抖动缓冲最小播放延迟设为 0 s（下限）；实际延迟仍可能 >0（重排序、拥塞控制等）。
    receiver->SetJitterBufferMinimumDelay(std::optional<double>(0.0));
    qDebug() << "[P2pPlayer] RtpReceiver SetJitterBufferMinimumDelay(0.0) 已应用 (video)";
    auto track = receiver->track();
    if (!track || track->kind() != std::string(webrtc::MediaStreamTrackInterface::kVideoKind))
      return;
    auto *video = static_cast<webrtc::VideoTrackInterface *>(track.get());
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> v(video);
    QMetaObject::invokeMethod(
        m_client,
        [this, v]() { Q_EMIT m_client->remoteVideoTrackReady(v); },
        Qt::QueuedConnection);
  }

 private:
  WebRTCReceiverClient *m_client;
};

// =============================================================================
WebRTCReceiverClient::WebRTCReceiverClient(QObject *parent)
    : QObject(parent)
{
  m_observer = std::make_unique<WebRTCReceiverClient::PeerConnectionObserverImpl>(this);
  connect(this, &WebRTCReceiverClient::remoteVideoTrackReady, this,
          [this](webrtc::scoped_refptr<webrtc::VideoTrackInterface> track) {
            if (m_videoRenderer) {
              auto *r = qobject_cast<WebRTCVideoRenderer *>(m_videoRenderer);
              if (r)
                r->setVideoTrack(track);
            }
          });
#ifdef Q_OS_ANDROID
  int sdkInt = QJniObject::getStaticField<jint>("android/os/Build$VERSION", "SDK_INT");
  QJniObject releaseObj =
      QJniObject::getStaticObjectField("android/os/Build$VERSION", "RELEASE", "Ljava/lang/String;");
  QString release = releaseObj.isValid() ? releaseObj.toString() : "unknown";
  QJniObject abiObj =
      QJniObject::getStaticObjectField("android/os/Build", "CPU_ABI", "Ljava/lang/String;");
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
    VERIFY_LOG("5", "关闭 PeerConnection");
    m_peerConnection->Close();
    m_peerConnection = nullptr;
  }
  VERIFY_LOG("5", "等待 WebRTC worker_thread 停止(约 800ms)...");
  QThread::msleep(800);
  VERIFY_LOG("5", "释放 PeerConnectionFactory...");
  m_factory = nullptr;
  QThread::msleep(200);
  m_webrtcInitialized = false;
  VERIFY_LOG("5", "析构完成");
}

void WebRTCReceiverClient::runVerificationDiagnostic()
{
  VERIFY_LOG("DIAG", "========== 逐项验证诊断开始 (排除方向5) ==========");
#ifdef Q_OS_ANDROID
  VERIFY_LOG("1", "--- 方向1: 权限 --- 纯视频接收，不检查/不请求 RECORD_AUDIO");

  VERIFY_LOG("2", "--- 方向2: 延迟初始化 (不在应用启动时 Initialize) ---");
  VERIFY_LOG("2", QString("当前 m_webrtcInitialized=%1").arg(m_webrtcInitialized ? "true" : "false"));
  VERIFY_LOG("2", "WebRTC 仅在【收到 Offer】时 initWebRTC，不在构造/启动时调用");
  VERIFY_LOG("4", "--- 方向4: NDK/API 与系统兼容性 ---");
  int sdkInt = QJniObject::getStaticField<jint>("android/os/Build$VERSION", "SDK_INT");
  QJniObject releaseObj =
      QJniObject::getStaticObjectField("android/os/Build$VERSION", "RELEASE", "Ljava/lang/String;");
  QString release = releaseObj.isValid() ? releaseObj.toString() : "unknown";
  QJniObject abiObj =
      QJniObject::getStaticObjectField("android/os/Build", "CPU_ABI", "Ljava/lang/String;");
  QString abi = abiObj.isValid() ? abiObj.toString() : "unknown";
  VERIFY_LOG("4", QString("设备: Android %1, API %2, ABI %3").arg(release).arg(sdkInt).arg(abi));
  VERIFY_LOG("4", "确认预编译 WebRTC 与设备 ABI 一致 (如 lib/arm64-v8a 对应 arm64)");
#else
  VERIFY_LOG("1", "--- 方向1: 权限 --- 非 Android，跳过");
  VERIFY_LOG("4", "--- 方向4: NDK/API --- 非 Android，跳过");
#endif
  VERIFY_LOG("DIAG", "========== 逐项验证诊断结束 ==========");
}

void WebRTCReceiverClient::requestPermissionAndConnect(const QString &addr)
{
  connectToSignaling(addr);
}

void WebRTCReceiverClient::connectToSignaling(const QString &addr)
{
  if (m_signaling) {
    m_signaling->Stop();
    m_signaling.reset();
  }

  std::string serverAddr = addr.isEmpty() ? DEFAULT_SIGNALING_ADDR : addr.toStdString();

  m_signaling = std::make_unique<webrtc_demo::SignalingClient>(serverAddr, "subscriber");

  m_signaling->SetOnOffer([this](const std::string &type, const std::string &sdp) {
    QMetaObject::invokeMethod(this, [this, type, sdp]() { handleOffer(type, sdp); }, Qt::QueuedConnection);
  });

  m_signaling->SetOnIce([this](const std::string &mid, int mline_index, const std::string &candidate) {
    QMetaObject::invokeMethod(
        this,
        [this, mid, mline_index, candidate]() { handleRemoteIceCandidate(mid, mline_index, candidate); },
        Qt::QueuedConnection);
  });

  m_signaling->SetOnError([this](const std::string &err) {
    QString msg = QString::fromStdString(err);
    QMetaObject::invokeMethod(
        this,
        [this, msg]() { Q_EMIT statusChanged(QString("信令错误: %1").arg(msg)); },
        Qt::QueuedConnection);
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
  m_pendingRemoteIce.clear();
  m_remoteDescriptionApplied = false;
  m_pendingSetRemoteObserver = nullptr;
  m_pendingCreateAnswerObserver = nullptr;
  m_pendingSetLocalObserver = nullptr;
  if (m_videoRenderer) {
    auto *r = qobject_cast<WebRTCVideoRenderer *>(m_videoRenderer);
    if (r)
      r->clearVideoTrack();
  }
  if (m_signaling) {
    m_signaling->Stop();
    m_signaling.reset();
  }
  if (m_peerConnection) {
    m_peerConnection->Close();
    m_peerConnection = nullptr;
  }
}

void WebRTCReceiverClient::setVideoRenderer(QObject *renderer)
{
  if (m_videoRenderer == renderer)
    return;
  if (m_videoRenderer) {
    auto *r = qobject_cast<WebRTCVideoRenderer *>(m_videoRenderer);
    if (r)
      r->clearVideoTrack();
  }
  m_videoRenderer = renderer;
}

void WebRTCReceiverClient::initWebRTC()
{
  if (m_webrtcInitialized) {
    VERIFY_LOG("2", "WebRTC 已初始化，跳过");
    return;
  }
  VERIFY_LOG("2", "创建 PeerConnectionFactory (官方 API)...");
  m_factory = webrtc_demo::CreatePeerConnectionFactory();
  if (!m_factory) {
    Q_EMIT statusChanged("创建 PeerConnectionFactory 失败");
    return;
  }
  m_webrtcInitialized = true;
  VERIFY_LOG("2", "PeerConnectionFactory 就绪");
}

void WebRTCReceiverClient::createPeerConnection()
{
  // 须在 CreatePeerConnectionFactory 之前注册；全局至多一次（见 field_trial.h）。
  // 与发送端保持一致：FrameTracking（RTP 扩展协商）+ FlexFEC-03（SDP 中带 flexfec-03，且启用 FEC 收包；可与 NACK 并存）。
  static const char kReceiverFieldTrials[] =
      "WebRTC-VideoFrameTrackingIdAdvertised/Enabled/"
      "WebRTC-FlexFEC-03-Advertised/Enabled/"
      "WebRTC-FlexFEC-03/Enabled/";
  static bool field_trials_inited = false;
  if (!field_trials_inited) {
    webrtc::field_trial::InitFieldTrialsFromString(kReceiverFieldTrials);
    field_trials_inited = true;
  }
  auto log_trial = [](const char *name) {
    const std::string full = webrtc::field_trial::FindFullName(name);
    const bool on = webrtc::field_trial::IsEnabled(name);
    VERIFY_LOG("2", QString("FieldTrial %1: fullName=\"%2\", IsEnabled=%3")
                        .arg(name)
                        .arg(QString::fromStdString(full))
                        .arg(on ? "true" : "false"));
  };
  log_trial("WebRTC-VideoFrameTrackingIdAdvertised");
  log_trial("WebRTC-FlexFEC-03-Advertised");
  log_trial("WebRTC-FlexFEC-03");

  initWebRTC();
  if (!m_factory) {
    return;
  }

  if (m_peerConnection) {
    m_peerConnection->Close();
    m_peerConnection = nullptr;
  }

  webrtc::PeerConnectionInterface::RTCConfiguration config;
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  // 音频 NetEq：最小目标延迟（ms），默认即为 0；显式写出便于与视频 RtpReceiver 策略一致。
  config.audio_jitter_buffer_min_delay_ms = 0;
  webrtc::PeerConnectionInterface::IceServer ice_server;
  ice_server.urls.push_back("stun:stun.l.google.com:19302");
  config.servers.push_back(ice_server);

  webrtc::PeerConnectionDependencies deps(m_observer.get());
  auto result = m_factory->CreatePeerConnectionOrError(config, std::move(deps));
  if (!result.ok()) {
    Q_EMIT statusChanged(QString("CreatePeerConnection 失败: %1")
                             .arg(result.error().message()));
    return;
  }
  m_peerConnection = result.MoveValue();
}

void WebRTCReceiverClient::handleOffer(const std::string &type, const std::string &sdp)
{
  Q_EMIT statusChanged("收到 Offer，正在创建 Answer...");

  m_pendingRemoteIce.clear();
  m_remoteDescriptionApplied = false;
  m_pendingSetRemoteObserver = nullptr;
  m_pendingCreateAnswerObserver = nullptr;
  m_pendingSetLocalObserver = nullptr;

  createPeerConnection();
  if (!m_peerConnection) {
    return;
  }

  webrtc::SdpParseError err;
  webrtc::SdpType sdp_type = SdpTypeFromOfferAnswerString(type);
  auto remote = webrtc::CreateSessionDescription(sdp_type, sdp, &err);
  if (!remote) {
    Q_EMIT statusChanged(QString("解析远端 SDP 失败: %1").arg(QString::fromStdString(err.description)));
    return;
  }

  m_pendingSetRemoteObserver = webrtc::make_ref_counted<SetRemoteDescObserver>(
      [this](webrtc::RTCError error) {
        m_pendingSetRemoteObserver = nullptr;
        qDebug() << "[P2pPlayer] SetRemoteDescription 回调进入 ok=" << error.ok();
        if (!error.ok()) {
          m_remoteDescriptionApplied = false;
          m_pendingRemoteIce.clear();
          Q_EMIT statusChanged(
              QString("SetRemoteDescription 失败: %1").arg(QString::fromStdString(error.message())));
          return;
        }
        m_remoteDescriptionApplied = true;
        flushPendingRemoteIceCandidates();
        // 若在 WebRTC 完成回调所在线程里直接 CreateAnswer，会卡在 CreateAnswer() 内（observer 永不回调）。
        // PostTask 到 network 线程仍会阻塞同一套操作链；改投递到 Qt 线程（与 handleOffer 同源），由 PC 代理再转内部线程。
        qDebug() << "[P2pPlayer] SetRemote 成功, QueuedConnection -> Qt 线程执行 CreateAnswer";
        QPointer<WebRTCReceiverClient> self(this);
        QMetaObject::invokeMethod(
            this,
            [self]() {
              if (!self)
                return;
              self->doCreateAnswerAfterSetRemote();
            },
            Qt::QueuedConnection);
      });

  qDebug() << "[P2pPlayer] 即将 SetRemoteDescription(异步), 已挂 m_pendingSetRemoteObserver";
  m_peerConnection->SetRemoteDescription(std::move(remote), m_pendingSetRemoteObserver);
  qDebug() << "[P2pPlayer] SetRemoteDescription 调用已返回(结果在回调, 勿与成功混淆)";

}

void WebRTCReceiverClient::doCreateAnswerAfterSetRemote()
{
  qDebug() << "[P2pPlayer] doCreateAnswerAfterSetRemote 进入 pc=" << (m_peerConnection ? "有" : "无");
  if (!m_peerConnection)
    return;

  m_pendingCreateAnswerObserver = webrtc::make_ref_counted<CreateAnswerObserver>(
      [this](webrtc::RTCError e, std::unique_ptr<webrtc::SessionDescriptionInterface> desc) {
        m_pendingCreateAnswerObserver = nullptr;
        qDebug() << "[P2pPlayer] CreateAnswer 回调进入 ok=" << e.ok() << " desc=" << (desc ? "非空" : "空");
        if (!e.ok()) {
          Q_EMIT statusChanged(
              QString("CreateAnswer 失败: %1").arg(QString::fromStdString(e.message())));
          return;
        }
        if (!desc) {
          qDebug() << "[P2pPlayer] CreateAnswer 成功但 desc 为空, 中止";
          Q_EMIT statusChanged("CreateAnswer 成功但未返回 SDP 对象");
          return;
        }
        std::string answer_sdp;
        if (!desc->ToString(&answer_sdp) || answer_sdp.empty()) {
          qDebug() << "[P2pPlayer] CreateAnswer ToString 失败或 sdp 空";
          Q_EMIT statusChanged("CreateAnswer SDP ToString 失败或为空");
          return;
        }
        qDebug() << "[P2pPlayer] CreateAnswer OK, sdp 字节=" << static_cast<int>(answer_sdp.size());

        m_pendingSetLocalObserver = webrtc::make_ref_counted<SetLocalDescObserver>(
            [this, answer_sdp]() {
              m_pendingSetLocalObserver = nullptr;
              qDebug() << "[P2pPlayer] SetLocalDescription OnSuccess 进入, sdp 字节="
                       << static_cast<int>(answer_sdp.size())
                       << " signaling=" << (m_signaling ? "非空" : "空");
              if (!m_signaling) {
                Q_EMIT statusChanged("SetLocal OK 但信令已断开，Answer 未发出");
                qWarning() << "[P2pPlayer] SendAnswer 跳过: m_signaling 为空";
                return;
              }
              m_signaling->SendAnswer(answer_sdp);
              qDebug() << "[P2pPlayer] SendAnswer 已调用 (字节" << answer_sdp.size() << ")";
              Q_EMIT statusChanged("Answer 已发送，ICE 候选交换中...");
            },
            [this](webrtc::RTCError er) {
              m_pendingSetLocalObserver = nullptr;
              qWarning() << "[P2pPlayer] SetLocalDescription OnFailure:"
                         << QString::fromStdString(er.message());
              Q_EMIT statusChanged(QString("SetLocalDescription 失败: %1")
                                       .arg(QString::fromStdString(er.message())));
            });
        qDebug() << "[P2pPlayer] 即将 SetLocalDescription(异步), observer 已挂成员";
        m_peerConnection->SetLocalDescription(m_pendingSetLocalObserver.get(), desc.release());
        qDebug() << "[P2pPlayer] SetLocalDescription 调用已返回(结果在回调)";
      },
      [this](webrtc::RTCError e) {
        m_pendingCreateAnswerObserver = nullptr;
        qWarning() << "[P2pPlayer] CreateAnswer OnFailure:"
                   << QString::fromStdString(e.message());
        Q_EMIT statusChanged(
            QString("CreateAnswer 失败: %1").arg(QString::fromStdString(e.message())));
      });
  qDebug() << "[P2pPlayer] 即将调用 CreateAnswer(异步)";
  m_peerConnection->CreateAnswer(m_pendingCreateAnswerObserver.get(),
                                   webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
  qDebug() << "[P2pPlayer] CreateAnswer 调用已返回(结果在回调)";
}

void WebRTCReceiverClient::addRemoteIceCandidateNow(const std::string &mid, int mline_index,
                                                      const std::string &candidate)
{
  if (!m_peerConnection)
    return;
  webrtc::SdpParseError err;
  webrtc::IceCandidateInterface *cand =
      webrtc::CreateIceCandidate(mid, mline_index, candidate, &err);
  if (!cand) {
    qWarning() << "CreateIceCandidate failed:" << QString::fromStdString(err.description)
               << "mid=" << QString::fromStdString(mid) << "mline=" << mline_index;
    return;
  }
  bool ok = m_peerConnection->AddIceCandidate(cand);
  delete cand;
  if (!ok)
    qWarning() << "AddIceCandidate returned false mid=" << QString::fromStdString(mid)
               << "mline=" << mline_index;
}

void WebRTCReceiverClient::flushPendingRemoteIceCandidates()
{
  if (m_pendingRemoteIce.empty())
    return;
  qDebug() << "[ICE] 应用暂存的远端候选，数量:" << static_cast<int>(m_pendingRemoteIce.size());
  for (const auto &p : m_pendingRemoteIce)
    addRemoteIceCandidateNow(p.mid, p.mline_index, p.candidate);
  m_pendingRemoteIce.clear();
}

void WebRTCReceiverClient::handleRemoteIceCandidate(const std::string &mid, int mline_index,
                                                    const std::string &candidate)
{
  if (!m_peerConnection) {
    m_pendingRemoteIce.push_back(PendingRemoteIce{mid, mline_index, candidate});
    qDebug() << "[ICE] 远端候选先入队(尚无 PeerConnection)，当前队列"
             << static_cast<int>(m_pendingRemoteIce.size());
    return;
  }
  if (!m_remoteDescriptionApplied) {
    m_pendingRemoteIce.push_back(PendingRemoteIce{mid, mline_index, candidate});
    qDebug() << "[ICE] 远端候选先入队(等待 SetRemoteDescription 完成)，当前队列"
             << static_cast<int>(m_pendingRemoteIce.size());
    return;
  }
  addRemoteIceCandidateNow(mid, mline_index, candidate);
}

void WebRTCReceiverClient::startStatsTimer()
{
  if (m_statsTimer)
    return;
  m_prevFramesDecoded = 0;
  m_prevTotalDecodeTime = 0.0;
  m_prevFramesReceived = 0;
  m_prevFramesDropped = 0;
  m_prevJitterBufferDelay = 0.0;
  m_prevJitterBufferEmitted = 0;

  m_statsTimer = new QTimer(this);
  m_statsTimer->setInterval(3000);
  connect(m_statsTimer, &QTimer::timeout, this, [this]() {
    if (!m_peerConnection)
      return;
    auto cb = webrtc::make_ref_counted<StatsCallback>(
        [this](const webrtc::scoped_refptr<const webrtc::RTCStatsReport> &report) {
          double currentRtt = 0.0;
          double totalRtt = 0.0;
          int64_t responsesReceived = 0;
          bool foundNominated = false;
          for (const auto *pair : report->GetStatsOfType<webrtc::RTCIceCandidatePairStats>()) {
            if (pair->nominated && *pair->nominated) {
              if (pair->current_round_trip_time)
                currentRtt = *pair->current_round_trip_time;
              if (pair->total_round_trip_time)
                totalRtt = *pair->total_round_trip_time;
              if (pair->responses_received)
                responsesReceived = static_cast<int64_t>(*pair->responses_received);
              foundNominated = true;
              break;
            }
          }
          (void)foundNominated;

          for (const auto *inb : report->GetStatsOfType<webrtc::RTCInboundRtpStreamStats>()) {
            if (!inb->kind || *inb->kind != "video")
              continue;

            uint32_t framesDecoded = inb->frames_decoded.value_or(0);
            double totalDecodeTime = inb->total_decode_time.value_or(0.0);
            double fps = inb->frames_per_second.value_or(0.0);
            uint32_t framesReceived = inb->frames_received.value_or(0);
            uint32_t framesDropped = inb->frames_dropped.value_or(0);
            double jitterBufferDelay = inb->jitter_buffer_delay.value_or(0.0);
            uint64_t jitterBufferEmitted = inb->jitter_buffer_emitted_count.value_or(0);
            int frameWidth = static_cast<int>(inb->frame_width.value_or(0));
            int frameHeight = static_cast<int>(inb->frame_height.value_or(0));
            uint64_t bytesReceived = inb->bytes_received.value_or(0);
            std::string decoderImpl = inb->decoder_implementation.value_or("");
            double jitter = inb->jitter.value_or(0.0);
            int64_t packetsLost = inb->packets_lost.value_or(0);
            int64_t packetsReceived = static_cast<int64_t>(inb->packets_received.value_or(0));
            int64_t nackCount = inb->nack_count.value_or(0);
            int64_t pliCount = inb->pli_count.value_or(0);
            int64_t firCount = inb->fir_count.value_or(0);
            int64_t fec_packets_received = inb->fec_packets_received.value_or(0);
            uint32_t deltaFrames = framesDecoded - m_prevFramesDecoded;
            double deltaTime = totalDecodeTime - m_prevTotalDecodeTime;
            double avgDecodeMs = (deltaFrames > 0) ? (deltaTime / deltaFrames * 1000.0) : 0.0;
            uint32_t deltaDropped = framesDropped - m_prevFramesDropped;

            uint64_t deltaJbEmitted = jitterBufferEmitted - m_prevJitterBufferEmitted;
            double deltaJbDelay = jitterBufferDelay - m_prevJitterBufferDelay;
            double avgJitterMs =
                (deltaJbEmitted > 0) ? (deltaJbDelay / deltaJbEmitted * 1000.0) : 0.0;

            double lossRate = (packetsLost + packetsReceived > 0)
                ? (packetsLost * 100.0 / (packetsLost + packetsReceived))
                : 0.0;
            double avgRttMs =
                (responsesReceived > 0) ? (totalRtt / responsesReceived * 1000.0) : 0.0;

            const double rttCurrentMs = currentRtt * 1000.0;
            QMetaObject::invokeMethod(
                this,
                [=]() {
                  m_rttCurrentMs = rttCurrentMs;
                  m_rttAvgMs = avgRttMs;
                  m_jitterBufferMs = avgJitterMs;
                  m_hasConnectionStats = true;
                  Q_EMIT connectionStatsChanged();

                  qDebug().noquote() << QString(
                      "[DecodeStats] %1x%2 | 解码器: %3 | fps: %4 | "
                      "解码帧: +%5 (总%6) | 平均解码: %7 ms/帧 | "
                      "丢帧: +%8 (总%9) | 抖动缓冲: %10 ms | "
                      "接收: %11 KB")
                      .arg(frameWidth)
                      .arg(frameHeight)
                      .arg(QString::fromStdString(decoderImpl))
                      .arg(fps, 0, 'f', 1)
                      .arg(deltaFrames)
                      .arg(framesDecoded)
                      .arg(avgDecodeMs, 0, 'f', 2)
                      .arg(deltaDropped)
                      .arg(framesDropped)
                      .arg(avgJitterMs, 0, 'f', 1)
                      .arg(bytesReceived / 1024);

                  qDebug().noquote() << QString(
                      "[NetStats] RTT: %1 ms (平均 %2 ms) | "
                      "抖动: %3 ms | 丢包率: %4% (%5/%6) | "
                      "NACK: %7 | PLI: %8 | FIR: %9 | FEC: %10")
                      .arg(rttCurrentMs, 0, 'f', 1)
                      .arg(avgRttMs, 0, 'f', 1)
                      .arg(jitter * 1000.0, 0, 'f', 1)
                      .arg(lossRate, 0, 'f', 2)
                      .arg(packetsLost)
                      .arg(packetsReceived)
                      .arg(nackCount)
                      .arg(pliCount)
                      .arg(firCount)
                      .arg(fec_packets_received);
                },
                Qt::QueuedConnection);

            m_prevFramesDecoded = framesDecoded;
            m_prevTotalDecodeTime = totalDecodeTime;
            m_prevFramesReceived = framesReceived;
            m_prevFramesDropped = framesDropped;
            m_prevJitterBufferDelay = jitterBufferDelay;
            m_prevJitterBufferEmitted = jitterBufferEmitted;
            break;
          }
        });
    m_peerConnection->GetStats(cb.get());
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
  resetConnectionStats();
}

void WebRTCReceiverClient::resetConnectionStats()
{
  m_rttCurrentMs = 0.0;
  m_rttAvgMs = 0.0;
  m_jitterBufferMs = 0.0;
  if (m_hasConnectionStats) {
    m_hasConnectionStats = false;
    Q_EMIT connectionStatsChanged();
  }
}
