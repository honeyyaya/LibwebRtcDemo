#ifndef ENCODED_TRACKING_BRIDGE_H
#define ENCODED_TRACKING_BRIDGE_H

#include <cstdint>
#include <optional>  // IWYU: RecordEncodedFrameTrackingId

namespace webrtc_demo {

// 由 Android MediaCodec Decode() 等编码入站路径调用（与 log 中 EncodedFrame 一致）。
void RecordEncodedFrameTrackingId(std::optional<uint16_t> id);

// 供 UI 线程轮询：无有效 id 时为 -1。
int32_t GetLastEncodedFrameTrackingIdForUi();

void ResetEncodedFrameTrackingForUi();

// 与信令/服务端对齐的采样：仅当存在 tracking id 且 (id % 120)==0 时打【耗时分析】；id 用 uint32_t 取模（可 >65535）。
inline bool ShouldLogTrackingTimedSampleById(uint32_t id) {
  return (id % 120u) == 0u;
}

}  // namespace webrtc_demo

#endif
