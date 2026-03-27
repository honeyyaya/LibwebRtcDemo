#pragma once

#include <memory>

#include "api/video_codecs/video_decoder.h"

namespace webrtc_demo {

// Android NDK AMediaCodec-backed H.264 decoder; decode work runs on an internal thread.
class AndroidMediaCodecVideoDecoder : public webrtc::VideoDecoder {
 public:
  AndroidMediaCodecVideoDecoder();
  ~AndroidMediaCodecVideoDecoder() override;

  bool Configure(const Settings& settings) override;
  int32_t Decode(const webrtc::EncodedImage& input_image,
                 bool missing_frames,
                 int64_t render_time_ms) override;
  int32_t RegisterDecodeCompleteCallback(
      webrtc::DecodedImageCallback* callback) override;
  int32_t Release() override;
  DecoderInfo GetDecoderInfo() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace webrtc_demo
