#include "android_mediacodec_video_decoder.h"
#include "video_decode_sink_timing_bridge.h"

#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_type.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/ref_counter.h"
#include "libyuv/convert.h"

#define LOG_TAG "McVideoDec"
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// NDK r26+ 的 NdkMediaCodec.h 可能不再定义 KEY_FRAME；与 Java MediaCodec.BUFFER_FLAG_KEY_FRAME 一致。
#ifndef AMEDIACODEC_BUFFER_FLAG_KEY_FRAME
#define AMEDIACODEC_BUFFER_FLAG_KEY_FRAME 1u
#endif

namespace webrtc_demo {

// 供 Decode（类外）与匿名命名空间内共用。
int64_t McMonotonicUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::atomic<uint64_t> g_mc_e2e_log_seq{0};

namespace {

// COLOR_FormatYUV420SemiPlanar
constexpr int32_t kColorFormatNv12 = 21;
// COLOR_FormatYUV420Flexible
constexpr int32_t kColorFormatYuv420Flexible = 0x7F420888;
// 部分厂商 Codec2 在 AMessage 里使用与 color-format 不同的 android._color-format
constexpr int32_t kColorFormatQtiSurface = 2141391876;

// 与 Java MediaFormat.KEY_LOW_LATENCY 一致；部分设备在 API 30+ 上可降低解码器内部排队。
constexpr char kMediaFormatLowLatency[] = "low-latency";

// queue 后 drain：先非阻塞清空已就绪帧，再单次短阻塞吸收「刚完成」的 output。
// 原先单阶段 first_timeout=35ms 会在 worker 上串行堆叠，主观延迟远大于软解。
constexpr int64_t kDrainAfterQueueShortWaitUs = 3000;
constexpr int64_t kDrainOnInputBackpressureUs = 3000;
constexpr int64_t kDequeueInputTimeoutUs = 3000;

bool IsNv12FamilyOutput(int32_t fmt) {
  if (fmt == 0) {
    return true;
  }
  return fmt == kColorFormatNv12 || fmt == kColorFormatYuv420Flexible ||
         fmt == kColorFormatQtiSurface;
}

// WebRTC H264 接收路径多为 Annex B（00 00 01 / 00 00 00 01）。Codec2 解码器通常要这种输入；
// 若误转成 AVCC（4 字节长度前缀），部分机型会吃满 input 但永远不出 output（fps=0）。
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

// NV12→I420 直接写入已分配好的 dst 平面（不做任何分配）。
bool FillI420FromNv12(const uint8_t* src, size_t src_cap, int offset,
                      int width, int height, int y_stride, int slice_height,
                      uint8_t* dy, int dsy, uint8_t* du, int dsu, uint8_t* dv, int dsv) {
  if (width <= 0 || height <= 0 || y_stride < width) return false;
  const int y_plane = y_stride * slice_height;
  if (offset + y_plane + y_stride * (slice_height / 2) > static_cast<int>(src_cap)) return false;
  const uint8_t* ys = src + offset;
  return libyuv::NV12ToI420(ys, y_stride, ys + y_plane, y_stride,
                            dy, dsy, du, dsu, dv, dsv, width, height) == 0;
}

bool FillI420FromNv12Tight(const uint8_t* src, size_t src_cap, int offset,
                           int width, int height,
                           uint8_t* dy, int dsy, uint8_t* du, int dsu, uint8_t* dv, int dsv) {
  if (width <= 0 || height <= 0) return false;
  const int need = width * height + width * (height / 2);
  if (offset < 0 || offset + need > static_cast<int>(src_cap)) return false;
  const uint8_t* ys = src + offset;
  return libyuv::NV12ToI420(ys, width, ys + width * height, width,
                            dy, dsy, du, dsu, dv, dsv, width, height) == 0;
}

// ---------------------------------------------------------------------------
// I420 内存池：避免每帧 mmap + 338 次 page-fault（1280×720 ≈ 1.38 MB）。
// 槽位内存通过 shared_ptr 与 PooledI420 共享生命周期，即使解码器销毁时
// 仍有帧在渲染管线中也不会产生悬垂指针。
// ---------------------------------------------------------------------------
struct I420PoolSlot {
  std::shared_ptr<uint8_t> mem;
  int width = 0, height = 0;
  int stride_y = 0, stride_u = 0, stride_v = 0;
  size_t off_u = 0, off_v = 0;
  std::atomic<bool> free{true};
};

class PooledI420 final : public webrtc::I420BufferInterface {
 public:
  PooledI420(std::shared_ptr<uint8_t> m, int w, int h,
             int sy, int su, int sv, size_t ou, size_t ov,
             std::atomic<bool>* flag)
      : m_(std::move(m)), w_(w), h_(h),
        sy_(sy), su_(su), sv_(sv), ou_(ou), ov_(ov), flag_(flag) {}
  ~PooledI420() override {
    if (flag_) flag_->store(true, std::memory_order_release);
  }

