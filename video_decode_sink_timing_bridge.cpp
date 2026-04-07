#include "video_decode_sink_timing_bridge.h"

#include "encoded_tracking_bridge.h"

#include <mutex>
#include <unordered_map>

namespace webrtc_demo {
namespace {

std::mutex g_mu;
std::unordered_map<uint32_t, int64_t> g_decoded_return_us;
std::unordered_map<uint32_t, int64_t> g_decode_pipeline_start_us;
constexpr size_t kMaxEntries = 128;

}  // namespace

void DecodeSinkRecordAfterDecoded(uint32_t rtp_timestamp,
                                  int64_t decoded_return_monotonic_us) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_decoded_return_us.size() >= kMaxEntries) {
    g_decoded_return_us.clear();
  }
  g_decoded_return_us[rtp_timestamp] = decoded_return_monotonic_us;
}

bool DecodeSinkTakeDecodedReturn(uint32_t rtp_timestamp,
                                 int64_t* out_decoded_return_monotonic_us) {
  if (!out_decoded_return_monotonic_us) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_decoded_return_us.find(rtp_timestamp);
  if (it == g_decoded_return_us.end()) {
    return false;
  }
  *out_decoded_return_monotonic_us = it->second;
  g_decoded_return_us.erase(it);
  return true;
}

void RecordDecodePipelineStartForE2eIfSampled(uint32_t tracking_id) {
  if (!ShouldLogTrackingTimedSampleById(tracking_id)) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_decode_pipeline_start_us.size() >= kMaxEntries) {
    g_decode_pipeline_start_us.clear();
  }
  g_decode_pipeline_start_us[tracking_id] = DecodeSinkMonotonicUs();
}

bool TakeDecodePipelineStartMonotonicUs(uint32_t tracking_id, int64_t* out_start_us) {
  if (!out_start_us) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_decode_pipeline_start_us.find(tracking_id);
  if (it == g_decode_pipeline_start_us.end()) {
    return false;
  }
  *out_start_us = it->second;
  g_decode_pipeline_start_us.erase(it);
  return true;
}

}  // namespace webrtc_demo
