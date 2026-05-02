#include "webrtc_video_renderer.h"

#if defined(WEBRTC_ANDROID)
#include "android_native_video_frame_buffer.h"
#endif
#include "encoded_tracking_bridge.h"
#include "video_decode_sink_timing_bridge.h"

#include <QByteArray>
#include <QDebug>
#include <QElapsedTimer>
#include <QMatrix4x4>
#include <QMetaObject>
#include <QMutexLocker>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QQuickWindow>
#include <QSGNode>
#include <QSGRenderNode>
#include <QSGRendererInterface>
#include <QSurfaceFormat>
#include <QThread>

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <list>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(WEBRTC_ANDROID)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#include <android/hardware_buffer.h>
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#endif

#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef GL_LUMINANCE_ALPHA
#define GL_LUMINANCE_ALPHA 0x190A
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif
#ifndef GL_MAP_WRITE_BIT
#define GL_MAP_WRITE_BIT 0x0002
#endif
#ifndef GL_MAP_INVALIDATE_BUFFER_BIT
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#endif
#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

namespace {

struct PlaneTextureCache {
  int width = 0;
  int height = 0;
};

struct PlaneUploadPbo {
  GLuint ids[2] = {0, 0};
  int nextStageIndex = 0;
  int readyIndex = -1;
  int readyWidth = 0;
  int readyHeight = 0;
  int readyStrideBytes = 0;
  GLenum readyInternalFormat = GL_LUMINANCE;
  GLenum readyFormat = GL_LUMINANCE;
  int readyBytesPerPixel = 1;
  bool ready = false;
};

struct PlanePipelineResult {
  bool uploaded = false;
  bool staged = false;
};

enum class UploadPathKind {
  None,
  Native,
  I420,
  NV12,
};

const char* UploadPathKindName(UploadPathKind kind) {
  switch (kind) {
    case UploadPathKind::Native:
      return "AHardwareBuffer";
    case UploadPathKind::NV12:
      return "NV12";
    case UploadPathKind::I420:
      return "I420";
    case UploadPathKind::None:
    default:
      return "None";
  }
}

constexpr int kEncodedIngressPollIntervalMs = 250;
constexpr int64_t kHighlightSignalThrottleUs = 200000;

#if defined(WEBRTC_ANDROID)
constexpr int kNativeTextureSlotCount = 6;
constexpr size_t kMaxNativeImportedImageCacheEntries = 16;

enum class NativeImportCacheEvent {
  None,
  Hit,
  MissCreated,
  MissNoSpace,
  MissCreateFailed,
  MissNoClientBuffer,
};

const char* NativeImportCacheEventName(NativeImportCacheEvent event) {
  switch (event) {
    case NativeImportCacheEvent::Hit:
      return "hit";
    case NativeImportCacheEvent::MissCreated:
      return "miss_create";
    case NativeImportCacheEvent::MissNoSpace:
      return "miss_no_space";
    case NativeImportCacheEvent::MissCreateFailed:
      return "miss_create_failed";
    case NativeImportCacheEvent::MissNoClientBuffer:
      return "miss_no_client_buffer";
    case NativeImportCacheEvent::None:
    default:
      return "none";
  }
}

struct NativeImportedImageCacheEntry {
  EGLImageKHR egl_image = EGL_NO_IMAGE_KHR;
  std::list<AHardwareBuffer*>::iterator lru_it{};
};

using NativeImportedImageCache = std::unordered_map<AHardwareBuffer*, NativeImportedImageCacheEntry>;

struct NativeTextureSlot {
  GLuint texture_id = 0;
  AHardwareBuffer* bound_hardware_buffer = nullptr;
  webrtc::scoped_refptr<webrtc::VideoFrameBuffer> held_buffer;
  GLsync reuse_fence = nullptr;
};

struct NativeImportTrace {
  NativeImportCacheEvent cache_event = NativeImportCacheEvent::None;
  bool evicted = false;
  bool slot_unavailable = false;
  int slot_index = -1;
  int cache_entries = 0;
  qint64 egl_create_image_us = 0;
};
#endif

bool SupportsUnpackRowLength() {
  QOpenGLContext* const ctx = QOpenGLContext::currentContext();
  if (!ctx) {
    return false;
  }

  const QSurfaceFormat fmt = ctx->format();
  if (!ctx->isOpenGLES()) {
    return true;
  }
  if (fmt.majorVersion() >= 3) {
    return true;
  }
  return ctx->hasExtension(QByteArrayLiteral("GL_EXT_unpack_subimage"));
}

bool SupportsPixelUnpackBuffer() {
  QOpenGLContext* const ctx = QOpenGLContext::currentContext();
  if (!ctx) {
    return false;
  }
  if (!ctx->isOpenGLES()) {
    return true;
  }
  return ctx->format().majorVersion() >= 3;
}

bool SupportsExternalOesSampling() {
  QOpenGLContext* const ctx = QOpenGLContext::currentContext();
  if (!ctx || !ctx->isOpenGLES()) {
    return false;
  }
  return ctx->hasExtension(QByteArrayLiteral("GL_OES_EGL_image_external"));
}

bool SupportsGpuReuseSync() {
  QOpenGLContext* const ctx = QOpenGLContext::currentContext();
  if (!ctx) {
    return false;
  }
  if (!ctx->isOpenGLES()) {
    return true;
  }
  return ctx->format().majorVersion() >= 3;
}

#if defined(WEBRTC_ANDROID)
bool HasExtensionToken(const char* extensions, const char* needle) {
  if (!extensions || !needle || !needle[0]) {
    return false;
  }

  const QByteArray haystack(extensions);
  const QByteArray token(needle);
  int pos = 0;
  while ((pos = haystack.indexOf(token, pos)) >= 0) {
    const bool start_ok = pos == 0 || haystack.at(pos - 1) == ' ';
    const int end_pos = pos + token.size();
    const bool end_ok = end_pos == haystack.size() || haystack.at(end_pos) == ' ';
    if (start_ok && end_ok) {
      return true;
    }
    pos = end_pos;
  }
  return false;
}

constexpr int kNativeFenceWaitTimeoutMs = 0;

enum class NativeFenceWaitStatus {
  Ready,
  Pending,
  Failed,
};

NativeFenceWaitStatus WaitForNativeFenceFd(int fenceFd, int timeoutMs) {
  if (fenceFd < 0) {
    return NativeFenceWaitStatus::Ready;
  }

  pollfd pfd{};
  pfd.fd = fenceFd;
  pfd.events = POLLIN;

  int poll_result = -1;
  do {
    poll_result = poll(&pfd, 1, timeoutMs);
  } while (poll_result < 0 && errno == EINTR);

  close(fenceFd);
  if (poll_result == 0) {
    return NativeFenceWaitStatus::Pending;
  }
  if (poll_result > 0 && (pfd.revents & POLLNVAL) == 0) {
    return NativeFenceWaitStatus::Ready;
  }
  return NativeFenceWaitStatus::Failed;
}
#endif

const uint8_t* PrepareTightPlaneIfNeeded(const uint8_t* src, int srcStrideBytes, int planeWidth,
                                         int planeHeight, int bytesPerPixel,
                                         bool canUseUnpackRowLength,
                                         std::vector<uint8_t>& tightBuffer) {
  const int rowBytes = planeWidth * bytesPerPixel;
  if (canUseUnpackRowLength || srcStrideBytes == rowBytes) {
    return src;
  }

  tightBuffer.resize(static_cast<size_t>(rowBytes) * static_cast<size_t>(planeHeight));
  for (int row = 0; row < planeHeight; ++row) {
    std::memcpy(tightBuffer.data() + static_cast<size_t>(row) * static_cast<size_t>(rowBytes),
                src + static_cast<size_t>(row) * static_cast<size_t>(srcStrideBytes),
                static_cast<size_t>(rowBytes));
  }
  return tightBuffer.data();
}

void UploadPlane(QOpenGLExtraFunctions& gl, GLenum textureUnit, GLuint textureId, int planeWidth,
                 int planeHeight, GLenum internalFormat, GLenum format, int bytesPerPixel,
                 const uint8_t* planeData, int strideBytes, bool canUseUnpackRowLength,
                 PlaneTextureCache& cache, std::vector<uint8_t>& tightBuffer) {
  gl.glActiveTexture(textureUnit);
  gl.glBindTexture(GL_TEXTURE_2D, textureId);

  const bool sameSize = cache.width == planeWidth && cache.height == planeHeight;
  if (!sameSize) {
    cache.width = planeWidth;
    cache.height = planeHeight;
    gl.glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, planeWidth, planeHeight, 0, format,
                    GL_UNSIGNED_BYTE, nullptr);
  }

  gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  const uint8_t* uploadData = planeData;
  int rowLengthPixels = strideBytes / bytesPerPixel;
  if (!canUseUnpackRowLength) {
    uploadData = PrepareTightPlaneIfNeeded(planeData, strideBytes, planeWidth, planeHeight,
                                           bytesPerPixel, false, tightBuffer);
    rowLengthPixels = planeWidth;
  }

  if (canUseUnpackRowLength) {
    gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLengthPixels);
  }
  gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, planeWidth, planeHeight, format, GL_UNSIGNED_BYTE,
                     uploadData);
  if (canUseUnpackRowLength) {
    gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  }
}

void ResetPlaneUploadPbo(PlaneUploadPbo& pbo) {
  pbo.nextStageIndex = 0;
  pbo.readyIndex = -1;
  pbo.readyWidth = 0;
  pbo.readyHeight = 0;
  pbo.readyStrideBytes = 0;
  pbo.readyInternalFormat = GL_LUMINANCE;
  pbo.readyFormat = GL_LUMINANCE;
  pbo.readyBytesPerPixel = 1;
  pbo.ready = false;
}

