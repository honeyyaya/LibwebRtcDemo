#include "android_mediacodec_video_decoder.h"
#include "android_native_video_frame_buffer.h"
#include "encoded_tracking_bridge.h"
#include "video_decode_sink_timing_bridge.h"

#include "api/video/encoded_image.h"
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "api/video/video_frame.h"
#include "api/video/video_frame_type.h"
#include "modules/video_coding/include/video_error_codes.h"

#define LOG_TAG "McVideoDec"
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// NDK r26+ 鐨?NdkMediaCodec.h 鍙兘涓嶅啀瀹氫箟 KEY_FRAME锛涗笌 Java MediaCodec.BUFFER_FLAG_KEY_FRAME 涓€鑷淬€?
#ifndef AMEDIACODEC_BUFFER_FLAG_KEY_FRAME
#define AMEDIACODEC_BUFFER_FLAG_KEY_FRAME 1u
#endif

namespace webrtc_demo {

// 渚?Decode锛堢被澶栵級涓庡尶鍚嶅懡鍚嶇┖闂村唴鍏辩敤銆?
int64_t McMonotonicUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

namespace {

// COLOR_FormatYUV420SemiPlanar
// COLOR_FormatYUV420Flexible
// 閮ㄥ垎鍘傚晢 Codec2 鍦?AMessage 閲屼娇鐢ㄤ笌 color-format 涓嶅悓鐨?android._color-format

// 涓?Java MediaFormat.KEY_LOW_LATENCY 涓€鑷达紱閮ㄥ垎璁惧鍦?API 30+ 涓婂彲闄嶄綆瑙ｇ爜鍣ㄥ唴閮ㄦ帓闃熴€?
constexpr char kMediaFormatLowLatency[] = "low-latency";
#if __ANDROID_API__ >= 30
constexpr int kCodecLowLatencyEnabled = 1;
#else
constexpr int kCodecLowLatencyEnabled = 0;
#endif

// queue 鍚?drain锛氬厛闈為樆濉炴竻绌哄凡灏辩华甯э紝鍐嶅崟娆＄煭闃诲鍚告敹銆屽垰瀹屾垚銆嶇殑 output銆?
// 鍘熷厛鍗曢樁娈?first_timeout=35ms 浼氬湪 worker 涓婁覆琛屽爢鍙狅紝涓昏寤惰繜杩滃ぇ浜庤蒋瑙ｃ€?
constexpr int64_t kDequeueInputTimeoutUs = 3000;
constexpr int64_t kOutputDequeueTimeoutUs = 3000;
constexpr int64_t kDrainAfterQueueShortWaitUs = 1000;
constexpr int64_t kBackpressureLogIntervalUs = 500000;
// acquireLatestImageAsync() needs headroom beyond the native render slots.
// With 6 native texture slots, renderer-side pending/held buffers can keep 2
// more AImages alive at the same time, so maxImages must stay above 8 to let
// ImageReader discard stale buffers and still hand us the newest image.
constexpr int32_t kImageReaderMaxImages = 10;
constexpr size_t kMaxPendingOutputMetadata = 128;

// WebRTC H264 鎺ユ敹璺緞澶氫负 Annex B锛?0 00 01 / 00 00 00 01锛夈€侰odec2 瑙ｇ爜鍣ㄩ€氬父瑕佽繖绉嶈緭鍏ワ紱
// 鑻ヨ杞垚 AVCC锛? 瀛楄妭闀垮害鍓嶇紑锛夛紝閮ㄥ垎鏈哄瀷浼氬悆婊?input 浣嗘案杩滀笉鍑?output锛坒ps=0锛夈€?
bool LooksLikeAnnexB(const uint8_t* d, size_t sz) {
  if (sz < 4 || !d) {
    return false;
  }
  if (d[0] == 0 && d[1] == 0 && d[2] == 1) {
    return true;
  }
  if (d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 1) {
    return true;
  }
  return false;
}

std::vector<std::pair<const uint8_t*, size_t>> SplitAnnexB(const uint8_t* data, size_t size) {
  std::vector<std::pair<const uint8_t*, size_t>> nals;
  if (!data || size < 3) {
    return nals;
  }
  size_t i = 0;
  while (i < size) {
    size_t nal_start = 0;
    if (i + 3 <= size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      nal_start = i + 3;
      i += 3;
    } else if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
               data[i + 3] == 1) {
      nal_start = i + 4;
      i += 4;
    } else {
      ++i;
      continue;
    }
    size_t j = nal_start;
    while (j < size) {
      if (j + 3 <= size && data[j] == 0 && data[j + 1] == 0 &&
          (data[j + 2] == 1 || (j + 4 <= size && data[j + 2] == 0 && data[j + 3] == 1))) {
        break;
      }
      ++j;
    }
    if (j > nal_start) {
      nals.push_back({data + nal_start, j - nal_start});
    }
    i = j;
  }
  return nals;
}

void AnnexBToAvcc(const uint8_t* data, size_t size, std::vector<uint8_t>* out) {
  out->clear();
  if (!data || size == 0) {
    return;
  }
  std::vector<std::pair<const uint8_t*, size_t>> nals = SplitAnnexB(data, size);
  if (nals.empty()) {
    out->push_back(static_cast<uint8_t>((size >> 24) & 0xff));
    out->push_back(static_cast<uint8_t>((size >> 16) & 0xff));
    out->push_back(static_cast<uint8_t>((size >> 8) & 0xff));
    out->push_back(static_cast<uint8_t>(size & 0xff));
    out->insert(out->end(), data, data + size);
    return;
  }
  for (const auto& nal : nals) {
    const size_t len = nal.second;
    if (len == 0) {
      continue;
    }
    out->push_back(static_cast<uint8_t>((len >> 24) & 0xff));
    out->push_back(static_cast<uint8_t>((len >> 16) & 0xff));
    out->push_back(static_cast<uint8_t>((len >> 8) & 0xff));
    out->push_back(static_cast<uint8_t>(len & 0xff));
    out->insert(out->end(), nal.first, nal.first + len);
  }
}

// Decoder output stays on the native AImageReader/AHardwareBuffer path.
// The old NV12->I420 CPU fallback has been removed to keep the pipeline single-path.
std::atomic<uint64_t> g_mc_decode_ingress_seq{0};
std::atomic<uint64_t> g_mc_process_frame_seq{0};

// RTP 鎵╁睍 / Field Trial 甯︽潵鐨?EncodedImage::VideoFrameTrackingId锛涗笌瑙ｇ爜鍚?VideoFrame::id 瀵归綈渚涙祴璇曢摼璺娇鐢ㄣ€?
void LogEncodedFrameTrackingIngress(const std::optional<uint16_t>& tracking_id,
                                    uint32_t rtp_ts,
                                    bool key) {
  if (!tracking_id.has_value()) {
    return;
  }
  webrtc_demo::RecordEncodedFrameTrackingId(tracking_id);
  if (webrtc_demo::ShouldLogTrackingTimedSampleById(static_cast<uint32_t>(tracking_id.value()))) {
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
    const int64_t steady_us = McMonotonicUs();

    ALOGI(
        "銆愯€楁椂鍒嗘瀽銆慐ncodedFrame VideoFrameTrackingId=%u rtp_ts=%u key=%d | "
        "local_time=%s unix_ms=%lld steady_us=%lld",
        static_cast<unsigned>(*tracking_id), rtp_ts, key ? 1 : 0, local_time_str,
        static_cast<long long>(unix_ms), static_cast<long long>(steady_us));
  }
}

// 鍗曟 DrainOutputs 鍐呴儴缁嗗垎锛堝井绉掞級锛沺erf_detail==nullptr 鏃朵笉閲囨牱锛岄伩鍏嶇儹璺緞寮€閿€銆?
struct McDrainDetail {
  int64_t total_us = 0;
  int64_t dequeue_us = 0;
  int64_t get_out_buf_us = 0;
  int64_t nv12_i420_us = 0;
  int64_t decoded_cb_us = 0;
  int64_t release_us = 0;
  int out_buffers = 0;
};

// 纭 WebRTC 鏄惁鎶?EncodedImage 閫佽繘鏈В鐮佸櫒锛氬墠 10 甯?+ 涔嬪悗姣?30 甯ф墦涓€鏉★紝閬垮厤鍒峰睆銆?
void LogMcDecodeIngress(const uint8_t* data,
                        size_t sz,
                        uint32_t rtp_ts,
                        bool key,
                        int64_t render_time_ms) {
  // const uint64_t n = ++g_mc_decode_ingress_seq;
  // char head_hex[3 * 16 + 1] = {0};
  // const int nshow = (sz >= 16) ? 16 : static_cast<int>(sz);
  // for (int i = 0; i < nshow; ++i) {
  //   snprintf(head_hex + i * 3, 4, "%02x ", static_cast<unsigned int>(data[i]));
  // }
  // if (n <= 10u || (n % 30u) == 0u) {
  //   ALOGI(
  //       "Decode ingress #%llu sz=%zu rtp_ts=%u key=%d render_ms=%lld head16=[%s] "
  //       "(00 00 01=AnnexB; 4byte_len=AVCC)",
  //       static_cast<unsigned long long>(n), sz, rtp_ts, key ? 1 : 0,
  //       static_cast<long long>(render_time_ms), head_hex);
  // }
}

}  // namespace