  void AddRef() const override { rc_.IncRef(); }
  webrtc::RefCountReleaseStatus Release() const override {
    auto s = rc_.DecRef();
    if (s == webrtc::RefCountReleaseStatus::kDroppedLastRef) delete this;
    return s;
  }

  int width() const override { return w_; }
  int height() const override { return h_; }
  const uint8_t* DataY() const override { return m_.get(); }
  const uint8_t* DataU() const override { return m_.get() + ou_; }
  const uint8_t* DataV() const override { return m_.get() + ov_; }
  int StrideY() const override { return sy_; }
  int StrideU() const override { return su_; }
  int StrideV() const override { return sv_; }

 private:
  std::shared_ptr<uint8_t> m_;
  int w_, h_, sy_, su_, sv_;
  size_t ou_, ov_;
  std::atomic<bool>* flag_;
  mutable webrtc::webrtc_impl::RefCounter rc_{0};
};

std::atomic<uint64_t> g_mc_decode_ingress_seq{0};
std::atomic<uint64_t> g_mc_process_frame_seq{0};

// 单次 DrainOutputs 内部细分（微秒）；perf_detail==nullptr 时不采样，避免热路径开销。
struct McDrainDetail {
  int64_t total_us = 0;
  int64_t dequeue_us = 0;
  int64_t get_out_buf_us = 0;
  int64_t nv12_i420_us = 0;
  int64_t decoded_cb_us = 0;
  int64_t release_us = 0;
  int out_buffers = 0;
};

// 确认 WebRTC 是否把 EncodedImage 送进本解码器：前 10 帧 + 之后每 30 帧打一条，避免刷屏。
void LogMcDecodeIngress(const uint8_t* data,
                        size_t sz,
                        uint32_t rtp_ts,
                        bool key,
                        int64_t render_time_ms) {
  const uint64_t n = ++g_mc_decode_ingress_seq;
  char head_hex[3 * 16 + 1] = {0};
  const int nshow = (sz >= 16) ? 16 : static_cast<int>(sz);
  for (int i = 0; i < nshow; ++i) {
    snprintf(head_hex + i * 3, 4, "%02x ", static_cast<unsigned int>(data[i]));
  }
  if (n <= 10u || (n % 30u) == 0u) {
    ALOGI(
        "Decode ingress #%llu sz=%zu rtp_ts=%u key=%d render_ms=%lld head16=[%s] "
        "(00 00 01=AnnexB; 4byte_len=AVCC)",
        static_cast<unsigned long long>(n), sz, rtp_ts, key ? 1 : 0,
        static_cast<long long>(render_time_ms), head_hex);
  }
}

}  // namespace

struct AndroidMediaCodecVideoDecoder::Impl {
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  bool running_ = false;
  std::thread thread_;

  AMediaCodec* codec_ = nullptr;

  webrtc::DecodedImageCallback* callback_ = nullptr;

  int out_width_ = 0;
  int out_height_ = 0;
  int y_stride_ = 0;
  int slice_height_ = 0;
  int32_t color_format_ = 0;

  std::vector<uint8_t> avcc_scratch_;

  // 输入 PTS 使用单调递增时间戳；RTP 时间戳会回绕/乱序，易让 Codec2 PipelineWatcher 产生噪声告警。
  int64_t next_input_pts_us_ = 0;

  static constexpr int kPoolSlots = 6;
  I420PoolSlot pool_[kPoolSlots];

  struct PoolResult {
    uint8_t* y; uint8_t* u; uint8_t* v;
    int sy, su, sv;
    size_t ou, ov;
    std::shared_ptr<uint8_t> mem;
    std::atomic<bool>* flag;
  };