size_t CopyPlaneForUpload(uint8_t* dst, const uint8_t* src, int srcStrideBytes, int planeWidth,
                          int planeHeight, int bytesPerPixel, bool canUseUnpackRowLength) {
  const int rowBytes = planeWidth * bytesPerPixel;
  if (canUseUnpackRowLength && srcStrideBytes != rowBytes) {
    const size_t totalBytes =
        static_cast<size_t>(srcStrideBytes) * static_cast<size_t>(planeHeight);
    std::memcpy(dst, src, totalBytes);
    return totalBytes;
  }

  for (int row = 0; row < planeHeight; ++row) {
    std::memcpy(dst + static_cast<size_t>(row) * static_cast<size_t>(rowBytes),
                src + static_cast<size_t>(row) * static_cast<size_t>(srcStrideBytes),
                static_cast<size_t>(rowBytes));
  }
  return static_cast<size_t>(rowBytes) * static_cast<size_t>(planeHeight);
}

bool UploadPlaneFromReadyPbo(QOpenGLExtraFunctions& gl, GLenum textureUnit, GLuint textureId,
                             bool canUseUnpackRowLength, PlaneTextureCache& cache,
                             PlaneUploadPbo& pbo) {
  if (!pbo.ready || pbo.readyIndex < 0) {
    return false;
  }

  gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo.ids[pbo.readyIndex]);
  gl.glActiveTexture(textureUnit);
  gl.glBindTexture(GL_TEXTURE_2D, textureId);

  const bool sameSize = cache.width == pbo.readyWidth && cache.height == pbo.readyHeight;
  if (!sameSize) {
    cache.width = pbo.readyWidth;
    cache.height = pbo.readyHeight;
    gl.glTexImage2D(GL_TEXTURE_2D, 0, pbo.readyInternalFormat, pbo.readyWidth, pbo.readyHeight, 0,
                    pbo.readyFormat, GL_UNSIGNED_BYTE, nullptr);
  }

  gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  if (canUseUnpackRowLength) {
    gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, pbo.readyStrideBytes / pbo.readyBytesPerPixel);
  }
  gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pbo.readyWidth, pbo.readyHeight, pbo.readyFormat,
                     GL_UNSIGNED_BYTE, nullptr);
  if (canUseUnpackRowLength) {
    gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  }
  gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  return true;
}

bool StagePlaneIntoPbo(QOpenGLExtraFunctions& gl, int planeWidth, int planeHeight,
                       GLenum internalFormat, GLenum format, int bytesPerPixel,
                       const uint8_t* planeData, int srcStrideBytes, bool canUseUnpackRowLength,
                       PlaneUploadPbo& pbo) {
  if (!pbo.ids[0] || !pbo.ids[1]) {
    return false;
  }

  const int index = pbo.nextStageIndex;
  pbo.nextStageIndex = (index + 1) % 2;

  const int rowBytes = planeWidth * bytesPerPixel;
  const size_t uploadBytes =
      (canUseUnpackRowLength && srcStrideBytes != rowBytes)
          ? static_cast<size_t>(srcStrideBytes) * static_cast<size_t>(planeHeight)
          : static_cast<size_t>(rowBytes) * static_cast<size_t>(planeHeight);

  gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo.ids[index]);
  gl.glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(uploadBytes), nullptr,
                  GL_STREAM_DRAW);

  void* mapped = gl.glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0,
                                     static_cast<GLsizeiptr>(uploadBytes),
                                     GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
  if (!mapped) {
    gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    return false;
  }

  CopyPlaneForUpload(static_cast<uint8_t*>(mapped), planeData, srcStrideBytes, planeWidth,
                     planeHeight, bytesPerPixel, canUseUnpackRowLength);
  if (gl.glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER) == GL_FALSE) {
    gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    return false;
  }

  gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  pbo.readyIndex = index;
  pbo.readyWidth = planeWidth;
  pbo.readyHeight = planeHeight;
  pbo.readyStrideBytes = canUseUnpackRowLength ? srcStrideBytes : rowBytes;
  pbo.readyInternalFormat = internalFormat;
  pbo.readyFormat = format;
  pbo.readyBytesPerPixel = bytesPerPixel;
  pbo.ready = true;
  return true;
}

bool InitProgram(QOpenGLShaderProgram& program, const char* vertexShader,
                 const char* fragmentShader) {
  if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader)) {
    qWarning().noquote() << "[WebRTCVideoRenderer] vertex shader compile failed:" << program.log();
    return false;
  }
  if (!program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader)) {
    qWarning().noquote() << "[WebRTCVideoRenderer] fragment shader compile failed:" << program.log();
    return false;
  }
  program.bindAttributeLocation("vertexIn", 0);
  program.bindAttributeLocation("textureIn", 1);
  if (!program.link()) {
    qWarning().noquote() << "[WebRTCVideoRenderer] shader link failed:" << program.log();
    return false;
  }
  return true;
}

}  // namespace

class WebRTCVideoRenderNode : public QSGRenderNode, protected QOpenGLExtraFunctions {
 public:
  WebRTCVideoRenderNode() = default;
  ~WebRTCVideoRenderNode() override { DestroyGlResources(); }

  void Sync(WebRTCVideoRenderer* item) {
    m_videoItem = item;
    m_rect = QRectF(0.0, 0.0, item->width(), item->height());
    UpdateGeometry();
    const int64_t nodeSyncMonoUs = webrtc_demo::DecodeSinkMonotonicUs();

    {
      QMutexLocker locker(&item->m_frameMutex);
      if (item->m_syncFrame.buffer && item->m_syncFrame.generation != 0 &&
          item->m_syncFrame.generation != m_lastConsumedFrameGeneration) {
        ClearHeldFrame();
        m_heldBuffer = item->m_syncFrame.buffer;
        m_queueTraceStartMonoUs = item->m_syncFrame.queueStartMonoUs;
        m_guiUpdateDispatchMonoUs = item->m_syncFrame.guiUpdateDispatchMonoUs;
        m_syncTraceStartMonoUs = item->m_syncFrame.syncStartMonoUs;
        m_nodeSyncTraceMonoUs = nodeSyncMonoUs;
        m_currentFrameId = item->m_syncFrame.frameId;
        m_frameFromTracking = item->m_syncFrame.fromTracking;
        m_haveFrameTrace = true;
        m_lastConsumedFrameGeneration = item->m_syncFrame.generation;
      }
    }

    if (!item->hasVideo()) {
      m_hasData = false;
#if defined(WEBRTC_ANDROID)
      if (m_lastUploadPath == UploadPathKind::Native) {
        ResetNativeImportState(false);
      }
      m_lastNativeImportTrace = {};
      m_nativeCacheHitCount = 0;
      m_nativeCacheCreateCount = 0;
      m_nativeCacheFailCount = 0;
      m_nativeCacheEvictionCount = 0;
      m_nativeSlotUnavailableCount = 0;
#endif
      ClearHeldFrame();
      m_lastConsumedFrameGeneration = 0;
      m_haveFrameTrace = false;
      m_haveDrawFrameTrace = false;
    }
  }

  StateFlags changedStates() const override {
    return DepthState | StencilState | ColorState | BlendState | CullState;
  }

  RenderingFlags flags() const override { return BoundedRectRendering; }

  QRectF rect() const override { return m_rect; }

  void render(const RenderState* state) override {
    if (!EnsureInitialized()) {
      return;
    }

    if (m_heldBuffer) {
      UploadHeldFrame();
    }

    if (!m_hasData) {
      return;
    }

    QElapsedTimer drawTimer;
    drawTimer.start();

    static const GLfloat kTexCoords[] = {
        1.0f, 1.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 0.0f, 0.0f,
    };

    QMatrix4x4 mvp = *state->projectionMatrix();
    if (const QMatrix4x4* nodeMatrix = matrix()) {
      mvp *= *nodeMatrix;
    }

    const bool use_native_texture = m_frameFormat == UploadPathKind::Native &&
                                    m_oesProgram.isLinked() &&
                                    m_currentNativeTextureSlot >= 0;
    QOpenGLShaderProgram* program = nullptr;
    if (use_native_texture) {
      program = &m_oesProgram;
    } else if (m_frameFormat == UploadPathKind::NV12 && m_nv12Program.isLinked()) {
      program = &m_nv12Program;
    } else if (m_i420Program.isLinked()) {
      program = &m_i420Program;
    }
    if (!program) {
      return;
    }

    program->bind();
    program->setUniformValue("qt_Matrix", mvp);
    program->setAttributeArray(0, GL_FLOAT, m_vertices, 2);
    program->enableAttributeArray(0);
    program->setAttributeArray(1, GL_FLOAT, kTexCoords, 2);
    program->enableAttributeArray(1);

    if (use_native_texture) {
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_EXTERNAL_OES,
                    m_nativeTextureSlots[static_cast<size_t>(m_currentNativeTextureSlot)]
                        .texture_id);
      program->setUniformValue("tex", 0);
    } else {
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, m_texY);
      program->setUniformValue("tex_y", 0);
    }

    if (!use_native_texture && m_frameFormat == UploadPathKind::NV12) {
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, m_texUV);
      program->setUniformValue("tex_uv", 1);
    } else if (!use_native_texture) {
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, m_texU);
      glActiveTexture(GL_TEXTURE2);
      glBindTexture(GL_TEXTURE_2D, m_texV);
      program->setUniformValue("tex_u", 1);
      program->setUniformValue("tex_v", 2);
    }

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
#if defined(WEBRTC_ANDROID)
    if (use_native_texture) {
      ArmNativeTextureSlotReuseFence(m_currentNativeTextureSlot);
    }