struct AndroidMediaCodecVideoDecoder::Impl {
  struct OutputFrameMetadata {
    int64_t render_time_ms = 0;
    uint32_t rtp_timestamp = 0;
    int64_t decode_wall_t0_us = 0;
    std::optional<uint16_t> video_frame_tracking_id;
  };

  struct PendingOutputFrameMetadata {
    int64_t pts_us = 0;
    OutputFrameMetadata metadata;
  };

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  bool running_ = false;
  std::thread thread_;
  bool image_drain_task_posted_ = false;

  AMediaCodec* codec_ = nullptr;
  AImageReader* image_reader_ = nullptr;
  ANativeWindow* output_window_ = nullptr;

  webrtc::DecodedImageCallback* callback_ = nullptr;

  int out_width_ = 0;
  int out_height_ = 0;

  std::vector<uint8_t> avcc_scratch_;
  std::deque<PendingOutputFrameMetadata> pending_output_metadata_;
  std::atomic<int32_t> pending_image_notifications_{0};

  int64_t next_input_pts_us_ = 0;
  size_t pending_decode_tasks_ = 0;
  uint32_t dequeue_input_fail_burst_ = 0;
  uint64_t dequeue_input_fail_total_ = 0;
  int64_t last_backpressure_log_us_ = 0;

