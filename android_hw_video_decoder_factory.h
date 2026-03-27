#pragma once

#include <memory>

#include "api/video_codecs/video_decoder_factory.h"

namespace webrtc_demo {

// H.264 优先走 NDK MediaCodec，其余格式委托内置工厂（与 CreateBuiltinVideoDecoderFactory 合并能力）。
std::unique_ptr<webrtc::VideoDecoderFactory>
CreateAndroidHwOrBuiltinVideoDecoderFactory();

}  // namespace webrtc_demo
