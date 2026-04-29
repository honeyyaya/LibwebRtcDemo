#include "android_mediacodec_video_decoder.h"
#include "android_native_video_frame_buffer.h"
#include "encoded_tracking_bridge.h"
#include "video_decode_sink_timing_bridge.h"

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

// NDK r26+ �?NdkMediaCodec.h 可能不再定义 KEY_FRAME；与 Java MediaCodec.BUFFER_FLAG_KEY_FRAME 一致�?
#ifndef AMEDIACODEC_BUFFER_FLAG_KEY_FRAME
#define AMEDIACODEC_BUFFER_FLAG_KEY_FRAME 1u
#endif

namespace webrtc_demo {

// �?Decode（类外）与匿名命名空间内共用�?
int64_t McMonotonicUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

namespace {

// COLOR_FormatYUV420SemiPlanar
// COLOR_FormatYUV420Flexible
// 部分厂商 Codec2 �?AMessage 里使用与 color-format 不同�?android._color-format

// �?Java MediaFormat.KEY_LOW_LATENCY 一致；部分设备�?API 30+ 上可降低解码器内部排队�?
constexpr char kMediaFormatLowLatency[] = "low-latency";

// queue �?drain：先非阻塞清空已就绪帧，再单次短阻塞吸收「刚完成」的 output�?
// 原先单阶�?first_timeout=35ms 会在 worker 上串行堆叠，主观延迟远大于软解�?
constexpr int64_t kDrainAfterQueueShortWaitUs = 3000;
constexpr int64_t kDrainOnInputBackpressureUs = 3000;
constexpr int64_t kDequeueInputTimeoutUs = 3000;
constexpr int32_t kImageReaderMaxImages = 4;
constexpr int kAcquireLatestImageMaxAttempts = 4;
constexpr int64_t kAcquireLatestImageRetryDelayUs = 250;

// WebRTC H264 接收路径多为 Annex B�?0 00 01 / 00 00 00 01）。Codec2 解码器通常要这种输入；
// 若误转成 AVCC�? 字节长度前缀），部分机型会吃�?input 但永远不�?output（fps=0）�?
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

// RTP 扩展 / Field Trial 带来�?EncodedImage::VideoFrameTrackingId；与解码�?VideoFrame::id 对齐供测试链路使用�?
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
        "【耗时分析】EncodedFrame VideoFrameTrackingId=%u rtp_ts=%u key=%d | "
        "local_time=%s unix_ms=%lld steady_us=%lld",
        static_cast<unsigned>(*tracking_id), rtp_ts, key ? 1 : 0, local_time_str,
        static_cast<long long>(unix_ms), static_cast<long long>(steady_us));
  }
}

// 单次 DrainOutputs 内部细分（微秒）；perf_detail==nullptr 时不采样，避免热路径开销�?
struct McDrainDetail {
  int64_t total_us = 0;
  int64_t dequeue_us = 0;
  int64_t get_out_buf_us = 0;
  int64_t nv12_i420_us = 0;
  int64_t decoded_cb_us = 0;
  int64_t release_us = 0;
  int out_buffers = 0;
};

// 确认 WebRTC 是否�?EncodedImage 送进本解码器：前 10 �?+ 之后�?30 帧打一条，避免刷屏�?
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
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  bool running_ = false;
  std::thread thread_;

  AMediaCodec* codec_ = nullptr;
  AImageReader* image_reader_ = nullptr;
  ANativeWindow* output_window_ = nullptr;

  webrtc::DecodedImageCallback* callback_ = nullptr;

  int out_width_ = 0;
  int out_height_ = 0;

  std::vector<uint8_t> avcc_scratch_;
  std::atomic<int32_t> pending_image_notifications_{0};

  // 输入 PTS 使用单调递增时间戳；RTP 时间戳会回绕/乱序，易�?Codec2 PipelineWatcher 产生噪声告警�?  int64_t next_input_pts_us_ = 0;

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
    }
    cv_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
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

#if __ANDROID_API__ >= 26
  static void OnImageAvailable(void* context, AImageReader* /*reader*/) {
    auto* self = static_cast<Impl*>(context);
    if (!self) {
      return;
    }
    self->pending_image_notifications_.fetch_add(1, std::memory_order_relaxed);
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
    for (int attempt = 0; attempt < kAcquireLatestImageMaxAttempts; ++attempt) {
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

      if (st != AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) {
        static std::atomic<int> acquire_warn_count{0};
        if (acquire_warn_count.fetch_add(1, std::memory_order_relaxed) < 5) {
          ALOGW("AImageReader_acquireLatestImageAsync failed: %d", static_cast<int>(st));
        }
        return false;
      }

      if (attempt + 1 >= kAcquireLatestImageMaxAttempts) {
        break;
      }

      if (pending_image_notifications_.load(std::memory_order_relaxed) > 0) {
        std::this_thread::yield();
      } else {
        std::this_thread::sleep_for(
            std::chrono::microseconds(kAcquireLatestImageRetryDelayUs));
      }
    }

    return false;
  }