  void WorkerLoop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (running_) {
      cv_.wait(lk, [this] { return !tasks_.empty() || !running_; });
      if (!running_) {
        break;
      }
      while (!tasks_.empty()) {
        std::function<void()> job = std::move(tasks_.front());
        tasks_.pop_front();
        lk.unlock();
        if (job) {
          job();
        }
        lk.lock();
      }
    }
  }

  void StopWorker() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      running_ = false;
      tasks_.clear();
      pending_decode_tasks_ = 0;
      image_drain_task_posted_ = false;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  void ResetInputBackpressureBurst() {
    std::lock_guard<std::mutex> lk(mu_);
    dequeue_input_fail_burst_ = 0;
  }

  void MaybeLogInputBackpressure(ssize_t in_idx) {
    const int64_t now_us = McMonotonicUs();
    size_t pending_decode_tasks = 0;
    size_t pending_output_metadata = 0;
    uint32_t burst = 0;
    uint64_t total = 0;
    bool should_log = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      ++dequeue_input_fail_burst_;
      ++dequeue_input_fail_total_;
      burst = dequeue_input_fail_burst_;
      total = dequeue_input_fail_total_;
      pending_decode_tasks = pending_decode_tasks_;
      pending_output_metadata = pending_output_metadata_.size();
      should_log =
          burst <= 3 || (now_us - last_backpressure_log_us_) >= kBackpressureLogIntervalUs;
      if (should_log) {
        last_backpressure_log_us_ = now_us;
      }
    }

    if (!should_log) {
      return;
    }

    ALOGW(
        "[McBackpressure] dequeueInputBuffer=%zd burst=%u total=%llu pending_tasks=%zu "
        "pending_meta=%zu pending_images=%d out=%dx%d",
        in_idx, burst, static_cast<unsigned long long>(total), pending_decode_tasks,
        pending_output_metadata, pending_image_notifications_.load(std::memory_order_relaxed),
        out_width_, out_height_);
  }

  void DestroyCodec() {
    if (codec_) {
      AMediaCodec_stop(codec_);
      AMediaCodec_delete(codec_);
      codec_ = nullptr;
    }
    if (image_reader_) {
#if __ANDROID_API__ >= 26
      DetachImageReaderListener();
#endif
      AImageReader_delete(image_reader_);
      image_reader_ = nullptr;
      output_window_ = nullptr;
    } else {
#if __ANDROID_API__ >= 26
      ResetImageReaderState();
#endif
    }
    out_width_ = out_height_ = 0;
  }

  void UpdateOutputFormat(AMediaFormat* fmt) {
    if (!fmt) {
      return;
    }
    AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, &out_width_);
    AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, &out_height_);
  }

  void RefreshOutputFormat() {
    if (!codec_) {
      return;
    }
    AMediaFormat* fmt = AMediaCodec_getOutputFormat(codec_);
    if (fmt) {
      UpdateOutputFormat(fmt);
      AMediaFormat_delete(fmt);
    }
  }

  void ClearOutputMetadata() {
    std::lock_guard<std::mutex> lk(mu_);
    pending_output_metadata_.clear();
  }

#if __ANDROID_API__ >= 26
  void RunQueuedImageDrainTask() {
    DrainReadyImages(nullptr, nullptr);

    {
      std::lock_guard<std::mutex> lk(mu_);
      image_drain_task_posted_ = false;
    }

    if (pending_image_notifications_.load(std::memory_order_relaxed) > 0) {
      QueueImageDrainTask();
    }
  }

  void QueueImageDrainTask() {
    bool should_notify = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!running_ || image_drain_task_posted_) {
        return;
      }
      image_drain_task_posted_ = true;
      tasks_.push_back([this] { RunQueuedImageDrainTask(); });
      should_notify = true;
    }
    if (should_notify) {
      cv_.notify_one();
    }
  }
