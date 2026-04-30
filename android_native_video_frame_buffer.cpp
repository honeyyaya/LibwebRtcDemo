#include "android_native_video_frame_buffer.h"

#if defined(WEBRTC_ANDROID)

#include <unistd.h>

namespace webrtc_demo {

namespace {
constexpr char kStorageRepresentation[] = "AndroidHardwareBuffer";
}

AndroidHardwareBufferVideoFrameBuffer::AndroidHardwareBufferVideoFrameBuffer(
    AImage* image, AHardwareBuffer* hardware_buffer, int sync_fence_fd, int width, int height)
    : image_(image),
      hardware_buffer_(hardware_buffer),
      sync_fence_fd_(sync_fence_fd),
      width_(width),
      height_(height) {
  if (hardware_buffer_) {
    AHardwareBuffer_acquire(hardware_buffer_);
  }
  // Keep the AImage acquired until the GL consumer releases the frame.
  // Releasing it immediately lets ImageReader/MediaCodec recycle the slot
  // while the external texture may still be in use by the GPU.
}

AndroidHardwareBufferVideoFrameBuffer::~AndroidHardwareBufferVideoFrameBuffer() {
  if (sync_fence_fd_ >= 0) {
    close(sync_fence_fd_);
    sync_fence_fd_ = -1;
  }
  if (hardware_buffer_) {
    AHardwareBuffer_release(hardware_buffer_);
    hardware_buffer_ = nullptr;
  }
  if (image_) {
    AImage_delete(image_);
    image_ = nullptr;
  }
}

const AndroidHardwareBufferVideoFrameBuffer* AndroidHardwareBufferVideoFrameBuffer::TryGet(
    const webrtc::scoped_refptr<webrtc::VideoFrameBuffer>& buffer) {
  return TryGet(buffer.get());
}

const AndroidHardwareBufferVideoFrameBuffer* AndroidHardwareBufferVideoFrameBuffer::TryGet(
    const webrtc::VideoFrameBuffer* buffer) {
  if (!buffer || buffer->type() != webrtc::VideoFrameBuffer::Type::kNative) {
    return nullptr;
  }
  if (buffer->storage_representation() != kStorageRepresentation) {
    return nullptr;
  }
  return static_cast<const AndroidHardwareBufferVideoFrameBuffer*>(buffer);
}

void AndroidHardwareBufferVideoFrameBuffer::AddRef() const {
  ref_count_.IncRef();
}

webrtc::RefCountReleaseStatus AndroidHardwareBufferVideoFrameBuffer::Release() const {
  const auto status = ref_count_.DecRef();
  if (status == webrtc::RefCountReleaseStatus::kDroppedLastRef) {
    delete this;
  }
  return status;
}

webrtc::VideoFrameBuffer::Type AndroidHardwareBufferVideoFrameBuffer::type() const {
  return Type::kNative;
}

int AndroidHardwareBufferVideoFrameBuffer::width() const {
  return width_;
}

int AndroidHardwareBufferVideoFrameBuffer::height() const {
  return height_;
}

webrtc::scoped_refptr<webrtc::I420BufferInterface>
AndroidHardwareBufferVideoFrameBuffer::ToI420() {
  return nullptr;
}

webrtc::scoped_refptr<webrtc::VideoFrameBuffer>
AndroidHardwareBufferVideoFrameBuffer::GetMappedFrameBuffer(webrtc::ArrayView<Type> /*types*/) {
  return nullptr;
}

std::string AndroidHardwareBufferVideoFrameBuffer::storage_representation() const {
  return kStorageRepresentation;
}

int AndroidHardwareBufferVideoFrameBuffer::ConsumeSyncFenceFd() const {
  const int fd = sync_fence_fd_;
  sync_fence_fd_ = -1;
  return fd;
}

}  // namespace webrtc_demo

#endif  // defined(WEBRTC_ANDROID)
