#include "encoded_tracking_bridge.h"

#include <atomic>

namespace webrtc_demo {

namespace {

// -1：尚未收到带 tracking 的帧；0..65535：最后一帧的 VideoFrameTrackingId。
std::atomic<int32_t> g_last_encoded_tracking_id{-1};

}  // namespace

void RecordEncodedFrameTrackingId(std::optional<uint16_t> id) {
  if (id.has_value()) {
    g_last_encoded_tracking_id.store(static_cast<int32_t>(*id), std::memory_order_relaxed);
  }
}

int32_t GetLastEncodedFrameTrackingIdForUi() {
  return g_last_encoded_tracking_id.load(std::memory_order_relaxed);
}

void ResetEncodedFrameTrackingForUi() {
  g_last_encoded_tracking_id.store(-1, std::memory_order_relaxed);
}

}  // namespace webrtc_demo