#endif

  int64_t AllocateInputPtsUs() {
    const int64_t now_us = McMonotonicUs();
    if (now_us <= next_input_pts_us_) {
      ++next_input_pts_us_;
    } else {
      next_input_pts_us_ = now_us;
    }
    return next_input_pts_us_;
  }

  void RecordOutputMetadata(int64_t pts_us,
                            int64_t render_time_ms,
                            uint32_t rtp_timestamp,
                            int64_t decode_wall_t0_us,
                            const std::optional<uint16_t>& video_frame_tracking_id) {
    std::lock_guard<std::mutex> lk(mu_);
    OutputFrameMetadata meta;
    meta.render_time_ms = render_time_ms;
    meta.rtp_timestamp = rtp_timestamp;
    meta.decode_wall_t0_us = decode_wall_t0_us;
    meta.video_frame_tracking_id = video_frame_tracking_id;
    pending_output_metadata_.push_back({pts_us, meta});
    while (pending_output_metadata_.size() > kMaxPendingOutputMetadata) {
      pending_output_metadata_.pop_front();
    }
  }

  void RemoveOutputMetadata(int64_t pts_us) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!pending_output_metadata_.empty() && pending_output_metadata_.back().pts_us == pts_us) {
      pending_output_metadata_.pop_back();
      return;
    }
    for (auto it = pending_output_metadata_.begin(); it != pending_output_metadata_.end(); ++it) {
      if (it->pts_us == pts_us) {
        pending_output_metadata_.erase(it);
        break;
      }
    }
  }

  void DiscardOutputMetadataUpToPtsUsLocked(int64_t pts_us) {
    while (!pending_output_metadata_.empty() &&
           pending_output_metadata_.front().pts_us <= pts_us) {
      pending_output_metadata_.pop_front();
    }
  }

  std::optional<OutputFrameMetadata> TakeOutputMetadataForPtsUs(int64_t pts_us) {
    std::lock_guard<std::mutex> lk(mu_);
    static std::atomic<int> stale_warn_count{0};
    while (!pending_output_metadata_.empty() &&
           pending_output_metadata_.front().pts_us < pts_us) {
      if (stale_warn_count.fetch_add(1, std::memory_order_relaxed) < 5) {
        ALOGW("drop stale output metadata: expected pts=%lld got newer pts=%lld",
              static_cast<long long>(pending_output_metadata_.front().pts_us),
              static_cast<long long>(pts_us));
      }
      pending_output_metadata_.pop_front();
    }

    if (!pending_output_metadata_.empty() &&
        pending_output_metadata_.front().pts_us == pts_us) {
      OutputFrameMetadata meta = pending_output_metadata_.front().metadata;
      pending_output_metadata_.pop_front();
      return meta;
    }

    static std::atomic<int> missing_warn_count{0};
    if (missing_warn_count.fetch_add(1, std::memory_order_relaxed) < 5) {
      ALOGW("output pts metadata missing: pts=%lld", static_cast<long long>(pts_us));
    }
    return std::nullopt;
  }

  void DiscardOutputMetadataUpToPtsUs(int64_t pts_us) {
    std::lock_guard<std::mutex> lk(mu_);
    DiscardOutputMetadataUpToPtsUsLocked(pts_us);
  }

