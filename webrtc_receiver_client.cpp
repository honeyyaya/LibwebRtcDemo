#include "webrtc_receiver_client.h"
#include "webrtc_factory_helper.h"
#include "webrtc_video_renderer.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <QDateTime>
#include <QDebug>
#include <QMetaObject>
#include <QPointer>
#include <QStringList>
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
#include "api/video_codecs/h264_profile_level_id.h"
#include "system_wrappers/include/field_trial.h"

#ifdef Q_OS_ANDROID
#include <QJniObject>
#endif

#ifndef DEFAULT_SIGNALING_ADDR
#define DEFAULT_SIGNALING_ADDR "192.168.3.20:8765"
#endif

#define VERIFY_LOG(tag, msg) qDebug() << "[VERIFY-" tag "]" << msg

namespace {

// Carry a small NetEQ floor so a single packet loss has time to be repaired
// by NACK retransmission before the frame is forwarded to the decoder.
// Targeting steady-state jb_avg ~25-35 ms: 20 ms floor lets clean frames
// glide through quickly while still surviving one short retransmission on
// a typical LAN / WiFi RTT. Setting this to 0 produced visible corruption.
constexpr double kReceiverVideoJitterBufferMinDelaySeconds = 0.02;  // 20 ms
// Hard cap NetEQ playout delay to [min, max] ms. min lets clean frames flow
// through immediately; max bounds the worst-case latency we will tolerate.
// 60 ms is roughly one LAN RTT plus a vsync of slack -- enough headroom for
// a single NACK retransmission to land before the frame is emitted, while
// keeping the steady-state target at the low end of the [20, 60] window.
constexpr int kReceiverForcedPlayoutMinMs = 0;
constexpr int kReceiverForcedPlayoutMaxMs = 60;
// Pace ZeroPlayoutDelay flushes at ~half a 60 fps frame interval. 2 ms is
// the documented sweet spot: low enough to not stall at high frame rates,
// high enough to coalesce bursts and avoid spin-loop pacer wakeups.
constexpr int kReceiverZeroPlayoutMinPacingMs = 2;
// Decode queue depth in ZeroPlayoutDelay mode. 3 keeps the smallest ring
// that still tolerates a normal amount of out-of-order delivery: 2 frames
// for the jitter buffer to slot retransmissions into plus 1 for the decoder
// in flight. Going to 2 here makes any reordered packet a frame drop.
constexpr int kReceiverMaxDecodeQueueSize = 3;
constexpr bool kReceiverEnablePacerFastRetransmissions = true;
// Enable FlexFEC-03 reception. Advertise the codec in our Answer SDP so the
// sender can opt in, and turn on the receive-side FEC depacketizer. FlexFEC
// is per-packet redundancy, so it complements (not replaces) NACK and lets
// us recover single-packet drops without spending an RTT on retransmission.
// Receiver-side has no downside if the sender does not send FEC; advertise
// is essentially free.
constexpr bool kReceiverEnableFlexFec = true;
// If we go this long without a newly decoded frame while the connection is
// live, kick the receiver-side jitter buffer to force WebRTC to re-evaluate
// keyframe / NACK timers. This is a belt-and-braces backup for cases where
// PLI was sent but the sender failed to respond (e.g. WAN drops, dropped
// RTCP). The hop briefly nudges minimum delay up and back to the configured
// floor; the perceived latency increase is < 1 stats interval.
//
// 300 ms pairs with the 30-35 ms steady-state target: tight enough that the
// user does not perceive a hard freeze, loose enough that a single jittered
// frame on a clean network does not trip the watchdog. The cooldown stays
// at 2x this value (~600 ms) to let the regenerated IDR cross the network.
constexpr int kReceiverNoNewFrameKeyframeWatchdogMs = 300;

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

template <typename MapT>
QString CodecParametersToQString(const MapT& parameters) {
  QStringList parts;
  for (const auto& entry : parameters) {
    parts.push_back(QString("%1=%2")
                        .arg(QString::fromStdString(entry.first),
                             QString::fromStdString(entry.second)));
  }
  return parts.join(';');
}

QString H264ProfileToQString(webrtc::H264Profile profile) {
  switch (profile) {
    case webrtc::H264Profile::kProfileBaseline:
      return "baseline";
    case webrtc::H264Profile::kProfileConstrainedBaseline:
      return "constrained-baseline";
    case webrtc::H264Profile::kProfileMain:
      return "main";
    case webrtc::H264Profile::kProfileConstrainedHigh:
      return "constrained-high";
    case webrtc::H264Profile::kProfileHigh:
      return "high";
    case webrtc::H264Profile::kProfilePredictiveHigh444:
      return "predictive-high-444";
  }
  return "unknown";
}

QString DescribeCodecCapability(const webrtc::RtpCodecCapability& codec) {
  QStringList parts;
  parts.push_back(codec.IsMediaCodec() ? "media" : "aux");
  parts.push_back(QString::fromStdString(codec.name));
  if (codec.preferred_payload_type.has_value()) {
    parts.push_back(QString("pt=%1").arg(*codec.preferred_payload_type));
  }
  if (!codec.parameters.empty()) {
    parts.push_back(QString("fmtp=%1").arg(CodecParametersToQString(codec.parameters)));
  }
  if (codec.name == "H264") {
    const std::optional<webrtc::H264ProfileLevelId> profile =
        webrtc::ParseSdpForH264ProfileLevelId(codec.parameters);
    if (profile.has_value()) {
      parts.push_back(QString("profile=%1").arg(H264ProfileToQString(profile->profile)));
    } else {
      parts.push_back("profile=parse-failed");
    }
  }
  return parts.join(" | ");
}

void LogCodecCapabilityList(const char* tag,
                            const std::vector<webrtc::RtpCodecCapability>& codecs) {
  qDebug().noquote() << QString("[%1] codec_count=%2").arg(tag).arg(codecs.size());
  int index = 0;
  for (const auto& codec : codecs) {
    qDebug().noquote()
        << QString("[%1] #%2 %3").arg(tag).arg(index++).arg(DescribeCodecCapability(codec));
  }
}

void LogReceiverVideoCapabilities(const webrtc::RtpCapabilities& capabilities) {
  std::vector<webrtc::RtpCodecCapability> video_codecs;
  for (const auto& codec : capabilities.codecs) {
    if (codec.kind == webrtc::MediaType::VIDEO) {
      video_codecs.push_back(codec);
    }
  }
  LogCodecCapabilityList("CodecCaps", video_codecs);
}

void LogVideoSdpSummary(const char* tag, const std::string& sdp) {
  const QStringList lines =
      QString::fromStdString(sdp).split('\n', Qt::SkipEmptyParts);
  bool in_video_section = false;
  int emitted = 0;
  qDebug().noquote() << QString("[%1] ===== video sdp summary begin =====").arg(tag);
  for (const QString& raw_line : lines) {
    const QString line = raw_line.trimmed();
    if (line.startsWith("m=")) {
      in_video_section = line.startsWith("m=video ");
      if (in_video_section) {
        qDebug().noquote() << QString("[%1] %2").arg(tag, line);
        ++emitted;
      }
      continue;
    }
    if (!in_video_section) {
      continue;
    }
    if (line.startsWith("a=rtpmap:") || line.startsWith("a=fmtp:") ||
        line.startsWith("a=rtcp-fb:") || line.startsWith("a=mid:") ||
        line.startsWith("a=rtcp-mux") || line.startsWith("a=rtcp-rsize") ||
        line == "a=recvonly" || line == "a=sendrecv" || line == "a=sendonly" ||
        line == "a=inactive") {
      qDebug().noquote() << QString("[%1] %2").arg(tag, line);
      ++emitted;
    }
  }
  if (emitted == 0) {
    qDebug().noquote() << QString("[%1] no video section found").arg(tag);
  }
  qDebug().noquote() << QString("[%1] ===== video sdp summary end =====").arg(tag);
}

bool IsLowLatencyH264Capability(const webrtc::RtpCodecCapability& codec) {
  if (codec.kind != webrtc::MediaType::VIDEO || codec.name != "H264") {
    return false;
  }
  const std::optional<webrtc::H264ProfileLevelId> profile =
      webrtc::ParseSdpForH264ProfileLevelId(codec.parameters);
  if (!profile.has_value()) {
    return false;
  }
  return profile->profile == webrtc::H264Profile::kProfileConstrainedBaseline ||
         profile->profile == webrtc::H264Profile::kProfileBaseline;
}

std::vector<webrtc::RtpCodecCapability> BuildLowLatencyVideoCodecPreferences(
    const webrtc::RtpCapabilities& capabilities) {
  std::vector<webrtc::RtpCodecCapability> preferred_h264_codecs;
  std::vector<webrtc::RtpCodecCapability> compatible_h264_codecs;
  std::vector<webrtc::RtpCodecCapability> fallback_media_codecs;
  std::vector<webrtc::RtpCodecCapability> auxiliary_codecs;
  std::vector<int> allowed_payload_types;

  for (const auto& codec : capabilities.codecs) {
    if (codec.kind != webrtc::MediaType::VIDEO) {
      continue;
    }
    if (codec.IsMediaCodec()) {
      if (IsLowLatencyH264Capability(codec)) {
        preferred_h264_codecs.push_back(codec);
      } else if (codec.name == "H264") {
        compatible_h264_codecs.push_back(codec);
      } else {
        fallback_media_codecs.push_back(codec);
      }
      continue;
    }
    auxiliary_codecs.push_back(codec);
  }

  if (preferred_h264_codecs.empty() && compatible_h264_codecs.empty()) {
    return {};
  }

  for (const auto& codec : preferred_h264_codecs) {
    if (codec.preferred_payload_type.has_value()) {
      allowed_payload_types.push_back(*codec.preferred_payload_type);
    }
  }
  for (const auto& codec : compatible_h264_codecs) {
    if (codec.preferred_payload_type.has_value()) {
      allowed_payload_types.push_back(*codec.preferred_payload_type);
    }
  }
  for (const auto& codec : fallback_media_codecs) {
    if (codec.preferred_payload_type.has_value()) {
      allowed_payload_types.push_back(*codec.preferred_payload_type);
    }
  }

  std::vector<webrtc::RtpCodecCapability> result = preferred_h264_codecs;
  result.insert(result.end(), compatible_h264_codecs.begin(), compatible_h264_codecs.end());
  result.insert(result.end(), fallback_media_codecs.begin(), fallback_media_codecs.end());
  for (const auto& codec : auxiliary_codecs) {
    if (codec.name == "rtx") {
      const auto apt_it = codec.parameters.find("apt");
      if (apt_it == codec.parameters.end()) {
        continue;
      }
      const int apt = std::atoi(apt_it->second.c_str());
      if (std::find(allowed_payload_types.begin(), allowed_payload_types.end(), apt) ==
          allowed_payload_types.end()) {
        continue;
      }
    }
    result.push_back(codec);
  }

  return result;
}

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
    // Keep a small playout cushion while allowing the receiver to adapt.
    receiver->SetJitterBufferMinimumDelay(
        std::optional<double>(kReceiverVideoJitterBufferMinDelaySeconds));
    qDebug().noquote()
        << QString("[P2pPlayer] video jitter min delay target set to %1 ms")
               .arg(kReceiverVideoJitterBufferMinDelaySeconds * 1000.0, 0, 'f', 1);
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
  // static std::string g_field_trials_storage ="WebRTC-VideoFrameTrackingIdAdvertised/Enabled/";
  // g_field_trials_storage +="WebRTC-ForcePlayoutDelay/min_ms:100,max_ms:100/";
  // g_field_trials_storage += "WebRTC-ZeroPlayoutDelay/min_pacing:4ms,max_decode_queue_size:6/";
  // g_field_trials_storage +="WebRTC-Pacer-KeyframeFlushing/Enabled/";
  // g_field_trials_storage +="WebRTC-Pacer-FastRetransmissions/Enabled/";