#endif

    program->disableAttributeArray(0);
    program->disableAttributeArray(1);
    program->release();

    const qint64 drawUs = drawTimer.nsecsElapsed() / 1000;

    if (m_uploadStatsValidForLastFrame && m_haveDrawFrameTrace && m_drawFrameTrace.fromTracking) {
      const uint32_t tid = static_cast<uint32_t>(std::max(0, m_drawFrameTrace.frameId));
      if (webrtc_demo::ShouldLogTrackingTimedSampleById(tid)) {
        const int64_t afterDrawMonoUs = webrtc_demo::DecodeSinkMonotonicUs();
        const qint64 wallFromQueueUs = afterDrawMonoUs - m_drawFrameTrace.queueStartMonoUs;
        const qint64 queueWaitUs =
            m_lastUploadStartMonoUs > 0 ? (m_lastUploadStartMonoUs - m_drawFrameTrace.queueStartMonoUs)
                                        : -1;
        const qint64 postToGuiUs =
            m_drawFrameTrace.guiUpdateDispatchMonoUs > 0
                ? std::max<qint64>(0, m_drawFrameTrace.guiUpdateDispatchMonoUs -
                                                 m_drawFrameTrace.queueStartMonoUs)
                : -1;
        const int64_t syncWaitStartUs =
            m_drawFrameTrace.guiUpdateDispatchMonoUs > 0
                ? std::max(m_drawFrameTrace.queueStartMonoUs,
                           m_drawFrameTrace.guiUpdateDispatchMonoUs)
                : m_drawFrameTrace.queueStartMonoUs;
        const qint64 guiToSyncUs =
            m_drawFrameTrace.syncStartMonoUs > 0
                ? (m_drawFrameTrace.syncStartMonoUs - syncWaitStartUs)
                : -1;
        const qint64 syncToNodeSyncUs =
            (m_drawFrameTrace.nodeSyncMonoUs > 0 && m_drawFrameTrace.syncStartMonoUs > 0)
                ? (m_drawFrameTrace.nodeSyncMonoUs - m_drawFrameTrace.syncStartMonoUs)
                : -1;
        const qint64 nodeSyncToUploadUs =
            (m_lastUploadStartMonoUs > 0 && m_drawFrameTrace.nodeSyncMonoUs > 0)
                ? (m_lastUploadStartMonoUs - m_drawFrameTrace.nodeSyncMonoUs)
                : -1;
        const qint64 syncToRenderUs =
            (m_lastUploadStartMonoUs > 0 && m_drawFrameTrace.syncStartMonoUs > 0)
                ? (m_lastUploadStartMonoUs - m_drawFrameTrace.syncStartMonoUs)
                : -1;
        const qint64 uploadAndDrawUs = m_lastUploadUs + drawUs;
        const auto fmtStageMs = [](qint64 us) {
          return us >= 0 ? QString::number(us / 1000.0, 'f', 3) : QStringLiteral("N/A");
        };

        int64_t decodePipelineStartUs = 0;
        const bool haveDecodeToRender =
            webrtc_demo::TakeDecodePipelineStartMonotonicUs(tid, &decodePipelineStartUs);
        const double decodeToRenderMs =
            haveDecodeToRender ? (afterDrawMonoUs - decodePipelineStartUs) / 1000.0 : -1.0;
        const QString decodeToRenderStr =
            haveDecodeToRender ? QString::number(decodeToRenderMs, 'f', 3) : QStringLiteral("N/A");
        QString nativeCacheEvent = QStringLiteral("N/A");
        QString nativeCreateImageMs = QStringLiteral("N/A");
        QString nativeEvicted = QStringLiteral("N/A");
        QString nativeSlot = QStringLiteral("N/A");
        int nativeCacheEntries = -1;
        int nativeCacheCapacity = -1;
        qulonglong nativeCacheHitCount = 0;
        qulonglong nativeCacheCreateCount = 0;
        qulonglong nativeCacheFailCount = 0;
        qulonglong nativeCacheEvictionCount = 0;
        qulonglong nativeSlotUnavailableCount = 0;
#if defined(WEBRTC_ANDROID)
        nativeCacheEvent =
            QString::fromLatin1(NativeImportCacheEventName(m_lastNativeImportTrace.cache_event));
        if (m_lastNativeImportTrace.egl_create_image_us > 0) {
          nativeCreateImageMs =
              QString::number(m_lastNativeImportTrace.egl_create_image_us / 1000.0, 'f', 3);
        }
        nativeEvicted = m_lastNativeImportTrace.evicted ? QStringLiteral("yes")
                                                        : QStringLiteral("no");
        nativeSlot = m_lastNativeImportTrace.slot_index >= 0
                         ? QString::number(m_lastNativeImportTrace.slot_index)
                         : (m_lastNativeImportTrace.slot_unavailable ? QStringLiteral("miss")
                                                                     : QStringLiteral("N/A"));
        nativeCacheEntries = m_lastNativeImportTrace.cache_entries;
        nativeCacheCapacity = static_cast<int>(kMaxNativeImportedImageCacheEntries);
        nativeCacheHitCount = static_cast<qulonglong>(m_nativeCacheHitCount);
        nativeCacheCreateCount = static_cast<qulonglong>(m_nativeCacheCreateCount);
        nativeCacheFailCount = static_cast<qulonglong>(m_nativeCacheFailCount);
        nativeCacheEvictionCount = static_cast<qulonglong>(m_nativeCacheEvictionCount);
        nativeSlotUnavailableCount = static_cast<qulonglong>(m_nativeSlotUnavailableCount);
#endif

        qDebug().noquote()
            << QString("[Timing/Render] tracking_id=%1 | path=%2 | post_to_gui=%3 ms | "
                       "gui_to_sync=%4 ms | sync_to_node_sync=%5 ms | node_sync_to_upload=%6 ms | "
                       "sync_to_render=%7 ms | queue_wait=%8 ms | fence_wait=%9 ms | import=%10 ms | "
                       "upload=%11 ms | draw=%12 ms | upload+draw=%13 ms | wall(OnFrame->render)=%14 ms | "
                       "total(Decode->render)=%15 ms | cache=%16 | create=%17 ms | evict=%18 | slot=%19 | "
                       "cache_entries=%20/%21 | cache_totals(hit/create/fail/evict/slot_miss)=%22/%23/%24/%25/%26")
                   .arg(m_drawFrameTrace.frameId)
                   .arg(UploadPathKindName(m_frameFormat))
                   .arg(fmtStageMs(postToGuiUs))
                   .arg(fmtStageMs(guiToSyncUs))
                   .arg(fmtStageMs(syncToNodeSyncUs))
                   .arg(fmtStageMs(nodeSyncToUploadUs))
                   .arg(fmtStageMs(syncToRenderUs))
                   .arg(fmtStageMs(queueWaitUs))
                   .arg(m_lastNativeFenceWaitUs / 1000.0, 0, 'f', 3)
                   .arg(m_lastNativeImportUs / 1000.0, 0, 'f', 3)
                   .arg(m_lastUploadUs / 1000.0, 0, 'f', 3)
                   .arg(drawUs / 1000.0, 0, 'f', 3)
                   .arg(uploadAndDrawUs / 1000.0, 0, 'f', 3)
                   .arg(wallFromQueueUs / 1000.0, 0, 'f', 3)
                   .arg(decodeToRenderStr)
                   .arg(nativeCacheEvent)
                   .arg(nativeCreateImageMs)
                   .arg(nativeEvicted)
                   .arg(nativeSlot)
                   .arg(nativeCacheEntries)
                   .arg(nativeCacheCapacity)
                   .arg(nativeCacheHitCount)
                   .arg(nativeCacheCreateCount)
                   .arg(nativeCacheFailCount)
                   .arg(nativeCacheEvictionCount)
                   .arg(nativeSlotUnavailableCount);

        if (m_videoItem) {
          QMetaObject::invokeMethod(
              m_videoItem, "applySampledPipelineUi", Qt::QueuedConnection,
              Q_ARG(int, m_drawFrameTrace.frameId), Q_ARG(double, decodeToRenderMs),
              Q_ARG(double, wallFromQueueUs / 1000.0));
        }
      }
    }

    m_haveFrameTrace = false;
    m_haveDrawFrameTrace = false;
  }

  void releaseResources() override { DestroyGlResources(); }

 private:
  struct FrameTrace {
    int64_t queueStartMonoUs = 0;
    int64_t guiUpdateDispatchMonoUs = 0;
    int64_t syncStartMonoUs = 0;
    int frameId = -1;
    bool fromTracking = false;
    int64_t nodeSyncMonoUs = 0;
  };

  bool EnsureInitialized() {
    if (m_glInitialized) {
      return true;
    }
    if (!QOpenGLContext::currentContext()) {
      return false;
    }

    initializeOpenGLFunctions();
    m_canUseUnpackRowLength = SupportsUnpackRowLength();
    m_canUsePixelUnpackBuffer = SupportsPixelUnpackBuffer();
    m_canUseExternalOesSampling = SupportsExternalOesSampling();
#if defined(WEBRTC_ANDROID)
    m_canUseGpuReuseSync = SupportsGpuReuseSync();
    InitializeAndroidNativeInterop();
#endif
    InitShaders();
    InitTextures();
    InitUploadPbos();
    m_glInitialized = true;
    return true;
  }