#if __ANDROID_API__ >= 26
  static void OnImageAvailable(void* context, AImageReader* /*reader*/) {
    auto* self = static_cast<Impl*>(context);
    if (!self) {
      return;
    }
    self->pending_image_notifications_.fetch_add(1, std::memory_order_relaxed);
    self->QueueImageDrainTask();
  }

  void ResetImageReaderState() {
    pending_image_notifications_.store(0, std::memory_order_relaxed);
  }

  void DetachImageReaderListener() {
    if (!image_reader_) {
      ResetImageReaderState();
      return;
    }

    AImageReader_ImageListener listener{};
    listener.context = nullptr;
    listener.onImageAvailable = nullptr;
    AImageReader_setImageListener(image_reader_, &listener);
    ResetImageReaderState();
  }

  bool InstallImageReaderListener() {
    if (!image_reader_) {
      return false;
    }

    AImageReader_ImageListener listener{};
    listener.context = this;
    listener.onImageAvailable = &Impl::OnImageAvailable;
    const media_status_t st = AImageReader_setImageListener(image_reader_, &listener);
    if (st != AMEDIA_OK) {
      ALOGW("AImageReader_setImageListener failed: %d", static_cast<int>(st));
      ResetImageReaderState();
      return false;
    }

    ResetImageReaderState();
    return true;
  }

  bool AcquireLatestOutputImage(AImage** out_image, int* out_sync_fence_fd) {
    if (!image_reader_ || !out_image || !out_sync_fence_fd) {
      return false;
    }

    *out_image = nullptr;
    *out_sync_fence_fd = -1;
    const media_status_t st =
        AImageReader_acquireLatestImageAsync(image_reader_, out_image, out_sync_fence_fd);
    if (st == AMEDIA_OK && *out_image) {
      ResetImageReaderState();
      return true;
    }

    if (*out_sync_fence_fd >= 0) {
      close(*out_sync_fence_fd);
      *out_sync_fence_fd = -1;
    }
    if (st == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) {
      ResetImageReaderState();
      return false;
    }

    static std::atomic<int> acquire_warn_count{0};
    if (acquire_warn_count.fetch_add(1, std::memory_order_relaxed) < 5) {
      ALOGW("AImageReader_acquireLatestImageAsync failed: %d", static_cast<int>(st));
    }
    return false;
  }

  void DrainReadyImages(McDrainDetail* perf_detail, int* delivered_frames) {
    if (!image_reader_) {
      return;
    }

    for (;;) {
      webrtc::DecodedImageCallback* cb = nullptr;
      {
        std::lock_guard<std::mutex> lk(mu_);
        cb = callback_;
      }
      if (!cb) {
        return;
      }

      AImage* image = nullptr;
      int sync_fence_fd = -1;
      if (!AcquireLatestOutputImage(&image, &sync_fence_fd) || !image) {
        return;
      }

      AHardwareBuffer* hardware_buffer = nullptr;
      const media_status_t image_status = AImage_getHardwareBuffer(image, &hardware_buffer);
      if (image_status != AMEDIA_OK || !hardware_buffer) {
        if (sync_fence_fd >= 0) {
          close(sync_fence_fd);
        }
        AImage_delete(image);
        ALOGW("AImage_getHardwareBuffer failed: %d", static_cast<int>(image_status));
        continue;
      }

      int32_t image_width = out_width_;
      int32_t image_height = out_height_;
      AImage_getWidth(image, &image_width);
      AImage_getHeight(image, &image_height);
      if (image_width <= 0 || image_height <= 0) {
        if (sync_fence_fd >= 0) {
          close(sync_fence_fd);
        }
        AImage_delete(image);
        ALOGW("AImage dimensions invalid: %d x %d", image_width, image_height);
        continue;
      }

      int64_t image_timestamp_ns = 0;
      const media_status_t timestamp_status = AImage_getTimestamp(image, &image_timestamp_ns);
      const int64_t image_pts_us =
          timestamp_status == AMEDIA_OK ? image_timestamp_ns / 1000 : 0;
      if (timestamp_status != AMEDIA_OK) {
        if (sync_fence_fd >= 0) {
          close(sync_fence_fd);
        }
        AImage_delete(image);
        static std::atomic<int> timestamp_warn_count{0};
        if (timestamp_warn_count.fetch_add(1, std::memory_order_relaxed) < 5) {
          ALOGW("AImage_getTimestamp failed: %d", static_cast<int>(timestamp_status));
        }
        continue;
      }

      const std::optional<OutputFrameMetadata> frame_meta =
          TakeOutputMetadataForPtsUs(image_pts_us);
      if (!frame_meta.has_value()) {
        if (sync_fence_fd >= 0) {
          close(sync_fence_fd);
        }
        AImage_delete(image);
        continue;
      }

      webrtc::scoped_refptr<webrtc::VideoFrameBuffer> native_buffer(
          new AndroidHardwareBufferVideoFrameBuffer(image, hardware_buffer, sync_fence_fd,
                                                   image_width, image_height));
      webrtc::VideoFrame::Builder frame_builder;
      frame_builder.set_video_frame_buffer(native_buffer)
          .set_rtp_timestamp(frame_meta->rtp_timestamp)
          .set_timestamp_us(frame_meta->render_time_ms * 1000);
      if (frame_meta->video_frame_tracking_id.has_value()) {
        frame_builder.set_id(*frame_meta->video_frame_tracking_id);
      }

      int64_t t_cb0 = 0;
      if (perf_detail) {
        t_cb0 = McMonotonicUs();
      }
      webrtc::VideoFrame decoded_frame = frame_builder.build();
      cb->Decoded(decoded_frame);
      const int64_t t_after_decoded_cb = McMonotonicUs();
      if (frame_meta->decode_wall_t0_us > 0 &&
          frame_meta->video_frame_tracking_id.has_value() &&
          webrtc_demo::ShouldLogTrackingTimedSampleById(
              static_cast<uint32_t>(*frame_meta->video_frame_tracking_id))) {
        const int64_t e2e_us = t_after_decoded_cb - frame_meta->decode_wall_t0_us;
        ALOGI(
            "McE2E native tracking_id=%u rtp_ts=%u e2e=%lldus "
            "(Decode entry -> Decoded return; native AHardwareBuffer; excludes downstream sink)",
            static_cast<unsigned>(*frame_meta->video_frame_tracking_id), frame_meta->rtp_timestamp,
            static_cast<long long>(e2e_us));
      }
      DecodeSinkRecordAfterDecoded(frame_meta->rtp_timestamp, t_after_decoded_cb);
      if (perf_detail) {
        perf_detail->decoded_cb_us += t_after_decoded_cb - t_cb0;
        ++perf_detail->out_buffers;
      }
      if (delivered_frames) {
        ++(*delivered_frames);
      }
    }
  }
#endif

  bool ConfigureOnWorker(const webrtc::VideoDecoder::Settings& settings) {
    DestroyCodec();
    ClearOutputMetadata();
    int w = 1920;
    int h = 1080;
    if (settings.max_render_resolution().Valid()) {
      w = settings.max_render_resolution().Width();
      h = settings.max_render_resolution().Height();
    }
    codec_ = AMediaCodec_createDecoderByType("video/avc");
    if (!codec_) {
      ALOGW("AMediaCodec_createDecoderByType failed");
      return false;
    }
    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, w);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, h);
#if __ANDROID_API__ >= 30
    AMediaFormat_setInt32(format, kMediaFormatLowLatency, 1);
#endif

#if __ANDROID_API__ < 26
    AMediaFormat_delete(format);
    ALOGW("AHardwareBuffer decoder output requires Android API 26+");
    AMediaCodec_delete(codec_);
    codec_ = nullptr;
    return false;