  bool AcquirePoolSlot(int w, int h, PoolResult* r) {
    const int sy = w, su = (w + 1) / 2, sv = su;
    const int hh = (h + 1) / 2;
    const size_t ou = static_cast<size_t>(sy) * h;
    const size_t ov = ou + static_cast<size_t>(su) * hh;
    const size_t total = ov + static_cast<size_t>(sv) * hh;
    for (auto& s : pool_) {
      if (!s.free.load(std::memory_order_acquire)) continue;
      if (s.width == w && s.height == h && s.mem) {
        s.free.store(false, std::memory_order_relaxed);
        r->y = s.mem.get(); r->u = r->y + s.off_u; r->v = r->y + s.off_v;
        r->sy = s.stride_y; r->su = s.stride_u; r->sv = s.stride_v;
        r->ou = s.off_u; r->ov = s.off_v;
        r->mem = s.mem; r->flag = &s.free;
        return true;
      }
    }
    for (auto& s : pool_) {
      if (!s.free.load(std::memory_order_acquire)) continue;
      s.mem.reset(new uint8_t[total], std::default_delete<uint8_t[]>());
      s.width = w; s.height = h;
      s.stride_y = sy; s.stride_u = su; s.stride_v = sv;
      s.off_u = ou; s.off_v = ov;
      s.free.store(false, std::memory_order_relaxed);
      r->y = s.mem.get(); r->u = r->y + ou; r->v = r->y + ov;
      r->sy = sy; r->su = su; r->sv = sv;
      r->ou = ou; r->ov = ov;
      r->mem = s.mem; r->flag = &s.free;
      return true;
    }
    return false;
  }

  void ClearPool() {
    for (auto& s : pool_) {
      s.mem.reset();
      s.width = s.height = 0;
      s.free.store(true, std::memory_order_relaxed);
    }
  }

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
    out_width_ = out_height_ = y_stride_ = slice_height_ = 0;
    color_format_ = 0;
    ClearPool();
  }