  static const std::string g_field_trials_storage = [] {
    std::string s = "WebRTC-VideoFrameTrackingIdAdvertised/Enabled/";
    // Cap NetEQ playout delay to [min, max] ms instead of pinning it to a
    // fixed value. min=0,max=30 lets the receiver follow the wire while still
    // tolerating short bursts of jitter.
    if (kReceiverForcedPlayoutMaxMs > 0 &&
        kReceiverForcedPlayoutMinMs <= kReceiverForcedPlayoutMaxMs) {
      s += "WebRTC-ForcePlayoutDelay/min_ms:" +
           std::to_string(kReceiverForcedPlayoutMinMs) + ",max_ms:" +
           std::to_string(kReceiverForcedPlayoutMaxMs) + "/";
    }
    s += "WebRTC-ZeroPlayoutDelay/min_pacing:" +
         std::to_string(kReceiverZeroPlayoutMinPacingMs) +
         "ms,max_decode_queue_size:" +
         std::to_string(kReceiverMaxDecodeQueueSize) + "/";
    s += "WebRTC-Pacer-KeyframeFlushing/Enabled/";
    if (kReceiverEnablePacerFastRetransmissions) {
      s += "WebRTC-Pacer-FastRetransmissions/Enabled/";
    }
    if (kReceiverEnableFlexFec) {
      // -Advertised: include flexfec-03 in the Answer SDP capability list.
      // Without this trial WebRTC strips FEC codecs from the offer side, so
      // the sender never sees that we can consume them.
      s += "WebRTC-FlexFEC-03-Advertised/Enabled/";
      // (no -Advertised suffix): turn on receive-side FlexFEC depacketization
      // and recovery. Pairs with the codec advertised above.
      s += "WebRTC-FlexFEC-03/Enabled/";
    }
    return s;
  }();
  static bool field_trials_inited = false;
  if (!field_trials_inited) {
      webrtc::field_trial::InitFieldTrialsFromString(g_field_trials_storage.c_str());
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
  log_trial("WebRTC-ForcePlayoutDelay");
  log_trial("WebRTC-VideoFrameTrackingIdAdvertised");
  log_trial("WebRTC-FlexFEC-03-Advertised");
  log_trial("WebRTC-FlexFEC-03");
  log_trial("WebRTC-Pacer-KeyframeFlushing");
  log_trial("WebRTC-Pacer-FastRetransmissions");
  VERIFY_LOG("2", QString("FieldTrial WebRTC-ZeroPlayoutDelay: fullName=\"%1\"")
                      .arg(QString::fromStdString(
                          webrtc::field_trial::FindFullName("WebRTC-ZeroPlayoutDelay"))));
  const QString playoutMode =
      kReceiverForcedPlayoutMaxMs > 0
          ? QString("range-%1..%2ms")
                .arg(kReceiverForcedPlayoutMinMs)
                .arg(kReceiverForcedPlayoutMaxMs)
          : QStringLiteral("dynamic");
  qDebug().noquote()
      << QString("[Pipeline/Config] playout_mode=%1 | jitter_min=%2 ms | "
                 "min_pacing=%3 ms | max_decode_queue=%4 | fast_retx=%5 | fec=%6")
             .arg(playoutMode)
             .arg(kReceiverVideoJitterBufferMinDelaySeconds * 1000.0, 0, 'f', 1)
             .arg(kReceiverZeroPlayoutMinPacingMs)
             .arg(kReceiverMaxDecodeQueueSize)
             .arg(kReceiverEnablePacerFastRetransmissions ? "on" : "off")
             .arg(kReceiverEnableFlexFec ? "flexfec-03" : "off");

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
  config.audio_jitter_buffer_max_packets = 1;
  // 音频 NetEq：最小目标延迟（ms），默认即为 0；显式写出便于与视频 RtpReceiver 策略一致。
  config.audio_jitter_buffer_min_delay_ms = 0;
  config.audio_jitter_buffer_fast_accelerate = true;
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

  LogVideoSdpSummary("OfferSDP", sdp);
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

  if (m_factory) {
    const webrtc::RtpCapabilities capabilities =
        m_factory->GetRtpReceiverCapabilities(webrtc::MediaType::VIDEO);
    LogReceiverVideoCapabilities(capabilities);
    std::vector<webrtc::RtpCodecCapability> preferred_codecs =
        BuildLowLatencyVideoCodecPreferences(capabilities);
    if (preferred_codecs.empty()) {
      qWarning() << "[CodecPrefs] preferred list empty; SetCodecPreferences skipped";
    } else {
      LogCodecCapabilityList("CodecPrefs", preferred_codecs);
    }
    if (!preferred_codecs.empty()) {
      for (const auto& transceiver : m_peerConnection->GetTransceivers()) {
        if (!transceiver || transceiver->media_type() != webrtc::MediaType::VIDEO) {
          continue;
        }
        qDebug() << "[CodecPrefs] applying to video transceiver";
        const webrtc::RTCError error = transceiver->SetCodecPreferences(preferred_codecs);
        if (!error.ok()) {
          qWarning() << "[P2pPlayer] SetCodecPreferences failed:"
                     << QString::fromStdString(error.message());
        } else {
          qDebug() << "[CodecPrefs] SetCodecPreferences OK";
        }
      }
    }
  }

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

        LogVideoSdpSummary("AnswerSDP", answer_sdp);
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
  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
  options.offer_to_receive_audio = 0;
  options.num_simulcast_layers = 1;
  m_peerConnection->CreateAnswer(m_pendingCreateAnswerObserver.get(), options);
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
  m_prevTotalProcessingDelay = 0.0;
  m_prevTotalAssemblyTime = 0.0;
  m_prevFramesReceived = 0;
  m_prevFramesDropped = 0;
  m_prevJitterBufferDelay = 0.0;
  m_prevJitterBufferEmitted = 0;
  m_lastDecodeProgressMonoMs = QDateTime::currentMSecsSinceEpoch();
  m_lastKeyframeKickPacketsReceived = 0;
  m_lastKeyframeKickMonoMs = 0;

  m_statsTimer = new QTimer(this);
  // 1 s gives the keyframe watchdog a fresh signal every second while still
  // keeping the [Pipeline/...] log output sparse enough to read by hand.
  m_statsTimer->setInterval(1000);
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
            double totalProcessingDelay = inb->total_processing_delay.value_or(0.0);
            double totalAssemblyTime = inb->total_assembly_time.value_or(0.0);
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
            std::string codecMime;
            std::string codecFmtp;
            uint32_t codecPayloadType = 0;
            bool hasCodecPayloadType = false;
            if (inb->codec_id.has_value() && !inb->codec_id->empty()) {
              if (const auto* codec =
                      report->GetAs<webrtc::RTCCodecStats>(*inb->codec_id)) {
                codecMime = codec->mime_type.value_or("");
                codecFmtp = codec->sdp_fmtp_line.value_or("");
                if (codec->payload_type.has_value()) {
                  codecPayloadType = *codec->payload_type;
                  hasCodecPayloadType = true;
                }
              }
            }
            uint32_t deltaFrames = framesDecoded - m_prevFramesDecoded;
            double deltaTime = totalDecodeTime - m_prevTotalDecodeTime;
            double avgDecodeMs = (deltaFrames > 0) ? (deltaTime / deltaFrames * 1000.0) : 0.0;
            double deltaProcessingDelay = totalProcessingDelay - m_prevTotalProcessingDelay;
            double avgProcessingMs =
                (deltaFrames > 0) ? (deltaProcessingDelay / deltaFrames * 1000.0) : 0.0;
            double deltaAssemblyTime = totalAssemblyTime - m_prevTotalAssemblyTime;
            double avgAssemblyMs =
                (deltaFrames > 0) ? (deltaAssemblyTime / deltaFrames * 1000.0) : 0.0;
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

                  const QString codecLabel =
                      QString::fromStdString(codecMime.empty() ? std::string("unknown")
                                                               : codecMime) +
                      (hasCodecPayloadType ? QString(" pt=%1").arg(codecPayloadType) : QString());
                  qDebug().noquote() << QString(
                      "[Pipeline/Video] %1x%2 | decoder=%3 | codec=%4 | fps=%5 | "
                      "decoded=+%6 total=%7 | dropped=+%8 total=%9 | recv=%10 KB")
                      .arg(frameWidth)
                      .arg(frameHeight)
                      .arg(QString::fromStdString(decoderImpl))
                      .arg(codecLabel)
                      .arg(fps, 0, 'f', 1)
                      .arg(deltaFrames)
                      .arg(framesDecoded)
                      .arg(deltaDropped)
                      .arg(framesDropped)
                      .arg(bytesReceived / 1024);
                  qDebug().noquote() << QString(
                      "[Pipeline/Latency] jb_avg=%1 ms | decode_avg=%2 ms | "
                      "processing_avg=%3 ms | assembly_avg=%4 ms | playout_mode=%5 | "
                      "jitter_min=%6 ms")
                      .arg(avgJitterMs, 0, 'f', 1)
                      .arg(avgDecodeMs, 0, 'f', 2)
                      .arg(avgProcessingMs, 0, 'f', 2)
                      .arg(avgAssemblyMs, 0, 'f', 2)
                      .arg(kReceiverForcedPlayoutMaxMs > 0
                               ? QString("range-%1..%2ms")
                                     .arg(kReceiverForcedPlayoutMinMs)
                                     .arg(kReceiverForcedPlayoutMaxMs)
                               : QStringLiteral("dynamic"))
                      .arg(kReceiverVideoJitterBufferMinDelaySeconds * 1000.0, 0, 'f', 1);
                  if (!codecFmtp.empty()) {
                    qDebug().noquote()
                        << QString("[Pipeline/Codec] fmtp=%1")
                               .arg(QString::fromStdString(codecFmtp));
                  }

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
                  qDebug().noquote() << QString(
                      "[Pipeline/Net] rtt=%1 ms | rtt_avg=%2 ms | jitter=%3 ms | "
                      "loss=%4% (%5/%6) | nack=%7 | pli=%8 | fir=%9 | fec=%10")
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

            // ---- Keyframe-recovery watchdog ----
            // Symptom: packets are still being received but the decoder has
            // stopped producing frames -> the reference chain is broken
            // (typical after a packet loss the NACK could not repair). The
            // belt-and-braces remedy is to briefly raise the receiver's
            // jitter-buffer minimum delay; WebRTC's RtpVideoStreamReceiver
            // re-evaluates frame state on that change and will issue PLI/FIR
            // if it detects the gap, recovering the IDR without a full
            // PeerConnection restart.
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (deltaFrames > 0) {
              m_lastDecodeProgressMonoMs = nowMs;
            } else if (m_lastDecodeProgressMonoMs == 0) {
              m_lastDecodeProgressMonoMs = nowMs;
            }
            const qint64 stuckMs = nowMs - m_lastDecodeProgressMonoMs;
            const bool packetsStillFlowing =
                static_cast<uint64_t>(packetsReceived) >
                m_lastKeyframeKickPacketsReceived;
            const bool kickCooldownElapsed =
                m_lastKeyframeKickMonoMs == 0 ||
                (nowMs - m_lastKeyframeKickMonoMs) >=
                    kReceiverNoNewFrameKeyframeWatchdogMs * 2;
            if (deltaFrames == 0 && packetsStillFlowing &&
                stuckMs >= kReceiverNoNewFrameKeyframeWatchdogMs &&
                kickCooldownElapsed && m_peerConnection) {
              QMetaObject::invokeMethod(
                  this,
                  [this, stuckMs, packetsReceived]() {
                    if (!m_peerConnection)
                      return;
                    qWarning().noquote()
                        << QString("[KeyframeWatchdog] decoder stalled %1 ms "
                                   "(packets_received=%2, packets_lost_total=%3); "
                                   "kicking jitter buffer to force PLI")
                               .arg(stuckMs)
                               .arg(packetsReceived)
                               .arg(static_cast<qulonglong>(0));
                    for (const auto& transceiver :
                         m_peerConnection->GetTransceivers()) {
                      if (!transceiver ||
                          transceiver->media_type() != webrtc::MediaType::VIDEO) {
                        continue;
                      }
                      auto receiver = transceiver->receiver();
                      if (!receiver)
                        continue;
                      // Bump min delay way up momentarily, then drop it back to
                      // the configured floor. The transition forces WebRTC's
                      // VCMReceiver to re-arm keyframe / NACK timers.
                      receiver->SetJitterBufferMinimumDelay(
                          std::optional<double>(0.3));
                      receiver->SetJitterBufferMinimumDelay(
                          std::optional<double>(
                              kReceiverVideoJitterBufferMinDelaySeconds));
                    }
                  },
                  Qt::QueuedConnection);
              m_lastKeyframeKickMonoMs = nowMs;
              m_lastKeyframeKickPacketsReceived =
                  static_cast<uint64_t>(packetsReceived);
            } else if (deltaFrames > 0) {
              // Once decoding resumes, re-baseline the kick gate so the next
              // genuine stall is detected promptly.
              m_lastKeyframeKickPacketsReceived =
                  static_cast<uint64_t>(packetsReceived);
            }

            m_prevFramesDecoded = framesDecoded;
            m_prevTotalDecodeTime = totalDecodeTime;
            m_prevTotalProcessingDelay = totalProcessingDelay;
            m_prevTotalAssemblyTime = totalAssemblyTime;
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
  qDebug() << "[Pipeline/Stats] stats timer started (1s)";
  qDebug() << "[DecodeStats] 统计定时器已启动 (每 1s)";
}

void WebRTCReceiverClient::stopStatsTimer()
{
  if (m_statsTimer) {
    m_statsTimer->stop();
    m_statsTimer->deleteLater();
    m_statsTimer = nullptr;
    qDebug() << "[DecodeStats] 统计定时器已停止";
  }
  qDebug() << "[Pipeline/Stats] stats timer stopped";
  resetConnectionStats();
}

void WebRTCReceiverClient::resetConnectionStats()
{
  m_rttCurrentMs = 0.0;
  m_rttAvgMs = 0.0;
  m_jitterBufferMs = 0.0;
  m_lastDecodeProgressMonoMs = 0;
  m_lastKeyframeKickPacketsReceived = 0;
  m_lastKeyframeKickMonoMs = 0;
  m_prevFramesDecoded = 0;
  m_prevTotalDecodeTime = 0.0;
  m_prevTotalProcessingDelay = 0.0;
  m_prevTotalAssemblyTime = 0.0;
  m_prevFramesReceived = 0;
  m_prevFramesDropped = 0;
  m_prevJitterBufferDelay = 0.0;
  m_prevJitterBufferEmitted = 0;
  if (m_hasConnectionStats) {
    m_hasConnectionStats = false;
    Q_EMIT connectionStatsChanged();
  }
}