#else
    media_status_t st = AImageReader_newWithUsage(
        w, h, AIMAGE_FORMAT_PRIVATE,
        AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
        kImageReaderMaxImages, &image_reader_);
    if (st != AMEDIA_OK || !image_reader_) {
      AMediaFormat_delete(format);
      ALOGW("AImageReader_newWithUsage failed: %d", static_cast<int>(st));
      AMediaCodec_delete(codec_);
      codec_ = nullptr;
      return false;
    }
    InstallImageReaderListener();
    st = AImageReader_getWindow(image_reader_, &output_window_);
    if (st != AMEDIA_OK || !output_window_) {
      AMediaFormat_delete(format);
      ALOGW("AImageReader_getWindow failed: %d", static_cast<int>(st));
      DestroyCodec();
      return false;
    }

    st = AMediaCodec_configure(codec_, format, output_window_, nullptr, 0);
    AMediaFormat_delete(format);
    if (st != AMEDIA_OK) {
      ALOGW("AMediaCodec_configure failed: %d", static_cast<int>(st));
      DestroyCodec();
      return false;
    }
#endif
    st = AMediaCodec_start(codec_);
    if (st != AMEDIA_OK) {
      ALOGW("AMediaCodec_start failed: %d", static_cast<int>(st));
      DestroyCodec();
      return false;
    }
    next_input_pts_us_ = 0;
    {
      std::lock_guard<std::mutex> lk(mu_);
      dequeue_input_fail_burst_ = 0;
      dequeue_input_fail_total_ = 0;
      last_backpressure_log_us_ = 0;
    }
    ALOGI("[McConfig] codec=video/avc size=%dx%d max_images=%d low_latency=%d", w, h,
          static_cast<int>(kImageReaderMaxImages), kCodecLowLatencyEnabled);
    // 灏芥棭鎷夊彇杈撳嚭鏍煎紡锛堥儴鍒?Codec2 鍦ㄩ甯?output 鍓嶄笉浼氬崟鐙Е鍙?INFO锛屽鑷?out_width_ 涓€鐩翠负 0锛?
    RefreshOutputFormat();
    return true;
  }

  void DrainOutputs(int64_t first_dequeue_timeout_us,
                    McDrainDetail* perf_detail,
                    int* delivered_frames) {
    if (!codec_) {
      return;
    }
    const int64_t t_wall_start = perf_detail ? McMonotonicUs() : int64_t{0};
    // 鏃犺鏄惁宸叉敞鍐?callback锛岄兘蹇呴』 dequeue 骞?release output锛屽惁鍒欎細濉炴弧绠￠亾 in=0 out=0銆佽В鐮佸抚鎭掍负 0

    bool used_timeout = false;
    bool released_to_native_surface = false;
    for (;;) {
      AMediaCodecBufferInfo info;
      const int64_t t_us = (!used_timeout) ? first_dequeue_timeout_us : 0;
      used_timeout = true;
      int64_t t_dq0 = 0;
      if (perf_detail) {
        t_dq0 = McMonotonicUs();
      }
      ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, t_us);
      if (perf_detail) {
        perf_detail->dequeue_us += McMonotonicUs() - t_dq0;
      }
      if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        RefreshOutputFormat();
        continue;
      }
      if (out_idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
        RefreshOutputFormat();
        continue;
      }
      if (out_idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
        break;
      }
      if (out_idx < 0) {
        break;
      }

      if (info.size > 0 && (out_width_ <= 0 || out_height_ <= 0)) {
        RefreshOutputFormat();
      }

      webrtc::DecodedImageCallback* cb = nullptr;
      {
        std::lock_guard<std::mutex> lk(mu_);
        cb = callback_;
      }

      const bool use_native_surface = cb && image_reader_;
      released_to_native_surface = released_to_native_surface || use_native_surface;

      int64_t t_rel0 = 0;
      if (perf_detail) {
        t_rel0 = McMonotonicUs();
      }
      AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(out_idx), use_native_surface);
      if (perf_detail) {
        perf_detail->release_us += McMonotonicUs() - t_rel0;
      }

#if __ANDROID_API__ >= 26
      if (!use_native_surface) {
        DiscardOutputMetadataUpToPtsUs(info.presentationTimeUs);
      }
#endif
    }
#if __ANDROID_API__ >= 26
    if (released_to_native_surface) {
      DrainReadyImages(perf_detail, delivered_frames);
    }
