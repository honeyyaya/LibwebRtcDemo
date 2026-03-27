#include "video_decode_sink_timing_bridge.h"

#include <mutex>
#include <unordered_map>

namespace webrtc_demo {
namespace {

std::mutex g_mu;
std::unordered_map<uint32_t, int64_t> g_decoded_return_us;
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

}  // namespace webrtc_demo
