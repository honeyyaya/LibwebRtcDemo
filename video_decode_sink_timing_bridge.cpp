#include "video_decode_sink_timing_bridge.h"

#include "encoded_tracking_bridge.h"

#include <mutex>
#include <unordered_map>
#include <iostream>
#include <android/log.h>
#define LOG_TAG "Time"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

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
void printLocalTime(int id)
{
    const auto now_sys = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now_sys);
    const int64_t unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now_sys.time_since_epoch())
                                .count();
    const auto ms_part = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now_sys.time_since_epoch()) %
                         1000;
    std::tm tm_local{};
#if defined(WEBRTC_WIN)
    localtime_s(&tm_local, &t);
#else
    localtime_r(&t, &tm_local);
#endif
    char local_time_str[80];
    snprintf(local_time_str, sizeof(local_time_str), "%04d-%02d-%02d %02d:%02d:%02d.%03lld",
             tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday, tm_local.tm_hour,
             tm_local.tm_min, tm_local.tm_sec, static_cast<long long>(ms_part.count()));

    ALOGI(
        "【耗时分析】trackId =%u local_time=%s ",
        id, local_time_str
        );
    std::cout<<"【耗时分析】trackId "<<id<<"end at  :"<<local_time_str;
}

}  // namespace webrtc_demo