#endif
    if (perf_detail) {
      perf_detail->total_us = McMonotonicUs() - t_wall_start;
    }
  }

  int DrainOutputsBounded(int64_t first_dequeue_timeout_us, int max_passes) {
    int total_delivered = 0;
    bool first_pass = true;
    for (int i = 0; i < max_passes; ++i) {
      int delivered_this_pass = 0;
      DrainOutputs(first_pass ? first_dequeue_timeout_us : 0, nullptr, &delivered_this_pass);
      total_delivered += delivered_this_pass;
      if (delivered_this_pass == 0) {
        break;
      }
      first_pass = false;
    }
    return total_delivered;
  }

  void ProcessOneFrame(
      const webrtc::scoped_refptr<webrtc::EncodedImageBufferInterface>& data,
      size_t data_size,
                       int64_t render_time_ms,
                       uint32_t rtp_timestamp,
                       bool is_keyframe,
                       int64_t decode_wall_t0_us,
                       const std::optional<uint16_t>& video_frame_tracking_id) {
    if (!codec_ || !data || data_size == 0 || !data->data()) {
      return;
    }

    const uint8_t* feed_ptr = data->data();
    size_t feed_size = data_size;
    if (LooksLikeAnnexB(data->data(), data_size)) {
      // 鐩存帴閫?Annex B锛堜笌 McVideoDec 鏃ュ織閲?head16 涓€鑷达級
    } else {
      AnnexBToAvcc(data->data(), data_size, &avcc_scratch_);
      if (avcc_scratch_.empty()) {
        return;
      }
      feed_ptr = avcc_scratch_.data();
      feed_size = avcc_scratch_.size();
    }
    ssize_t in_idx = AMediaCodec_dequeueInputBuffer(codec_, kDequeueInputTimeoutUs);
    if (in_idx < 0) {
      MaybeLogInputBackpressure(in_idx);
      DrainOutputsBounded(kOutputDequeueTimeoutUs, 3);
      return;
    }
    ResetInputBackpressureBurst();

    size_t in_cap = 0;
    uint8_t* in_buf = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(in_idx), &in_cap);
    if (!in_buf || feed_size > in_cap) {
      ALOGW("input buffer too small: need %zu have %zu", feed_size, in_cap);
      AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(in_idx), 0, 0, 0, 0);
      return;
    }
    memcpy(in_buf, feed_ptr, feed_size);

    uint32_t flags = 0;
    if (is_keyframe) {
      flags |= AMEDIACODEC_BUFFER_FLAG_KEY_FRAME;
    }
    const int64_t pts_us = AllocateInputPtsUs();
    RecordOutputMetadata(pts_us, render_time_ms, rtp_timestamp, decode_wall_t0_us,
                         video_frame_tracking_id);

    media_status_t st = AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(in_idx), 0,
                                                      feed_size, pts_us, flags);
    if (st != AMEDIA_OK) {
      ALOGW("queueInputBuffer failed: %d", static_cast<int>(st));
      RemoveOutputMetadata(pts_us);
      // 宸?dequeue 鐨?input 蹇呴』褰掕繕锛屽惁鍒欒В鐮佸櫒鍐呴儴鐘舵€佷細閿欎贡骞舵斁澶х郴缁熷眰 PipelineWatcher 鍛婅銆?
      AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(in_idx), 0, 0, 0, 0);
      return;
    }
    const int delivered_frames = DrainOutputsBounded(0, 3);
    if (delivered_frames == 0) {
      DrainOutputsBounded(kDrainAfterQueueShortWaitUs, 3);
    }
    // if (log_perf) {
    //   const int64_t worker_total_us = McMonotonicUs() - t_pf0;
    //   ALOGI(
    //       "McPerf #%llu worker_total=%lldus (prep=%lld deq_in=%lld memcpy_in=%lld q_in=%lld) | "
    //       "drain0: tot=%lld deq=%lld getbuf=%lld nv12_i420=%lld decoded_cb=%lld rel=%lld out=%d | "
    //       "drain1: tot=%lld deq=%lld getbuf=%lld nv12_i420=%lld decoded_cb=%lld rel=%lld out=%d | "
    //       "feed_sz=%zu key=%d "
    //       "(UI骞冲潎瑙ｇ爜鍚玏ebRTC閫傞厤鍣?鍥炶皟閾撅紝闈炴湰琛屾€诲拰)",
    //       static_cast<unsigned long long>(pfn), static_cast<long long>(worker_total_us),
    //       static_cast<long long>(prep_us), static_cast<long long>(deq_in_us),
    //       static_cast<long long>(memcpy_in_us), static_cast<long long>(q_in_us),
    //       static_cast<long long>(d0.total_us), static_cast<long long>(d0.dequeue_us),
    //       static_cast<long long>(d0.get_out_buf_us), static_cast<long long>(d0.nv12_i420_us),
    //       static_cast<long long>(d0.decoded_cb_us), static_cast<long long>(d0.release_us),
    //       d0.out_buffers, static_cast<long long>(d1.total_us),
    //       static_cast<long long>(d1.dequeue_us), static_cast<long long>(d1.get_out_buf_us),
    //       static_cast<long long>(d1.nv12_i420_us), static_cast<long long>(d1.decoded_cb_us),
    //       static_cast<long long>(d1.release_us), d1.out_buffers, feed_size, is_keyframe ? 1 : 0);
    // }
  }
};

AndroidMediaCodecVideoDecoder::AndroidMediaCodecVideoDecoder()
    : impl_(std::make_unique<AndroidMediaCodecVideoDecoder::Impl>()) {}

AndroidMediaCodecVideoDecoder::~AndroidMediaCodecVideoDecoder() {
  Release();
}