#if defined(WEBRTC_ANDROID)
  void InitializeAndroidNativeInterop() {
    if (m_androidInteropInitialized) {
      return;
    }

    m_eglDisplay = eglGetCurrentDisplay();
    if (m_eglDisplay == EGL_NO_DISPLAY) {
      m_androidInteropInitialized = true;
      return;
    }

    const char* egl_exts = eglQueryString(m_eglDisplay, EGL_EXTENSIONS);
    const bool has_image_base = HasExtensionToken(egl_exts, "EGL_KHR_image_base");
    const bool has_native_buffer =
        HasExtensionToken(egl_exts, "EGL_ANDROID_image_native_buffer");
    const bool has_get_native_buffer =
        HasExtensionToken(egl_exts, "EGL_ANDROID_get_native_client_buffer");
    const bool has_gl_egl_image =
        QOpenGLContext::currentContext()->hasExtension(QByteArrayLiteral("GL_OES_EGL_image"));

    if (has_image_base && has_native_buffer && has_get_native_buffer && has_gl_egl_image &&
        m_canUseExternalOesSampling) {
      m_eglCreateImageKHR =
          reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
      m_eglDestroyImageKHR =
          reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
      m_eglGetNativeClientBufferANDROID =
          reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
              eglGetProcAddress("eglGetNativeClientBufferANDROID"));
      m_glEGLImageTargetTexture2DOES =
          reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
              eglGetProcAddress("glEGLImageTargetTexture2DOES"));
      m_canImportHardwareBuffer = m_eglCreateImageKHR && m_eglDestroyImageKHR &&
                                  m_eglGetNativeClientBufferANDROID &&
                                  m_glEGLImageTargetTexture2DOES;
    }

    m_androidInteropInitialized = true;
  }

  void DeleteNativeTextureSlotFence(NativeTextureSlot& slot) {
    if (!slot.reuse_fence) {
      return;
    }
    glDeleteSync(slot.reuse_fence);
    slot.reuse_fence = nullptr;
  }

  bool IsNativeTextureSlotFenceReady(NativeTextureSlot& slot) {
    if (!m_canUseGpuReuseSync || !slot.reuse_fence) {
      return true;
    }

    const GLenum wait_result = glClientWaitSync(slot.reuse_fence, 0, 0);
    if (wait_result == GL_ALREADY_SIGNALED || wait_result == GL_CONDITION_SATISFIED) {
      DeleteNativeTextureSlotFence(slot);
      return true;
    }
    if (wait_result == GL_WAIT_FAILED) {
      DeleteNativeTextureSlotFence(slot);
      static int fence_wait_failed_warn_count = 0;
      if (fence_wait_failed_warn_count < 5) {
        ++fence_wait_failed_warn_count;
        qWarning().noquote()
            << "[WebRTCVideoRenderer] native slot fence poll failed; reusing slot defensively";
      }
      return true;
    }
    return false;
  }

  void ReleaseNativeTextureSlot(NativeTextureSlot& slot) {
    DeleteNativeTextureSlotFence(slot);
    slot.bound_hardware_buffer = nullptr;
    slot.held_buffer = nullptr;
  }

  int FindReusableNativeTextureSlotIndex() {
    for (int i = 0; i < kNativeTextureSlotCount; ++i) {
      const int index = (m_nextNativeTextureSlot + i) % kNativeTextureSlotCount;
      NativeTextureSlot& slot = m_nativeTextureSlots[static_cast<size_t>(index)];
      if (IsNativeTextureSlotFenceReady(slot)) {
        return index;
      }
    }
    return -1;
  }

  void ArmNativeTextureSlotReuseFence(int slot_index) {
    if (!m_canUseGpuReuseSync || slot_index < 0) {
      return;
    }
    NativeTextureSlot& slot = m_nativeTextureSlots[static_cast<size_t>(slot_index)];
    DeleteNativeTextureSlotFence(slot);
    slot.reuse_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  }

  bool IsHardwareBufferBoundToAnySlot(AHardwareBuffer* hardware_buffer) const {
    for (const auto& slot : m_nativeTextureSlots) {
      if (slot.bound_hardware_buffer == hardware_buffer) {
        return true;
      }
    }
    return false;
  }

  void ResetNativeImportState(bool release_slot_buffers = true) {
    if (release_slot_buffers && QOpenGLContext::currentContext()) {
      for (auto& slot : m_nativeTextureSlots) {
        ReleaseNativeTextureSlot(slot);
      }
    }
    m_currentNativeTextureSlot = -1;
    m_nextNativeTextureSlot = 0;
  }

  void ResetNativeImportTrace() { m_lastNativeImportTrace = {}; }

  void TouchImportedImageCacheEntry(NativeImportedImageCache::iterator it) {
    if (it == m_importedImageCache.end()) {
      return;
    }
    m_importedImageCacheLru.splice(m_importedImageCacheLru.end(), m_importedImageCacheLru,
                                   it->second.lru_it);
    it->second.lru_it = std::prev(m_importedImageCacheLru.end());
  }

  void DestroyImportedImageCacheEntry(NativeImportedImageCache::iterator it) {
    if (it == m_importedImageCache.end()) {
      return;
    }
    if (it->second.egl_image != EGL_NO_IMAGE_KHR && m_eglDestroyImageKHR &&
        m_eglDisplay != EGL_NO_DISPLAY) {
      m_eglDestroyImageKHR(m_eglDisplay, it->second.egl_image);
    }
    m_importedImageCacheLru.erase(it->second.lru_it);
    if (it->first) {
      AHardwareBuffer_release(it->first);
    }
    m_importedImageCache.erase(it);
  }

  bool EvictLeastRecentlyUsedImportedImageCacheEntry() {
    for (auto lru_it = m_importedImageCacheLru.begin();
         lru_it != m_importedImageCacheLru.end(); ++lru_it) {
      AHardwareBuffer* const hardware_buffer = *lru_it;
      if (IsHardwareBufferBoundToAnySlot(hardware_buffer)) {
        continue;
      }
      const auto cache_it = m_importedImageCache.find(hardware_buffer);
      if (cache_it != m_importedImageCache.end()) {
        m_lastNativeImportTrace.evicted = true;
        ++m_nativeCacheEvictionCount;
        DestroyImportedImageCacheEntry(cache_it);
        return true;
      }
    }
    return false;
  }

  void DestroyImportedImageCache() {
    while (!m_importedImageCache.empty()) {
      DestroyImportedImageCacheEntry(m_importedImageCache.begin());
    }
    m_importedImageCacheLru.clear();
  }

  NativeImportedImageCacheEntry* FindOrCreateImportedImageCacheEntry(
      AHardwareBuffer* hardware_buffer) {
    if (!hardware_buffer || !m_eglCreateImageKHR || !m_eglGetNativeClientBufferANDROID ||
        m_eglDisplay == EGL_NO_DISPLAY) {
      return nullptr;
    }

    const auto existing = m_importedImageCache.find(hardware_buffer);
    if (existing != m_importedImageCache.end()) {
      TouchImportedImageCacheEntry(existing);
      m_lastNativeImportTrace.cache_event = NativeImportCacheEvent::Hit;
      m_lastNativeImportTrace.cache_entries = static_cast<int>(m_importedImageCache.size());
      ++m_nativeCacheHitCount;
      return &existing->second;
    }

    EGLClientBuffer native_client_buffer = m_eglGetNativeClientBufferANDROID(hardware_buffer);
    if (!native_client_buffer) {
      m_lastNativeImportTrace.cache_event = NativeImportCacheEvent::MissNoClientBuffer;
      m_lastNativeImportTrace.cache_entries = static_cast<int>(m_importedImageCache.size());
      ++m_nativeCacheFailCount;
      return nullptr;
    }

    if (m_importedImageCache.size() >= kMaxNativeImportedImageCacheEntries &&
        !EvictLeastRecentlyUsedImportedImageCacheEntry()) {
      m_lastNativeImportTrace.cache_event = NativeImportCacheEvent::MissNoSpace;
      m_lastNativeImportTrace.cache_entries = static_cast<int>(m_importedImageCache.size());
      ++m_nativeCacheFailCount;
      return nullptr;
    }

    const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_FALSE, EGL_NONE};
    QElapsedTimer createTimer;
    createTimer.start();
    EGLImageKHR egl_image = m_eglCreateImageKHR(m_eglDisplay, EGL_NO_CONTEXT,
                                                EGL_NATIVE_BUFFER_ANDROID,
                                                native_client_buffer, attrs);
    m_lastNativeImportTrace.egl_create_image_us = createTimer.nsecsElapsed() / 1000;
    if (egl_image == EGL_NO_IMAGE_KHR) {
      m_lastNativeImportTrace.cache_event = NativeImportCacheEvent::MissCreateFailed;
      m_lastNativeImportTrace.cache_entries = static_cast<int>(m_importedImageCache.size());
      ++m_nativeCacheFailCount;
      return nullptr;
    }

    AHardwareBuffer_acquire(hardware_buffer);
    m_importedImageCacheLru.push_back(hardware_buffer);
    NativeImportedImageCacheEntry entry;
    entry.egl_image = egl_image;
    entry.lru_it = std::prev(m_importedImageCacheLru.end());
    const auto insert_result = m_importedImageCache.emplace(hardware_buffer, std::move(entry));
    if (!insert_result.second) {
      m_importedImageCacheLru.pop_back();
      if (m_eglDestroyImageKHR && m_eglDisplay != EGL_NO_DISPLAY) {
        m_eglDestroyImageKHR(m_eglDisplay, egl_image);
      }
      AHardwareBuffer_release(hardware_buffer);
      m_lastNativeImportTrace.cache_event = NativeImportCacheEvent::MissCreateFailed;
      m_lastNativeImportTrace.cache_entries = static_cast<int>(m_importedImageCache.size());
      ++m_nativeCacheFailCount;
      return nullptr;
    }
    m_lastNativeImportTrace.cache_event = NativeImportCacheEvent::MissCreated;
    m_lastNativeImportTrace.cache_entries = static_cast<int>(m_importedImageCache.size());
    ++m_nativeCacheCreateCount;
    return &insert_result.first->second;
  }
