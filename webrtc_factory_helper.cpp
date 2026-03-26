#include "webrtc_factory_helper.h"

#include <memory>

#include <QtGlobal>

#include "api/audio/audio_device.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/peer_connection_interface.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "rtc_base/thread.h"

namespace webrtc_demo {
namespace {

struct FactoryThreads {
  std::unique_ptr<webrtc::Thread> network;
  std::unique_ptr<webrtc::Thread> worker;
  std::unique_ptr<webrtc::Thread> signaling;
  bool started = false;
};

bool EnsureThreadsStarted(FactoryThreads& ctx) {
  if (ctx.started) {
    return true;
  }
  ctx.network = webrtc::Thread::CreateWithSocketServer();
  ctx.worker = webrtc::Thread::Create();
  ctx.signaling = webrtc::Thread::Create();
  if (!ctx.network || !ctx.worker || !ctx.signaling) {
    return false;
  }
  if (!ctx.network->Start() || !ctx.worker->Start() || !ctx.signaling->Start()) {
    return false;
  }
  ctx.started = true;
  return true;
}

FactoryThreads& Threads() {
  static FactoryThreads g;
  return g;
}

// Dummy ADM + TaskQueueFactory 与 Factory 同生命周期，避免 WebRtcVoiceEngine::Init()
// 在 ADM 为 nullptr 时崩溃；不访问真实麦克风/扬声器。
webrtc::scoped_refptr<webrtc::AudioDeviceModule> DummyAdm() {
  static std::unique_ptr<webrtc::TaskQueueFactory> task_queue_factory =
      webrtc::CreateDefaultTaskQueueFactory();
  static webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm =
      webrtc::AudioDeviceModule::Create(webrtc::AudioDeviceModule::kDummyAudio,
                                        task_queue_factory.get());
  return adm;
}

}  // namespace

webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> CreatePeerConnectionFactory() {
  FactoryThreads& th = Threads();
  if (!EnsureThreadsStarted(th)) {
    return nullptr;
  }

  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm = DummyAdm();
  if (!adm) {
    return nullptr;
  }

  // 音频：Dummy ADM + 内置编解码工厂；视频解码：全量 libwebrtc 自带的 CreateBuiltinVideoDecoderFactory。
  // 仅接收端可不设视频编码工厂。
  // 第三参数传专用 signaling 线程；传 nullptr 时与 network 混用，在 Qt 主线程发起 PC 调用时易出现
  // CreateAnswer 立即返回但 CreateSessionDescriptionObserver 永不回调等问题。
  return webrtc::CreatePeerConnectionFactory(
      th.network.get(), th.worker.get(), th.signaling.get(),
      adm,
      webrtc::CreateBuiltinAudioEncoderFactory(),
      webrtc::CreateBuiltinAudioDecoderFactory(),
      nullptr,
      webrtc::CreateBuiltinVideoDecoderFactory(),
      nullptr,
      nullptr);
}

webrtc::Thread *PeerConnectionFactoryNetworkThread() {
  FactoryThreads &th = Threads();
  if (!EnsureThreadsStarted(th))
    return nullptr;
  return th.network.get();
}

webrtc::Thread *PeerConnectionFactorySignalingThread() {
  FactoryThreads &th = Threads();
  if (!EnsureThreadsStarted(th))
    return nullptr;
  return th.signaling.get();
}

}  // namespace webrtc_demo
