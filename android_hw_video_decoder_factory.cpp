#include "android_hw_video_decoder_factory.h"

#include <android/log.h>
#include <media/NdkMediaCodec.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "android_mediacodec_video_decoder.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "media/base/media_constants.h"

namespace webrtc_demo {
namespace {

#define LOG_TAG "HwDecFactory"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

std::once_flag g_probe_once;
std::atomic<bool> g_h264_decoder_usable{false};

template <typename MapT>
std::string CodecParametersToString(const MapT& parameters) {
  std::string result;
  bool first = true;
  for (const auto& entry : parameters) {
    if (!first) {
      result += ';';
    }
    first = false;
    result += entry.first;
    result += '=';
    result += entry.second;
  }
  return result;
}

void ProbeH264Once() {
  AMediaCodec* c = AMediaCodec_createDecoderByType("video/avc");
  if (c) {
    AMediaCodec_delete(c);
    g_h264_decoder_usable.store(true, std::memory_order_relaxed);
    ALOGI("ProbeH264Once: MediaCodec H264 decoder available");
  } else {
    g_h264_decoder_usable.store(false, std::memory_order_relaxed);
    ALOGI("ProbeH264Once: MediaCodec H264 decoder unavailable");
  }
}

bool H264MediaCodecAvailable() {
  std::call_once(g_probe_once, ProbeH264Once);
  return g_h264_decoder_usable.load(std::memory_order_relaxed);
}

}  // namespace

class AndroidHwOrBuiltinVideoDecoderFactory : public webrtc::VideoDecoderFactory {
 public:
  AndroidHwOrBuiltinVideoDecoderFactory()
      : builtin_(webrtc::CreateBuiltinVideoDecoderFactory()) {}

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
    return builtin_->GetSupportedFormats();
  }

  webrtc::VideoDecoderFactory::CodecSupport QueryCodecSupport(
      const webrtc::SdpVideoFormat& format,
      bool reference_scaling) const override {
    if (!reference_scaling && format.name == webrtc::kH264CodecName && H264MediaCodecAvailable()) {
      webrtc::VideoDecoderFactory::CodecSupport support;
      support.is_supported = true;
      support.is_power_efficient = true;
      return support;
    }
    return builtin_->QueryCodecSupport(format, reference_scaling);
  }

  std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment& env,
                                                 const webrtc::SdpVideoFormat& format) override {
    const std::string params = CodecParametersToString(format.parameters);
    if (format.name == webrtc::kH264CodecName && H264MediaCodecAvailable()) {
      ALOGI("Create decoder: codec=%s params=%s -> AndroidMediaCodecVideoDecoder",
            format.name.c_str(), params.c_str());
      return std::make_unique<AndroidMediaCodecVideoDecoder>();
    }
    ALOGI("Create decoder: codec=%s params=%s -> builtin factory",
          format.name.c_str(), params.c_str());
    return builtin_->Create(env, format);
  }

 private:
  std::unique_ptr<webrtc::VideoDecoderFactory> builtin_;
};

std::unique_ptr<webrtc::VideoDecoderFactory> CreateAndroidHwOrBuiltinVideoDecoderFactory() {
  return std::make_unique<AndroidHwOrBuiltinVideoDecoderFactory>();
}

}  // namespace webrtc_demo