#endif

  void InitShaders() {
    static const char* kVertexShader =
        "uniform mat4 qt_Matrix;"
        "attribute vec4 vertexIn;"
        "attribute vec2 textureIn;"
        "varying vec2 textureOut;"
        "void main() {"
        "  gl_Position = qt_Matrix * vertexIn;"
        "  textureOut = textureIn;"
        "}";

    static const char* kI420FragmentShader =
        "varying mediump vec2 textureOut;"
        "uniform sampler2D tex_y;"
        "uniform sampler2D tex_u;"
        "uniform sampler2D tex_v;"
        "void main() {"
        "  mediump vec3 yuv;"
        "  yuv.x = texture2D(tex_y, textureOut).r;"
        "  yuv.y = texture2D(tex_u, textureOut).r - 0.5;"
        "  yuv.z = texture2D(tex_v, textureOut).r - 0.5;"
        "  mediump vec3 rgb = mat3(1.0, 1.0, 1.0,"
        "                          0.0, -0.34413, 1.772,"
        "                          1.402, -0.71414, 0.0) * yuv;"
        "  gl_FragColor = vec4(rgb, 1.0);"
        "}";

    static const char* kNv12FragmentShader =
        "varying mediump vec2 textureOut;"
        "uniform sampler2D tex_y;"
        "uniform sampler2D tex_uv;"
        "void main() {"
        "  mediump vec3 yuv;"
        "  yuv.x = texture2D(tex_y, textureOut).r;"
        "  mediump vec2 uv = texture2D(tex_uv, textureOut).ra - vec2(0.5, 0.5);"
        "  yuv.y = uv.x;"
        "  yuv.z = uv.y;"
        "  mediump vec3 rgb = mat3(1.0, 1.0, 1.0,"
        "                          0.0, -0.34413, 1.772,"
        "                          1.402, -0.71414, 0.0) * yuv;"
        "  gl_FragColor = vec4(rgb, 1.0);"
        "}";

    static const char* kOesFragmentShader =
        "#extension GL_OES_EGL_image_external : require\n"
        "precision mediump float;\n"
        "varying mediump vec2 textureOut;\n"
        "uniform samplerExternalOES tex;\n"
        "void main() {\n"
        "  gl_FragColor = texture2D(tex, textureOut);\n"
        "}\n";

    InitProgram(m_i420Program, kVertexShader, kI420FragmentShader);
    InitProgram(m_nv12Program, kVertexShader, kNv12FragmentShader);
    if (m_canUseExternalOesSampling) {
      InitProgram(m_oesProgram, kVertexShader, kOesFragmentShader);
    }
  }

  void InitTextures() {
    GLuint texIds[4] = {0, 0, 0, 0};
    glGenTextures(4, texIds);
    m_texY = texIds[0];
    m_texU = texIds[1];
    m_texV = texIds[2];
    m_texUV = texIds[3];

    for (GLuint texId : texIds) {
      glBindTexture(GL_TEXTURE_2D, texId);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

#if defined(WEBRTC_ANDROID)
    if (m_canUseExternalOesSampling && m_nativeTextureSlots[0].texture_id == 0) {
      GLuint nativeTexIds[kNativeTextureSlotCount] = {};
      glGenTextures(kNativeTextureSlotCount, nativeTexIds);
      for (int i = 0; i < kNativeTextureSlotCount; ++i) {
        m_nativeTextureSlots[static_cast<size_t>(i)].texture_id = nativeTexIds[i];
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, nativeTexIds[i]);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      }
    }
#endif
  }

  void DestroyGlResources() {
    ClearHeldFrame();
    if (!m_glInitialized || !QOpenGLContext::currentContext()) {
      return;
    }

    DestroyUploadPbos();
    GLuint texIds[4] = {m_texY, m_texU, m_texV, m_texUV};
    glDeleteTextures(4, texIds);
    m_texY = 0;
    m_texU = 0;
    m_texV = 0;
    m_texUV = 0;
#if defined(WEBRTC_ANDROID)
    ResetNativeImportState(true);
    for (auto& slot : m_nativeTextureSlots) {
      if (slot.texture_id != 0) {
        glDeleteTextures(1, &slot.texture_id);
        slot.texture_id = 0;
      }
    }
    DestroyImportedImageCache();
#endif
    m_glInitialized = false;
  }

  void ClearHeldFrame() { m_heldBuffer = nullptr; }

  void InitUploadPbos() {
    if (!m_canUsePixelUnpackBuffer) {
      return;
    }
    glGenBuffers(2, m_pboY.ids);
    glGenBuffers(2, m_pboU.ids);
    glGenBuffers(2, m_pboV.ids);
    glGenBuffers(2, m_pboUV.ids);
  }

  void DestroyUploadPbos() {
    if (!m_canUsePixelUnpackBuffer) {
      return;
    }
    glDeleteBuffers(2, m_pboY.ids);
    glDeleteBuffers(2, m_pboU.ids);
    glDeleteBuffers(2, m_pboV.ids);
    glDeleteBuffers(2, m_pboUV.ids);
    m_pboY = {};
    m_pboU = {};
    m_pboV = {};
    m_pboUV = {};
  }

  void ResetCpuUploadPipeline() {
    ResetPlaneUploadPbo(m_pboY);
    ResetPlaneUploadPbo(m_pboU);
    ResetPlaneUploadPbo(m_pboV);
    ResetPlaneUploadPbo(m_pboUV);
    m_cacheY = {};
    m_cacheU = {};
    m_cacheV = {};
    m_cacheUV = {};
    m_hasData = false;
    m_readyFrameTrace = {};
    m_haveReadyFrameTrace = false;
  }

  void UpdateGeometry() {
    const GLfloat w = static_cast<GLfloat>(m_rect.width());
    const GLfloat h = static_cast<GLfloat>(m_rect.height());
    const GLfloat verts[8] = {
        0.0f, 0.0f, w, 0.0f,
        0.0f, h, w, h,
    };
    std::copy(std::begin(verts), std::end(verts), m_vertices);
  }

  PlanePipelineResult UploadPlaneViaPboAsync(GLenum textureUnit, GLuint textureId, int planeWidth,
                                             int planeHeight, GLenum internalFormat, GLenum format,
                                             int bytesPerPixel, const uint8_t* planeData,
                                             int strideBytes, PlaneTextureCache& cache,
                                             PlaneUploadPbo& pbo,
                                             std::vector<uint8_t>& tightBuffer) {
    if (!m_canUsePixelUnpackBuffer || (!pbo.ids[0] && !pbo.ids[1])) {
      UploadPlane(*this, textureUnit, textureId, planeWidth, planeHeight, internalFormat, format,
                  bytesPerPixel, planeData, strideBytes, m_canUseUnpackRowLength, cache,
                  tightBuffer);
      return {true, false};
    }

    PlanePipelineResult result;
    result.uploaded =
        UploadPlaneFromReadyPbo(*this, textureUnit, textureId, m_canUseUnpackRowLength, cache, pbo);
    result.staged = StagePlaneIntoPbo(*this, planeWidth, planeHeight, internalFormat, format,
                                      bytesPerPixel, planeData, strideBytes,
                                      m_canUseUnpackRowLength, pbo);
    if (!result.staged) {
      ResetPlaneUploadPbo(pbo);
      UploadPlane(*this, textureUnit, textureId, planeWidth, planeHeight, internalFormat, format,
                  bytesPerPixel, planeData, strideBytes, m_canUseUnpackRowLength, cache,
                  tightBuffer);
      return {true, false};
    }
    return result;
  }

  PlanePipelineResult UploadI420(const webrtc::I420BufferInterface* i420) {
    if (!i420) {
      return {};
    }

    const PlanePipelineResult y =
        UploadPlaneViaPboAsync(GL_TEXTURE0, m_texY, i420->width(), i420->height(), GL_LUMINANCE,
                               GL_LUMINANCE, 1, i420->DataY(), i420->StrideY(), m_cacheY, m_pboY,
                               m_tightY);
    const PlanePipelineResult u = UploadPlaneViaPboAsync(
        GL_TEXTURE1, m_texU, i420->ChromaWidth(), i420->ChromaHeight(), GL_LUMINANCE,
        GL_LUMINANCE, 1, i420->DataU(), i420->StrideU(), m_cacheU, m_pboU, m_tightU);
    const PlanePipelineResult v = UploadPlaneViaPboAsync(
        GL_TEXTURE2, m_texV, i420->ChromaWidth(), i420->ChromaHeight(), GL_LUMINANCE,
        GL_LUMINANCE, 1, i420->DataV(), i420->StrideV(), m_cacheV, m_pboV, m_tightV);
    return {y.uploaded && u.uploaded && v.uploaded, y.staged && u.staged && v.staged};
  }

  PlanePipelineResult UploadNV12(const webrtc::NV12BufferInterface* nv12) {
    if (!nv12) {
      return {};
    }

    const PlanePipelineResult y =
        UploadPlaneViaPboAsync(GL_TEXTURE0, m_texY, nv12->width(), nv12->height(), GL_LUMINANCE,
                               GL_LUMINANCE, 1, nv12->DataY(), nv12->StrideY(), m_cacheY, m_pboY,
                               m_tightY);
    const PlanePipelineResult uv = UploadPlaneViaPboAsync(
        GL_TEXTURE1, m_texUV, nv12->ChromaWidth(), nv12->ChromaHeight(), GL_LUMINANCE_ALPHA,
        GL_LUMINANCE_ALPHA, 2, nv12->DataUV(), nv12->StrideUV(), m_cacheUV, m_pboUV, m_tightUV);
    return {y.uploaded && uv.uploaded, y.staged && uv.staged};
  }