#endif

  bool ConfigureOnWorker(const webrtc::VideoDecoder::Settings& settings) {
    DestroyCodec();
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
    // 尽早拉取输出格式（部�?Codec2 在首�?output 前不会单独触�?INFO，导�?out_width_ 一直为 0�?
    RefreshOutputFormat();
    return true;
  }

  void DrainOutputs(int64_t render_time_ms,
                    uint32_t rtp_timestamp,
                    int64_t first_dequeue_timeout_us,
                    McDrainDetail* perf_detail,
                    int* delivered_frames,
                    const int64_t* decode_wall_t0_us,
                    const std::optional<uint16_t>& video_frame_tracking_id) {
    if (!codec_) {
      return;
    }
    const int64_t t_wall_start = perf_detail ? McMonotonicUs() : int64_t{0};
    webrtc::DecodedImageCallback* cb = nullptr;
    {
      std::lock_guard<std::mutex> lk(mu_);
      cb = callback_;
    }
    // 无论是否已注�?callback，都必须 dequeue �?release output，否则会塞满管道 in=0 out=0、解码帧恒为 0

    bool used_timeout = false;
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


      const bool use_native_surface = cb && image_reader_;

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
        continue;
      }

      AImage* image = nullptr;
      int sync_fence_fd = -1;
      if (!AcquireLatestOutputImage(&image, &sync_fence_fd) || !image) {
        continue;
      }

      AHardwareBuffer* hardware_buffer = nullptr;
      media_status_t image_status = AImage_getHardwareBuffer(image, &hardware_buffer);
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

      webrtc::scoped_refptr<webrtc::VideoFrameBuffer> native_buffer(
          new AndroidHardwareBufferVideoFrameBuffer(image, hardware_buffer, sync_fence_fd,
                                                   image_width, image_height));
      webrtc::VideoFrame::Builder frame_builder;
      frame_builder.set_video_frame_buffer(native_buffer)
          .set_rtp_timestamp(rtp_timestamp)
          .set_timestamp_us(render_time_ms * 1000);
      if (video_frame_tracking_id.has_value()) {
        frame_builder.set_id(*video_frame_tracking_id);
      }

      int64_t t_cb0 = 0;
      if (perf_detail) {
        t_cb0 = McMonotonicUs();
      }
      webrtc::VideoFrame decoded_frame = frame_builder.build();
      cb->Decoded(decoded_frame);
      const int64_t t_after_decoded_cb = McMonotonicUs();
      if (decode_wall_t0_us && video_frame_tracking_id.has_value() &&
          webrtc_demo::ShouldLogTrackingTimedSampleById(
              static_cast<uint32_t>(*video_frame_tracking_id))) {
        const int64_t e2e_us = t_after_decoded_cb - *decode_wall_t0_us;
        ALOGI(
            "McE2E native tracking_id=%u rtp_ts=%u e2e=%lldus "
            "(Decode entry -> Decoded return; native AHardwareBuffer; excludes downstream sink)",
            static_cast<unsigned>(*video_frame_tracking_id), rtp_timestamp,
            static_cast<long long>(e2e_us));
      }
      DecodeSinkRecordAfterDecoded(rtp_timestamp, t_after_decoded_cb);
      if (perf_detail) {
        perf_detail->decoded_cb_us += t_after_decoded_cb - t_cb0;
        ++perf_detail->out_buffers;
      }
      if (delivered_frames) {
        ++(*delivered_frames);
      }