  void UpdateOutputFormat(AMediaFormat* fmt) {
    if (!fmt) {
      return;
    }
    AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, &out_width_);
    AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, &out_height_);
    if (!AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, &color_format_)) {
      int32_t alt = 0;
      if (AMediaFormat_getInt32(fmt, "android._color-format", &alt)) {
        color_format_ = alt;
      }
    }
    if (!AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_STRIDE, &y_stride_) || y_stride_ < out_width_) {
      y_stride_ = out_width_;
    }
    if (!AMediaFormat_getInt32(fmt, "slice-height", &slice_height_) || slice_height_ < out_height_) {
      slice_height_ = out_height_;
    }
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

    media_status_t st = AMediaCodec_configure(codec_, format, nullptr, nullptr, 0);
    AMediaFormat_delete(format);
    if (st != AMEDIA_OK) {
      ALOGW("AMediaCodec_configure failed: %d", static_cast<int>(st));
      AMediaCodec_delete(codec_);
      codec_ = nullptr;
      return false;
    }
    st = AMediaCodec_start(codec_);
    if (st != AMEDIA_OK) {
      ALOGW("AMediaCodec_start failed: %d", static_cast<int>(st));
      AMediaCodec_delete(codec_);
      codec_ = nullptr;
      return false;
    }
    next_input_pts_us_ = 0;
    // 尽早拉取输出格式（部分 Codec2 在首帧 output 前不会单独触发 INFO，导致 out_width_ 一直为 0）
    RefreshOutputFormat();
    return true;
  }

  void DrainOutputs(int64_t render_time_ms,
                    uint32_t rtp_timestamp,
                    int64_t first_dequeue_timeout_us,
                    McDrainDetail* perf_detail,
                    int* delivered_frames,
                    const int64_t* decode_wall_t0_us) {
    if (!codec_) {
      return;
    }
    const int64_t t_wall_start = perf_detail ? McMonotonicUs() : int64_t{0};
    webrtc::DecodedImageCallback* cb = nullptr;
    {
      std::lock_guard<std::mutex> lk(mu_);
      cb = callback_;
    }
    // 无论是否已注册 callback，都必须 dequeue 并 release output，否则会塞满管道 in=0 out=0、解码帧恒为 0

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

      if (info.size > 0 && out_width_ > 0 && out_height_ > 0 && cb &&
          IsNv12FamilyOutput(color_format_)) {
        size_t cap = 0;
        int64_t t_gb0 = 0;
        if (perf_detail) {
          t_gb0 = McMonotonicUs();
        }
        uint8_t* out_buf = AMediaCodec_getOutputBuffer(codec_, static_cast<size_t>(out_idx), &cap);
        if (perf_detail) {
          perf_detail->get_out_buf_us += McMonotonicUs() - t_gb0;
        }
        if (out_buf && static_cast<size_t>(info.offset) + static_cast<size_t>(info.size) <= cap) {
          int64_t t_c0 = 0;
          if (perf_detail) {
            t_c0 = McMonotonicUs();
          }
          webrtc::scoped_refptr<webrtc::I420BufferInterface> i420;
          PoolResult pr{};
          if (AcquirePoolSlot(out_width_, out_height_, &pr)) {
            const bool ok =
                FillI420FromNv12(out_buf, cap, info.offset, out_width_, out_height_,
                                 y_stride_, slice_height_,
                                 pr.y, pr.sy, pr.u, pr.su, pr.v, pr.sv) ||
                FillI420FromNv12Tight(out_buf, cap, info.offset, out_width_, out_height_,
                                      pr.y, pr.sy, pr.u, pr.su, pr.v, pr.sv);
            if (ok) {
              i420 = webrtc::scoped_refptr<PooledI420>(
                  new PooledI420(pr.mem, out_width_, out_height_,
                                 pr.sy, pr.su, pr.sv, pr.ou, pr.ov, pr.flag));
            } else {
              pr.flag->store(true, std::memory_order_release);
            }
          }
          if (!i420) {
            auto buf420 = webrtc::I420Buffer::Create(out_width_, out_height_);
            if (buf420) {
              const bool ok =
                  FillI420FromNv12(out_buf, cap, info.offset, out_width_, out_height_,
                                   y_stride_, slice_height_,
                                   buf420->MutableDataY(), buf420->StrideY(),
                                   buf420->MutableDataU(), buf420->StrideU(),
                                   buf420->MutableDataV(), buf420->StrideV()) ||
                  FillI420FromNv12Tight(out_buf, cap, info.offset, out_width_, out_height_,
                                        buf420->MutableDataY(), buf420->StrideY(),
                                        buf420->MutableDataU(), buf420->StrideU(),
                                        buf420->MutableDataV(), buf420->StrideV());
              if (ok) i420 = buf420;
            }
          }
          if (perf_detail) {
            perf_detail->nv12_i420_us += McMonotonicUs() - t_c0;
          }
          if (i420) {
            webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                                           .set_video_frame_buffer(i420)
                                           .set_rtp_timestamp(rtp_timestamp)
                                           .set_timestamp_us(render_time_ms * 1000)
                                           .build();
            int64_t t_cb0 = 0;
            if (perf_detail) {
              t_cb0 = McMonotonicUs();
            }
            cb->Decoded(frame);
            const int64_t t_after_decoded_cb = McMonotonicUs();
            if (decode_wall_t0_us) {
              const int64_t e2e_us = t_after_decoded_cb - *decode_wall_t0_us;
              const uint64_t n = ++g_mc_e2e_log_seq;
              if (n <= 10u || (n % 30u) == 0u) {
                ALOGI(
                    "McE2E #%llu rtp_ts=%u e2e=%lldus (Decode入口->Decoded返回; "
                    "含入队等待+worker+MediaCodec+NV12->I420+cb; 不含WebRTC后续sink)",
                    static_cast<unsigned long long>(n), rtp_timestamp,
                    static_cast<long long>(e2e_us));
              }
            }
            // D→E：Decoded 返回时刻，供 sink 侧 OnFrame 计算 WebRTC 内部投递延迟。
            DecodeSinkRecordAfterDecoded(rtp_timestamp, t_after_decoded_cb);
            if (perf_detail) {
              perf_detail->decoded_cb_us += t_after_decoded_cb - t_cb0;
              ++perf_detail->out_buffers;
            }
            if (delivered_frames) {
              ++(*delivered_frames);
            }
          } else {
            ALOGW("NV12->I420 failed w=%d h=%d stride=%d slice=%d color=%d cap=%zu off=%d size=%d",
                  out_width_, out_height_, y_stride_, slice_height_,
                  static_cast<int>(color_format_), cap, info.offset, info.size);
          }
        }
      } else if (info.size > 0 && out_width_ > 0 && out_height_ > 0 && cb) {
        ALOGW("Unsupported color format %d (size=%d)", static_cast<int>(color_format_),
              static_cast<int>(info.size));
      }

      int64_t t_rel0 = 0;
      if (perf_detail) {
        t_rel0 = McMonotonicUs();
      }
      AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(out_idx), false);
      if (perf_detail) {
        perf_detail->release_us += McMonotonicUs() - t_rel0;
      }
    }
    if (perf_detail) {
      perf_detail->total_us = McMonotonicUs() - t_wall_start;
    }
  }

  void ProcessOneFrame(const std::vector<uint8_t>& data,
                       int64_t render_time_ms,
                       uint32_t rtp_timestamp,
                       bool is_keyframe,
                       int64_t decode_wall_t0_us) {
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
      // 直接送 Annex B（与 McVideoDec 日志里 head16 一致）
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
                   nullptr);
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
      // 已 dequeue 的 input 必须归还，否则解码器内部状态会错乱并放大系统层 PipelineWatcher 告警。
      AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(in_idx), 0, 0, 0, 0);
      return;
    }
    // 首段非阻塞 drain 已出帧时，不再做带超时的第二段，避免把最多 3ms 睡眠算进解码路径。
    McDrainDetail d0{};
    McDrainDetail d1{};
    int delivered0 = 0;
    DrainOutputs(render_time_ms, rtp_timestamp, 0, log_perf ? &d0 : nullptr, &delivered0,
                 e2e_t0_ptr);
    if (delivered0 > 0) {
      DrainOutputs(render_time_ms, rtp_timestamp, 0, log_perf ? &d1 : nullptr, nullptr,
                   e2e_t0_ptr);
    } else {
      DrainOutputs(render_time_ms, rtp_timestamp, kDrainAfterQueueShortWaitUs,
                   log_perf ? &d1 : nullptr, nullptr, e2e_t0_ptr);
    }

    if (log_perf) {
      const int64_t worker_total_us = McMonotonicUs() - t_pf0;
      ALOGI(
          "McPerf #%llu worker_total=%lldus (prep=%lld deq_in=%lld memcpy_in=%lld q_in=%lld) | "
          "drain0: tot=%lld deq=%lld getbuf=%lld nv12_i420=%lld decoded_cb=%lld rel=%lld out=%d | "
          "drain1: tot=%lld deq=%lld getbuf=%lld nv12_i420=%lld decoded_cb=%lld rel=%lld out=%d | "
          "feed_sz=%zu key=%d "
          "(UI平均解码含WebRTC适配器+回调链，非本行总和)",
          static_cast<unsigned long long>(pfn), static_cast<long long>(worker_total_us),
          static_cast<long long>(prep_us), static_cast<long long>(deq_in_us),
          static_cast<long long>(memcpy_in_us), static_cast<long long>(q_in_us),
          static_cast<long long>(d0.total_us), static_cast<long long>(d0.dequeue_us),
          static_cast<long long>(d0.get_out_buf_us), static_cast<long long>(d0.nv12_i420_us),
          static_cast<long long>(d0.decoded_cb_us), static_cast<long long>(d0.release_us),
          d0.out_buffers, static_cast<long long>(d1.total_us),
          static_cast<long long>(d1.dequeue_us), static_cast<long long>(d1.get_out_buf_us),
          static_cast<long long>(d1.nv12_i420_us), static_cast<long long>(d1.decoded_cb_us),
          static_cast<long long>(d1.release_us), d1.out_buffers, feed_size, is_keyframe ? 1 : 0);
    }
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

  // std::function 要求可拷贝；packaged_task 仅可移动，故用 shared_ptr 包一层。
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
      ALOGW("Decode called empty: sz=%zu data=%p (未进入 MediaCodec)", sz,
            static_cast<const void*>(input_image.data()));
    }
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  const uint32_t rtp_ts = input_image.RtpTimestamp();
  const bool key = (input_image.FrameType() == webrtc::VideoFrameType::kVideoFrameKey);
  // 端到端计时起点：与本帧 EncodedImage 对应（含后续排队、worker、Decoded 回调）。
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
                             decode_wall_t0_us]() mutable {
      impl_->ProcessOneFrame(copy, render_time_ms, rtp_ts, key, decode_wall_t0_us);
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