#if defined(WEBRTC_ANDROID)
  bool ImportHardwareBuffer(const webrtc_demo::AndroidHardwareBufferVideoFrameBuffer* buffer) {
    if (!buffer || !m_canImportHardwareBuffer || m_nativeTextureSlots[0].texture_id == 0 ||
        m_eglDisplay == EGL_NO_DISPLAY) {
      return false;
    }

    m_lastNativeFenceWaitUs = 0;
    m_lastNativeImportUs = 0;
    ResetNativeImportTrace();

    const int sync_fence_fd = buffer->ConsumeSyncFenceFd();
    QElapsedTimer fenceTimer;
    fenceTimer.start();
    const NativeFenceWaitStatus fence_status =
        WaitForNativeFenceFd(sync_fence_fd, kNativeFenceWaitTimeoutMs);
    m_lastNativeFenceWaitUs = fenceTimer.nsecsElapsed() / 1000;
    if (fence_status == NativeFenceWaitStatus::Pending) {
      return false;
    }
    if (fence_status == NativeFenceWaitStatus::Failed) {
      static int fence_wait_warn_count = 0;
      if (fence_wait_warn_count < 5) {
        ++fence_wait_warn_count;
        qWarning().noquote()
            << "[WebRTCVideoRenderer] native buffer fence wait failed; dropping frame";
      }
      return false;
    }

    AHardwareBuffer* hardware_buffer = buffer->hardware_buffer();
    if (!hardware_buffer) {
      return false;
    }

    QElapsedTimer importTimer;
    importTimer.start();
    NativeImportedImageCacheEntry* const entry =
        FindOrCreateImportedImageCacheEntry(hardware_buffer);
    if (!entry) {
      static int cache_import_warn_count = 0;
      if (cache_import_warn_count < 8) {
        ++cache_import_warn_count;
        qWarning().noquote()
            << QString("[WebRTCVideoRenderer] native import cache miss unresolved: event=%1 "
                       "cache_entries=%2/%3 evicted=%4 create=%5 ms")
                   .arg(NativeImportCacheEventName(m_lastNativeImportTrace.cache_event))
                   .arg(m_lastNativeImportTrace.cache_entries)
                   .arg(static_cast<int>(kMaxNativeImportedImageCacheEntries))
                   .arg(m_lastNativeImportTrace.evicted ? QStringLiteral("yes")
                                                        : QStringLiteral("no"))
                   .arg(m_lastNativeImportTrace.egl_create_image_us / 1000.0, 0, 'f', 3);
      }
      return false;
    }

    const int slot_index = FindReusableNativeTextureSlotIndex();
    if (slot_index < 0) {
      m_lastNativeImportTrace.slot_unavailable = true;
      m_lastNativeImportTrace.cache_entries = static_cast<int>(m_importedImageCache.size());
      ++m_nativeSlotUnavailableCount;
      static int slot_unavailable_warn_count = 0;
      if (slot_unavailable_warn_count < 8) {
        ++slot_unavailable_warn_count;
        qWarning().noquote()
            << QString("[WebRTCVideoRenderer] all native texture slots busy; dropping frame "
                       "(cache_entries=%1/%2)")
                   .arg(m_lastNativeImportTrace.cache_entries)
                   .arg(static_cast<int>(kMaxNativeImportedImageCacheEntries));
      }
      return false;
    }

    NativeTextureSlot& slot = m_nativeTextureSlots[static_cast<size_t>(slot_index)];
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, slot.texture_id);
    if (slot.bound_hardware_buffer != hardware_buffer) {
      m_glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES,
                                     reinterpret_cast<GLeglImageOES>(entry->egl_image));
      slot.bound_hardware_buffer = hardware_buffer;
    }
    slot.held_buffer = m_heldBuffer;
    m_lastNativeImportTrace.slot_index = slot_index;
    m_lastNativeImportTrace.cache_entries = static_cast<int>(m_importedImageCache.size());
    m_currentNativeTextureSlot = slot_index;
    m_nextNativeTextureSlot = (slot_index + 1) % kNativeTextureSlotCount;
    m_lastNativeImportUs = importTimer.nsecsElapsed() / 1000;
    return true;
  }
#endif

  void UploadHeldFrame() {
    if (!m_heldBuffer) {
      return;
    }

    QElapsedTimer uploadTimer;
    uploadTimer.start();
    m_lastUploadStartMonoUs = webrtc_demo::DecodeSinkMonotonicUs();
#if defined(WEBRTC_ANDROID)
    m_lastNativeFenceWaitUs = 0;
    m_lastNativeImportUs = 0;
#endif

    PlanePipelineResult upload;
    const auto type = m_heldBuffer->type();
    const UploadPathKind nextPath =
        type == webrtc::VideoFrameBuffer::Type::kNative
            ? UploadPathKind::Native
            : (type == webrtc::VideoFrameBuffer::Type::kNV12 ? UploadPathKind::NV12
                                                             : UploadPathKind::I420);
    if (nextPath != m_lastUploadPath) {
#if defined(WEBRTC_ANDROID)
      if (m_lastUploadPath == UploadPathKind::Native && nextPath != UploadPathKind::Native) {
        ResetNativeImportState(true);
      }
#endif
      if (nextPath != UploadPathKind::Native) {
        ResetCpuUploadPipeline();
      }
      qDebug() << "[VideoRenderer] upload path ->"
               << (nextPath == UploadPathKind::Native
                       ? "AHardwareBuffer"
                       : (nextPath == UploadPathKind::NV12 ? "NV12" : "I420"));
    }

#if defined(WEBRTC_ANDROID)
    if (type == webrtc::VideoFrameBuffer::Type::kNative) {
      const auto* native_buffer =
          webrtc_demo::AndroidHardwareBufferVideoFrameBuffer::TryGet(m_heldBuffer);
      if (ImportHardwareBuffer(native_buffer)) {
        m_frameFormat = UploadPathKind::Native;
        m_lastUploadPath = UploadPathKind::Native;
        m_lastUploadUs = uploadTimer.nsecsElapsed() / 1000;
        m_uploadStatsValidForLastFrame = true;
        m_hasData = true;
        m_readyFrameTrace = {};
        m_haveReadyFrameTrace = false;
        m_drawFrameTrace = {m_queueTraceStartMonoUs, m_guiUpdateDispatchMonoUs,
                            m_syncTraceStartMonoUs, m_currentFrameId, m_frameFromTracking,
                            m_nodeSyncTraceMonoUs};
        m_haveDrawFrameTrace = m_haveFrameTrace;
      } else {
        m_uploadStatsValidForLastFrame = false;
        m_haveDrawFrameTrace = false;
      }
      ClearHeldFrame();
      return;
    }
#endif

    if (type == webrtc::VideoFrameBuffer::Type::kNV12) {
      upload = UploadNV12(m_heldBuffer->GetNV12());
      if (upload.uploaded || upload.staged) {
        m_frameFormat = UploadPathKind::NV12;
        m_lastUploadPath = UploadPathKind::NV12;
      }
    } else {
      const webrtc::I420BufferInterface* i420 = m_heldBuffer->GetI420();
      webrtc::scoped_refptr<webrtc::I420BufferInterface> converted;
      if (!i420) {
        converted = m_heldBuffer->ToI420();
        i420 = converted.get();
      }
      upload = UploadI420(i420);
      if (upload.uploaded || upload.staged) {
        m_frameFormat = UploadPathKind::I420;
        m_lastUploadPath = UploadPathKind::I420;
      }
    }

    m_lastUploadUs = uploadTimer.nsecsElapsed() / 1000;
    m_uploadStatsValidForLastFrame = upload.uploaded;
    if (upload.uploaded) {
      m_hasData = true;
      if (m_haveReadyFrameTrace) {
        m_drawFrameTrace = m_readyFrameTrace;
        m_haveDrawFrameTrace = true;
      } else {
        m_drawFrameTrace = {m_queueTraceStartMonoUs, m_guiUpdateDispatchMonoUs,
                            m_syncTraceStartMonoUs, m_currentFrameId, m_frameFromTracking,
                            m_nodeSyncTraceMonoUs};
        m_haveDrawFrameTrace = m_haveFrameTrace;
      }
    } else {
      m_haveDrawFrameTrace = false;
    }
    if (upload.staged) {
      m_readyFrameTrace = {m_queueTraceStartMonoUs, m_guiUpdateDispatchMonoUs,
                           m_syncTraceStartMonoUs, m_currentFrameId, m_frameFromTracking,
                           m_nodeSyncTraceMonoUs};
      m_haveReadyFrameTrace = m_haveFrameTrace;
    }
    ClearHeldFrame();
  }

  WebRTCVideoRenderer* m_videoItem = nullptr;
  QRectF m_rect;
  GLfloat m_vertices[8] = {0};

  bool m_glInitialized = false;
  bool m_canUseUnpackRowLength = false;
  bool m_canUsePixelUnpackBuffer = false;
  bool m_canUseExternalOesSampling = false;
  bool m_hasData = false;
  UploadPathKind m_frameFormat = UploadPathKind::I420;
  UploadPathKind m_lastUploadPath = UploadPathKind::None;

  QOpenGLShaderProgram m_i420Program;
  QOpenGLShaderProgram m_nv12Program;
  QOpenGLShaderProgram m_oesProgram;
  GLuint m_texY = 0;
  GLuint m_texU = 0;
  GLuint m_texV = 0;
  GLuint m_texUV = 0;
