#ifndef WEBRTC_OES_VIDEO_FRAME_BUFFER_H
#define WEBRTC_OES_VIDEO_FRAME_BUFFER_H

#include <functional>
#include <string>

#include "api/array_view.h"
#include "api/video/video_frame_buffer.h"
#include "api/scoped_refptr.h"
#include "rtc_base/ref_counted_object.h"

#include <cstdint>

using webrtc_oes_uint32 = std::uint32_t;

namespace webrtc_demo {

/// 表示 SurfaceTexture / MediaCodec Surface 输出的 GL_TEXTURE_EXTERNAL_OES，及可选的 EGLImage 同槽绑定名。
/// 在 GL 线程绘制前会调用 @p before_sample_in_gl_context（例如 @c ANativeWindow_* 或
/// <code>SurfaceTexture::updateTexImage</code> 经 JNI 桥接），保证 samplerExternalOES 读到新帧。
/// 实现须：与此 OES 相同 EGL/GL 上下文、与 RunBeforeSample 同线程；禁止 glFinish / glFlush 与跨线程 GL
/// 阻塞等待；OES 须与 Quick 渲染端处于可共享纹理的 context/share group，勿在解码线程上 updateTexImage。
class OesEglTextureFrameBuffer : public webrtc::VideoFrameBuffer {
 public:
  OesEglTextureFrameBuffer(int width,
                           int height,
                           webrtc_oes_uint32 oes_texture_id,
                           std::function<void()> before_sample_in_gl_context)
      : w_(width),
        h_(height),
        oes_(oes_texture_id),
        before_(std::move(before_sample_in_gl_context)) {}

  webrtc::VideoFrameBuffer::Type type() const override {
    return webrtc::VideoFrameBuffer::Type::kNative;
  }
  int width() const override { return w_; }
  int height() const override { return h_; }

  webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override { return nullptr; }

  webrtc::scoped_refptr<webrtc::VideoFrameBuffer> GetMappedFrameBuffer(
      webrtc::ArrayView<webrtc::VideoFrameBuffer::Type> /*types*/) override {
    return nullptr;
  }
  std::string storage_representation() const override { return "OES_external_or_EGLImage"; }

  webrtc_oes_uint32 oes_texture_id() const { return oes_; }
  const std::function<void()>& before_sample() const { return before_; }
  void RunBeforeSampleOnGlThread() const {
    if (before_) {
      before_();
    }
  }

  static OesEglTextureFrameBuffer* Cast(webrtc::VideoFrameBuffer* b) {
    if (!b || b->type() != webrtc::VideoFrameBuffer::Type::kNative) {
      return nullptr;
    }
    return dynamic_cast<OesEglTextureFrameBuffer*>(b);
  }

  static const OesEglTextureFrameBuffer* Cast(const webrtc::VideoFrameBuffer* b) {
    if (!b || b->type() != webrtc::VideoFrameBuffer::Type::kNative) {
      return nullptr;
    }
    return dynamic_cast<const OesEglTextureFrameBuffer*>(b);
  }

 private:
  int w_ = 0;
  int h_ = 0;
  webrtc_oes_uint32 oes_ = 0;
  std::function<void()> before_;
};

/// 用 WebRTC 官方 RefCountedObject 包装，作为 VideoFrame 的 kNative 缓冲挂入管线。
inline webrtc::scoped_refptr<webrtc::VideoFrameBuffer> CreateOesEglTextureFrameBuffer(
    int width,
    int height,
    webrtc_oes_uint32 oes_texture_id,
    std::function<void()> before_sample_in_gl_context) {
  return webrtc::scoped_refptr<webrtc::VideoFrameBuffer>(
      new webrtc::RefCountedObject<OesEglTextureFrameBuffer>(
          width, height, oes_texture_id, std::move(before_sample_in_gl_context)));
}

}  // namespace webrtc_demo

#endif  // WEBRTC_OES_VIDEO_FRAME_BUFFER_H
