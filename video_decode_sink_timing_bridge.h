#pragma once

#include <chrono>
#include <cstdint>

namespace webrtc_demo {

// 与 android_mediacodec_video_decoder.cpp 中 McMonotonicUs 一致：steady_clock 微秒。
inline int64_t DecodeSinkMonotonicUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 在自定义解码器里于 cb->Decoded() 返回后立即调用（Android MediaCodec 路径）。
void DecodeSinkRecordAfterDecoded(uint32_t rtp_timestamp,
                                  int64_t decoded_return_monotonic_us);

// 在应用 sink（如 WebRTCVideoRenderer::OnFrame）入口调用：若存在记录则取出并删除。
// 返回 false 表示无记录（软解路径、丢包、或 rtp_ts 未匹配）。
bool DecodeSinkTakeDecodedReturn(uint32_t rtp_timestamp, int64_t* out_decoded_return_monotonic_us);

}  // namespace webrtc_demo