#if defined(WEBRTC_ANDROID)
  bool m_androidInteropInitialized = false;
  bool m_canImportHardwareBuffer = false;
  bool m_canUseGpuReuseSync = false;
  EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
  PFNEGLCREATEIMAGEKHRPROC m_eglCreateImageKHR = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC m_eglDestroyImageKHR = nullptr;
  PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC m_eglGetNativeClientBufferANDROID = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_glEGLImageTargetTexture2DOES = nullptr;
  std::array<NativeTextureSlot, kNativeTextureSlotCount> m_nativeTextureSlots;
  NativeImportedImageCache m_importedImageCache;
  std::list<AHardwareBuffer*> m_importedImageCacheLru;
  int m_currentNativeTextureSlot = -1;
  int m_nextNativeTextureSlot = 0;
#endif

  PlaneTextureCache m_cacheY;
  PlaneTextureCache m_cacheU;
  PlaneTextureCache m_cacheV;
  PlaneTextureCache m_cacheUV;
  PlaneUploadPbo m_pboY;
  PlaneUploadPbo m_pboU;
  PlaneUploadPbo m_pboV;
  PlaneUploadPbo m_pboUV;
  std::vector<uint8_t> m_tightY;
  std::vector<uint8_t> m_tightU;
  std::vector<uint8_t> m_tightV;
  std::vector<uint8_t> m_tightUV;

  webrtc::scoped_refptr<webrtc::VideoFrameBuffer> m_heldBuffer;
  uint64_t m_lastConsumedFrameGeneration = 0;
  int64_t m_queueTraceStartMonoUs = 0;
  int64_t m_guiUpdateDispatchMonoUs = 0;
  int64_t m_syncTraceStartMonoUs = 0;
  int64_t m_nodeSyncTraceMonoUs = 0;
  int m_currentFrameId = -1;
  bool m_frameFromTracking = false;
  bool m_haveFrameTrace = false;
  FrameTrace m_readyFrameTrace;
  bool m_haveReadyFrameTrace = false;
  FrameTrace m_drawFrameTrace;
  bool m_haveDrawFrameTrace = false;

  int64_t m_lastUploadStartMonoUs = 0;
  qint64 m_lastUploadUs = 0;
  bool m_uploadStatsValidForLastFrame = false;
  qint64 m_lastNativeFenceWaitUs = 0;
  qint64 m_lastNativeImportUs = 0;
#if defined(WEBRTC_ANDROID)
  NativeImportTrace m_lastNativeImportTrace;
  uint64_t m_nativeCacheHitCount = 0;
  uint64_t m_nativeCacheCreateCount = 0;
  uint64_t m_nativeCacheFailCount = 0;
  uint64_t m_nativeCacheEvictionCount = 0;
  uint64_t m_nativeSlotUnavailableCount = 0;
#endif
};

WebRTCVideoRenderer::WebRTCVideoRenderer(QQuickItem* parent) : QQuickItem(parent) {
  setFlag(ItemHasContents, true);
  connect(this, &QQuickItem::windowChanged, this, &WebRTCVideoRenderer::OnWindowChanged);
  startTimer(kEncodedIngressPollIntervalMs);
}

WebRTCVideoRenderer::~WebRTCVideoRenderer() {
  clearVideoTrack();
}

void WebRTCVideoRenderer::timerEvent(QTimerEvent*) {
  const int32_t raw = webrtc_demo::GetLastEncodedFrameTrackingIdForUi();
  const int cur = raw >= 0 ? static_cast<int>(raw) : -1;
  if (cur != m_lastPolledEncodedIngressId) {
    m_lastPolledEncodedIngressId = cur;
    Q_EMIT encodedIngressTrackingChanged();
  }
}

void WebRTCVideoRenderer::OnFrame(const webrtc::VideoFrame& frame) {
  QElapsedTimer onFrameTotalTimer;
  onFrameTotalTimer.start();

  const int64_t onFrameMonoUs = webrtc_demo::DecodeSinkMonotonicUs();
  const uint32_t rtpTs = frame.rtp_timestamp();
  int64_t afterDecodedUs = 0;
  bool haveDecodedToOnFrame = false;
  double decodedToOnFrameMs = -1.0;
  if (webrtc_demo::DecodeSinkTakeDecodedReturn(rtpTs, &afterDecodedUs)) {
    const int64_t decodeToSinkUs = onFrameMonoUs - afterDecodedUs;
    haveDecodedToOnFrame = true;
    decodedToOnFrameMs = decodeToSinkUs / 1000.0;
  }

  static int onFrameCalls = 0;
  ++onFrameCalls;
  if (onFrameCalls == 1 || (onFrameCalls % 30) == 0) {
    qDebug() << "[VideoRenderer] OnFrame#" << onFrameCalls << "rtp_ts=" << rtpTs;
  }

  webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer = frame.video_frame_buffer();
  if (!buffer) {
    return;
  }

  bool is_native_ahb = false;
#if defined(WEBRTC_ANDROID)
  is_native_ahb = webrtc_demo::AndroidHardwareBufferVideoFrameBuffer::TryGet(buffer) != nullptr;
#endif

  if (buffer->type() != webrtc::VideoFrameBuffer::Type::kI420 &&
      buffer->type() != webrtc::VideoFrameBuffer::Type::kNV12 && !is_native_ahb) {
    buffer = buffer->ToI420();
    if (!buffer) {
      return;
    }
  }

  const int w = buffer->width();
  const int h = buffer->height();
  if (w <= 0 || h <= 0) {
    return;
  }

  const qint64 intervalUs =
      m_decodeIntervalTimer.isValid() ? m_decodeIntervalTimer.nsecsElapsed() / 1000 : 0;
  m_decodeIntervalTimer.start();

  const uint16_t rawId = frame.id();
  bool idChanged = false;
  bool shouldEnsureRenderPump = false;
  bool shouldQueueRenderUpdate = false;
  bool fromTrackingForLog = false;
  int displayIdForLog = -1;
  qint64 handoffUs = 0;
  const auto originalType = frame.video_frame_buffer()->type();

  QElapsedTimer handoffTimer;
  handoffTimer.start();
  {
    QMutexLocker locker(&m_frameMutex);
    const bool fromTracking = rawId != webrtc::VideoFrame::kNotSetId;
    const int displayId =
        fromTracking ? static_cast<int>(rawId) : static_cast<int>(++m_localPreviewSeq);

    fromTrackingForLog = fromTracking;
    displayIdForLog = displayId;

    m_latestFrame.buffer = std::move(buffer);
    m_latestFrame.queueStartMonoUs = onFrameMonoUs;
    m_latestFrame.guiUpdateDispatchMonoUs = 0;
    m_latestFrame.frameId = displayId;
    m_latestFrame.fromTracking = fromTracking;
    ++m_latestFrame.generation;
    if (m_highlightFrameId != displayId || m_frameIdFromTracking != fromTracking) {
      m_highlightFrameId = displayId;
      m_frameIdFromTracking = fromTracking;
      if (m_lastHighlightSignalMonoUs == 0 ||
          (onFrameMonoUs - m_lastHighlightSignalMonoUs) >= kHighlightSignalThrottleUs) {
        idChanged = true;
        m_lastHighlightSignalMonoUs = onFrameMonoUs;
      }
    }
    handoffUs = handoffTimer.nsecsElapsed() / 1000;
  }

  const bool renderPumpRunning = m_renderPumpRunning.load(std::memory_order_acquire);
  if (!renderPumpRunning &&
      !m_renderPumpStartPending.exchange(true, std::memory_order_acq_rel)) {
    shouldEnsureRenderPump = true;
  }
  shouldQueueRenderUpdate = renderPumpRunning;

  if (idChanged) {
    QMetaObject::invokeMethod(this, [this]() { Q_EMIT highlightFrameIdChanged(); },
                              Qt::QueuedConnection);
  }

  if (!m_hasVideo.exchange(true, std::memory_order_acq_rel)) {
    QMetaObject::invokeMethod(this, [this]() { Q_EMIT hasVideoChanged(); }, Qt::QueuedConnection);
  }

  if (shouldEnsureRenderPump) {
    QMetaObject::invokeMethod(this, [this]() { EnsureRenderPumpRunning(); },
                              Qt::QueuedConnection);
  }
  if (shouldQueueRenderUpdate) {
    QueueRenderPumpUpdate();
  }

  ++m_frameCount;

  const qint64 onFrameTotalUs = onFrameTotalTimer.nsecsElapsed() / 1000;
  if (fromTrackingForLog &&
      webrtc_demo::ShouldLogTrackingTimedSampleById(static_cast<uint32_t>(displayIdForLog))) {
    qDebug().noquote()
        << QString("[Timing/OnFrame] tracking_id=%1 | rtp_ts=%2 | decoded_to_onframe=%3 ms | "
                   "handoff=%4 ms | frame_interval=%5 ms | buffer=%6 | bytes=%7 KB | total=%8 ms")
               .arg(displayIdForLog)
               .arg(rtpTs)
               .arg(haveDecodedToOnFrame ? QString::number(decodedToOnFrameMs, 'f', 3)
                                         : QStringLiteral("N/A"))
               .arg(handoffUs / 1000.0, 0, 'f', 3)
               .arg(intervalUs / 1000.0, 0, 'f', 2)
               .arg(webrtc::VideoFrameBufferTypeToString(originalType))
               .arg((w * h * 3 / 2) / 1024)
               .arg(onFrameTotalUs / 1000.0, 0, 'f', 3);
  }
}