#endif
    }
    if (perf_detail) {
      perf_detail->total_us = McMonotonicUs() - t_wall_start;
    }
  }

  void ProcessOneFrame(const std::vector<uint8_t>& data,
                       int64_t render_time_ms,
                       uint32_t rtp_timestamp,
                       bool is_keyframe,
                       int64_t decode_wall_t0_us,
                       const std::optional<uint16_t>& video_frame_tracking_id) {
    if (!codec_ || data.empty()) {
      return;
    }

    const int64_t* const e2e_t0_ptr = (decode_wall_t0_us > 0) ? &decode_wall_t0_us : nullptr;

    const uint64_t pfn = ++g_mc_process_frame_seq;
    const bool log_perf = (pfn <= 10u || (pfn % 30u) == 0u);
    const int64_t t_pf0 = log_perf ? McMonotonicUs() : int64_t{0};

    int64_t prep_us = 0;
    int64_t t0 = log_perf ? McMonotonicUs() : 0;
    const uint8_t* feed_ptr = data.data();
    size_t feed_size = data.size();
    if (LooksLikeAnnexB(data.data(), data.size())) {
      // 直接�?Annex B（与 McVideoDec 日志�?head16 一致）
    } else {
      AnnexBToAvcc(data.data(), data.size(), &avcc_scratch_);
      if (avcc_scratch_.empty()) {
        return;
      }
      feed_ptr = avcc_scratch_.data();
      feed_size = avcc_scratch_.size();
    }
    if (log_perf) {
      prep_us = McMonotonicUs() - t0;
    }

    t0 = log_perf ? McMonotonicUs() : 0;
    ssize_t in_idx = AMediaCodec_dequeueInputBuffer(codec_, kDequeueInputTimeoutUs);
    int64_t deq_in_us = 0;
    if (log_perf) {
      deq_in_us = McMonotonicUs() - t0;
    }
    if (in_idx < 0) {
      ALOGW("dequeueInputBuffer failed: %zd", in_idx);
      DrainOutputs(render_time_ms, rtp_timestamp, kDrainOnInputBackpressureUs, nullptr, nullptr,
                   nullptr, video_frame_tracking_id);
      return;
    }

    size_t in_cap = 0;
    uint8_t* in_buf = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(in_idx), &in_cap);
    if (!in_buf || feed_size > in_cap) {
      ALOGW("input buffer too small: need %zu have %zu", feed_size, in_cap);
      AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(in_idx), 0, 0, 0, 0);
      return;
    }
    t0 = log_perf ? McMonotonicUs() : 0;
    memcpy(in_buf, feed_ptr, feed_size);
    int64_t memcpy_in_us = 0;
    if (log_perf) {
      memcpy_in_us = McMonotonicUs() - t0;
    }

    uint32_t flags = 0;
    if (is_keyframe) {
      flags |= AMEDIACODEC_BUFFER_FLAG_KEY_FRAME;
    }
    const int64_t pts_us = next_input_pts_us_++;

    t0 = log_perf ? McMonotonicUs() : 0;
    media_status_t st = AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(in_idx), 0,
                                                      feed_size, pts_us, flags);
    int64_t q_in_us = 0;
    if (log_perf) {
      q_in_us = McMonotonicUs() - t0;
    }
    if (st != AMEDIA_OK) {
      ALOGW("queueInputBuffer failed: %d", static_cast<int>(st));
      // �?dequeue �?input 必须归还，否则解码器内部状态会错乱并放大系统层 PipelineWatcher 告警�?
      AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(in_idx), 0, 0, 0, 0);
      return;
    }
    // 首段非阻�?drain 已出帧时，不再做带超时的第二段，避免把最�?3ms 睡眠算进解码路径�?
    McDrainDetail d0{};
    McDrainDetail d1{};
    int delivered0 = 0;
    DrainOutputs(render_time_ms, rtp_timestamp, 0, log_perf ? &d0 : nullptr, &delivered0,
                 e2e_t0_ptr, video_frame_tracking_id);
    if (delivered0 > 0) {
      DrainOutputs(render_time_ms, rtp_timestamp, 0, log_perf ? &d1 : nullptr, nullptr,
                   e2e_t0_ptr, video_frame_tracking_id);
    } else {
      DrainOutputs(render_time_ms, rtp_timestamp, kDrainAfterQueueShortWaitUs,
                   log_perf ? &d1 : nullptr, nullptr, e2e_t0_ptr, video_frame_tracking_id);
    }

    // if (log_perf) {
    //   const int64_t worker_total_us = McMonotonicUs() - t_pf0;
    //   ALOGI(
    //       "McPerf #%llu worker_total=%lldus (prep=%lld deq_in=%lld memcpy_in=%lld q_in=%lld) | "
    //       "drain0: tot=%lld deq=%lld getbuf=%lld nv12_i420=%lld decoded_cb=%lld rel=%lld out=%d | "
    //       "drain1: tot=%lld deq=%lld getbuf=%lld nv12_i420=%lld decoded_cb=%lld rel=%lld out=%d | "
    //       "feed_sz=%zu key=%d "
    //       "(UI平均解码含WebRTC适配�?回调链，非本行总和)",
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

  // std::function 要求可拷贝；packaged_task 仅可移动，故�?shared_ptr 包一层�?
  auto pt = std::make_shared<std::packaged_task<bool()>>(
      [this, settings] { return impl_->ConfigureOnWorker(settings); });
  std::future<bool> fut = pt->get_future();
  {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    impl_->tasks_.clear();
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
      ALOGW("Decode called empty: sz=%zu data=%p (未进�?MediaCodec)", sz,
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
  // 端到端计时起点：与本�?EncodedImage 对应（含后续排队、worker、Decoded 回调）�?
  const int64_t decode_wall_t0_us = McMonotonicUs();
  LogMcDecodeIngress(input_image.data(), sz, rtp_ts, key, render_time_ms);

  std::vector<uint8_t> copy(sz);
  memcpy(copy.data(), input_image.data(), sz);

  {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    if (!impl_->running_) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    impl_->tasks_.push_back([this, copy = std::move(copy), render_time_ms, rtp_ts, key,
                             decode_wall_t0_us, video_frame_tracking_id]() mutable {
      impl_->ProcessOneFrame(copy, render_time_ms, rtp_ts, key, decode_wall_t0_us,
                             video_frame_tracking_id);
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
    pt = std::make_shared<std::packaged_task<void()>>([this] { impl_->DestroyCodec(); });
    fut = pt->get_future();
  }
  {
    std::lock_guard<std::mutex> lk(impl_->mu_);
    if (impl_->running_) {
      impl_->tasks_.clear();
      impl_->tasks_.push_front([pt]() { (*pt)(); });
    }
  }
  impl_->cv_.notify_one();
  if (impl_->running_) {
    fut.wait();
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