bool AndroidMediaCodecVideoDecoder::Configure(const webrtc::VideoDecoder::Settings& settings) {
  if (!impl_) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    if (!impl_->running_) {
      impl_->running_ = true;
      impl_->thread_ = std::thread([this] { impl_->WorkerLoop(); });
    }
  }

  // std::function 瑕佹眰鍙嫹璐濓紱packaged_task 浠呭彲绉诲姩锛屾晠鐢?shared_ptr 鍖呬竴灞傘€?
  auto pt = std::make_shared<std::packaged_task<bool()>>(
      [this, settings] { return impl_->ConfigureOnWorker(settings); });
  std::future<bool> fut = pt->get_future();
  {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    impl_->tasks_.clear();
    impl_->pending_decode_tasks_ = 0;
    impl_->image_drain_task_posted_ = false;
    impl_->tasks_.push_front([pt]() { (*pt)(); });
  }
  impl_->cv_.notify_one();
  return fut.get();
}

int32_t AndroidMediaCodecVideoDecoder::Decode(const webrtc::EncodedImage& input_image,
                                                bool /*missing_frames*/,
                                                int64_t render_time_ms) {
  if (!impl_) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  const size_t sz = input_image.size();
  if (sz == 0 || !input_image.data()) {
    static std::atomic<int> empty_ingress_warn{0};
    if (empty_ingress_warn.fetch_add(1) < 5) {
      ALOGW("Decode called empty: sz=%zu data=%p (鏈繘鍏?MediaCodec)", sz,
            static_cast<const void*>(input_image.data()));
    }
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  const uint32_t rtp_ts = input_image.RtpTimestamp();
  const bool key = (input_image.FrameType() == webrtc::VideoFrameType::kVideoFrameKey);
  const std::optional<uint16_t> video_frame_tracking_id = input_image.VideoFrameTrackingId();
  LogEncodedFrameTrackingIngress(video_frame_tracking_id, rtp_ts, key);
  if (video_frame_tracking_id.has_value()) {
    webrtc_demo::RecordDecodePipelineStartForE2eIfSampled(
        static_cast<uint32_t>(*video_frame_tracking_id));
  }
  // 绔埌绔鏃惰捣鐐癸細涓庢湰甯?EncodedImage 瀵瑰簲锛堝惈鍚庣画鎺掗槦銆亀orker銆丏ecoded 鍥炶皟锛夈€?
  const int64_t decode_wall_t0_us = McMonotonicUs();
  LogMcDecodeIngress(input_image.data(), sz, rtp_ts, key, render_time_ms);

  webrtc::scoped_refptr<webrtc::EncodedImageBufferInterface> encoded_buffer =
      input_image.GetEncodedData();
  if (!encoded_buffer) {
    encoded_buffer = webrtc::EncodedImageBuffer::Create(input_image.data(), sz);
  }
  if (!encoded_buffer || encoded_buffer->size() < sz || !encoded_buffer->data()) {
    ALOGW("Decode buffer unavailable: sz=%zu", sz);
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    if (!impl_->running_) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    ++impl_->pending_decode_tasks_;
    impl_->tasks_.push_back([this, encoded_buffer = std::move(encoded_buffer), render_time_ms,
                             encoded_size = sz, rtp_ts, key, decode_wall_t0_us,
                             video_frame_tracking_id]() mutable {
      impl_->ProcessOneFrame(encoded_buffer, encoded_size, render_time_ms, rtp_ts, key,
                             decode_wall_t0_us, video_frame_tracking_id);
      std::lock_guard<std::mutex> lk(impl_->mu_);
      if (impl_->pending_decode_tasks_ > 0) {
        --impl_->pending_decode_tasks_;
      }
    });
  }
  impl_->cv_.notify_one();
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t AndroidMediaCodecVideoDecoder::RegisterDecodeCompleteCallback(
    webrtc::DecodedImageCallback* callback) {
  if (!impl_) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  std::lock_guard<std::mutex> lk(impl_->mu_);
  impl_->callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t AndroidMediaCodecVideoDecoder::Release() {
  if (!impl_) {
    return WEBRTC_VIDEO_CODEC_OK;
  }
  {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    impl_->callback_ = nullptr;
  }
  std::shared_ptr<std::packaged_task<void()>> pt;
  std::future<void> fut;
  {
    pt = std::make_shared<std::packaged_task<void()>>([this] {
      impl_->DestroyCodec();
      impl_->ClearOutputMetadata();
    });
    fut = pt->get_future();
  }
  bool wait_for_destroy = false;
  {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    if (impl_->running_) {
      impl_->tasks_.clear();
      impl_->pending_decode_tasks_ = 0;
      impl_->image_drain_task_posted_ = false;
      impl_->tasks_.push_front([pt]() { (*pt)(); });
      wait_for_destroy = true;
    }
  }
  impl_->cv_.notify_one();
  if (wait_for_destroy) {
    fut.wait();
  } else {
    impl_->DestroyCodec();
    impl_->ClearOutputMetadata();
  }
  impl_->StopWorker();
  return WEBRTC_VIDEO_CODEC_OK;
}

webrtc::VideoDecoder::DecoderInfo AndroidMediaCodecVideoDecoder::GetDecoderInfo() const {
  DecoderInfo info;
  info.implementation_name = "mediacodec-h264";
  info.is_hardware_accelerated = true;
  return info;
}

}  // namespace webrtc_demo
