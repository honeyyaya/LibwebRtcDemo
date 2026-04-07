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

}  // namespace webrtc_demo

#endif