int WebRTCVideoRenderer::highlightFrameId() const {
  QMutexLocker locker(&m_frameMutex);
  return m_highlightFrameId;
}

bool WebRTCVideoRenderer::frameIdFromTracking() const {
  QMutexLocker locker(&m_frameMutex);
  return m_frameIdFromTracking;
}

int WebRTCVideoRenderer::encodedIngressTrackingId() const {
  const int32_t v = webrtc_demo::GetLastEncodedFrameTrackingIdForUi();
  return v >= 0 ? static_cast<int>(v) : -1;
}

bool WebRTCVideoRenderer::hasEncodedIngressTracking() const {
  return webrtc_demo::GetLastEncodedFrameTrackingIdForUi() >= 0;
}

void WebRTCVideoRenderer::RequestRenderPumpUpdate() {
  const int64_t dispatchMonoUs = webrtc_demo::DecodeSinkMonotonicUs();
  {
    QMutexLocker locker(&m_frameMutex);
    if (m_latestFrame.buffer && m_latestFrame.generation != 0 &&
        m_latestFrame.generation != m_syncFrame.generation &&
        m_latestFrame.guiUpdateDispatchMonoUs == 0) {
      m_latestFrame.guiUpdateDispatchMonoUs = dispatchMonoUs;
    }
  }
  update();
}

void WebRTCVideoRenderer::QueueRenderPumpUpdate() {
  if (m_renderUpdatePending.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  if (QThread::currentThread() == thread()) {
    RequestRenderPumpUpdate();
    return;
  }
  QMetaObject::invokeMethod(this, [this]() { RequestRenderPumpUpdate(); }, Qt::QueuedConnection);
}

void WebRTCVideoRenderer::EnsureRenderPumpRunning() {
  m_renderPumpStartPending.store(false, std::memory_order_release);
  if (!hasVideo()) {
    return;
  }
  m_renderPumpRunning.store(true, std::memory_order_release);
  QueueRenderPumpUpdate();
}

void WebRTCVideoRenderer::StopRenderPump() {
  m_renderPumpRunning.store(false, std::memory_order_release);
  m_renderPumpStartPending.store(false, std::memory_order_release);
  m_renderUpdatePending.store(false, std::memory_order_release);
}

void WebRTCVideoRenderer::OnWindowChanged(QQuickWindow* currentWindow) {
  QObject::disconnect(m_beforeSynchronizingConnection);
  m_beforeSynchronizingConnection = {};
  QObject::disconnect(m_frameSwappedConnection);
  m_frameSwappedConnection = {};
  if (!currentWindow) {
    return;
  }

  currentWindow->setPersistentGraphics(true);
  currentWindow->setPersistentSceneGraph(true);

  m_beforeSynchronizingConnection = connect(currentWindow, &QQuickWindow::beforeSynchronizing, this,
                                            &WebRTCVideoRenderer::OnBeforeSynchronizing,
                                            Qt::DirectConnection);
  m_frameSwappedConnection =
      connect(currentWindow, &QQuickWindow::frameSwapped, this, &WebRTCVideoRenderer::OnFrameSwapped,
              Qt::QueuedConnection);
  if (m_renderPumpRunning.load(std::memory_order_acquire)) {
    QueueRenderPumpUpdate();
  }
}

void WebRTCVideoRenderer::OnBeforeSynchronizing() {
  if (!hasVideo()) {
    return;
  }

  const int64_t syncStartMonoUs = webrtc_demo::DecodeSinkMonotonicUs();
  QMutexLocker locker(&m_frameMutex);
  if (!m_latestFrame.buffer || m_latestFrame.generation == 0 ||
      m_latestFrame.generation == m_syncFrame.generation) {
    return;
  }

  m_syncFrame.buffer = m_latestFrame.buffer;
  m_syncFrame.queueStartMonoUs = m_latestFrame.queueStartMonoUs;
  m_syncFrame.guiUpdateDispatchMonoUs = m_latestFrame.guiUpdateDispatchMonoUs;
  m_syncFrame.syncStartMonoUs = syncStartMonoUs;
  m_syncFrame.frameId = m_latestFrame.frameId;
  m_syncFrame.fromTracking = m_latestFrame.fromTracking;
  m_syncFrame.generation = m_latestFrame.generation;
}

bool WebRTCVideoRenderer::HasUndeliveredFrame() const {
  QMutexLocker locker(&m_frameMutex);
  return m_latestFrame.buffer && m_latestFrame.generation != 0 &&
         m_latestFrame.generation != m_syncFrame.generation;
}

void WebRTCVideoRenderer::OnFrameSwapped() {
  m_renderUpdatePending.store(false, std::memory_order_release);
  if (!m_renderPumpRunning.load(std::memory_order_acquire) || !hasVideo()) {
    return;
  }
  if (!HasUndeliveredFrame()) {
    return;
  }
  QueueRenderPumpUpdate();
}

void WebRTCVideoRenderer::setTraceTargetFrameId(int id) {
  if (m_traceTargetFrameId == id) {
    return;
  }
  m_traceTargetFrameId = id;
  Q_EMIT traceTargetFrameIdChanged();
}

void WebRTCVideoRenderer::applySampledPipelineUi(int glTraceFrameId, double decodeToRenderTotalMs,
                                                 double wallOnFrameToRenderMs) {
  m_sampledHighlightFrameId = glTraceFrameId;
  m_sampledDecodeToRenderMs = decodeToRenderTotalMs;
  m_sampledWallOnFrameToRenderMs = wallOnFrameToRenderMs;
  m_hasSampledPipelineUi = true;
  Q_EMIT sampledPipelineStatsChanged();
}

QString WebRTCVideoRenderer::sampledPipelineLine() const {
  if (!m_hasSampledPipelineUi) {
    return {};
  }
  const QString decodeToRender =
      m_sampledDecodeToRenderMs >= 0.0 ? QString::number(m_sampledDecodeToRenderMs, 'f', 3)
                                       : QStringLiteral("N/A");
  return QStringLiteral("sample frame=%1 | total=%2 ms | ui_queue=%3 ms")
      .arg(m_sampledHighlightFrameId)
      .arg(decodeToRender)
      .arg(m_sampledWallOnFrameToRenderMs, 0, 'f', 3);
}

QSGNode* WebRTCVideoRenderer::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
  if (!window()) {
    delete oldNode;
    return nullptr;
  }

  const auto api = window()->rendererInterface()->graphicsApi();
  if (api != QSGRendererInterface::OpenGL && api != QSGRendererInterface::OpenGLRhi) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      qWarning() << "[WebRTCVideoRenderer] QSGRenderNode requires OpenGL scene graph backend,"
                 << "current api =" << api;
    }
    delete oldNode;
    return nullptr;
  }

  auto* node = static_cast<WebRTCVideoRenderNode*>(oldNode);
  if (!node) {
    node = new WebRTCVideoRenderNode();
  }
  node->Sync(this);
  return node;
}

void WebRTCVideoRenderer::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
  QQuickItem::geometryChange(newGeometry, oldGeometry);
  if (newGeometry.size() != oldGeometry.size()) {
    update();
  }
}

void WebRTCVideoRenderer::setVideoTrack(webrtc::scoped_refptr<webrtc::VideoTrackInterface> track) {
  if (m_track == track) {
    return;
  }
  clearVideoTrack();
  m_track = std::move(track);
  if (m_track) {
    m_track->AddOrUpdateSink(this, webrtc::VideoSinkWants());
    qDebug() << "[VideoRenderer] video track attached via QSGRenderNode path";
  }
}

void WebRTCVideoRenderer::clearVideoTrack() {
  if (m_track) {
    m_track->RemoveSink(this);
    m_track = nullptr;
  }

  StopRenderPump();

  webrtc_demo::ResetEncodedFrameTrackingForUi();
  m_lastPolledEncodedIngressId = -1;

  {
    QMutexLocker locker(&m_frameMutex);
    m_latestFrame = {};
    m_syncFrame = {};
    m_highlightFrameId = -1;
    m_frameIdFromTracking = false;
    m_lastHighlightSignalMonoUs = 0;
    m_localPreviewSeq = 0;
  }

  const bool hadVideo = m_hasVideo.exchange(false, std::memory_order_acq_rel);
  if (hadVideo) {
    Q_EMIT hasVideoChanged();
  }
  Q_EMIT highlightFrameIdChanged();
  Q_EMIT encodedIngressTrackingChanged();

  if (m_hasSampledPipelineUi) {
    m_sampledHighlightFrameId = -1;
    m_sampledDecodeToRenderMs = -1.0;
    m_sampledWallOnFrameToRenderMs = -1.0;
    m_hasSampledPipelineUi = false;
    Q_EMIT sampledPipelineStatsChanged();
  }

  update();
}
