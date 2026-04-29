#pragma once

#include "api/scoped_refptr.h"
#include "api/video/video_frame_buffer.h"
#include "rtc_base/ref_counter.h"

#include <string>

#if defined(WEBRTC_ANDROID)
#include <android/hardware_buffer.h>
#include <media/NdkImage.h>
#endif

namespace webrtc_demo {

#if defined(WEBRTC_ANDROID)

class AndroidHardwareBufferVideoFrameBuffer final : public webrtc::VideoFrameBuffer {
 public:
  AndroidHardwareBufferVideoFrameBuffer(AImage* image,
                                        AHardwareBuffer* hardware_buffer,
                                        int sync_fence_fd,
                                        int width,
                                        int height);
  ~AndroidHardwareBufferVideoFrameBuffer() override;

  static const AndroidHardwareBufferVideoFrameBuffer* TryGet(
      const webrtc::scoped_refptr<webrtc::VideoFrameBuffer>& buffer);
  static const AndroidHardwareBufferVideoFrameBuffer* TryGet(
      const webrtc::VideoFrameBuffer* buffer);

  void AddRef() const override;
  webrtc::RefCountReleaseStatus Release() const override;

  Type type() const override;
  int width() const override;
  int height() const override;
  webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override;
  webrtc::scoped_refptr<webrtc::VideoFrameBuffer> GetMappedFrameBuffer(
      webrtc::ArrayView<Type> types) override;
  std::string storage_representation() const override;

  AHardwareBuffer* hardware_buffer() const { return hardware_buffer_; }
  int ConsumeSyncFenceFd() const;

 private:
  AImage* image_ = nullptr;
  AHardwareBuffer* hardware_buffer_ = nullptr;
  mutable int sync_fence_fd_ = -1;
  int width_ = 0;
  int height_ = 0;
  mutable webrtc::webrtc_impl::RefCounter ref_count_{0};
};

#endif  // defined(WEBRTC_ANDROID)

}  // namespace webrtc_demo
