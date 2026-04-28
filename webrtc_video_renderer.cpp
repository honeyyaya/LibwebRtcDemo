#include "webrtc_video_renderer.h"
#include "encoded_tracking_bridge.h"
#include "video_decode_sink_timing_bridge.h"
#include "webrtc_oes_video_frame_buffer.h"

#include <atomic>

#include <QOpenGLExtraFunctions>
#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QQuickWindow>
#include <QSGRenderNode>
#include <QSGRendererInterface>
#include <QSurfaceFormat>
#include <QDebug>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QQuickItem>
#include <algorithm>
#include <iterator>
#include <cstring>
#include <vector>
#include "api/video/i420_buffer.h"
#include "api/video/nv12_buffer.h"
#include "api/video/video_frame_buffer.h"
#include "api/video/video_rotation.h"
#include "api/video/video_source_interface.h"
#include <limits>
#include <QtGlobal>

#if defined(__ANDROID__)
#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif
#endif

#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
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

struct I420PlaneTexCache {
    int w = 0;
    int h = 0;
};

struct PlaneUploadPbo {
    GLuint ids[2] = {0, 0};
    size_t capacities[2] = {0, 0};
    int next_stage_index = 0;
    int ready_index = -1;
    int ready_w = 0;
    int ready_h = 0;
    int ready_stride_bytes = 0;
    GLenum ready_internal_format = GL_LUMINANCE;
    GLenum ready_format = GL_LUMINANCE;
    int ready_bytes_per_pixel = 1;
    bool ready = false;
};

namespace {

bool SupportsUnpackRowLength()
{
    QOpenGLContext *const ctx = QOpenGLContext::currentContext();
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

bool SupportsPixelUnpackBuffer()
{
    QOpenGLContext *const ctx = QOpenGLContext::currentContext();
    if (!ctx) {
        return false;
    }
    if (!ctx->isOpenGLES()) {
        return true;
    }
    return ctx->format().majorVersion() >= 3;
}

/// 若不支持 UNPACK_ROW_LENGTH 且 src 行带 padding（stride>width），拷成紧密 stride==width 缓冲，
/// 供单块 TexSubImage 使用。禁止在 GL 侧逐行 TexSubImage。
const uint8_t *PrepareTightPlaneIfNeeded(const uint8_t *src,
                                         int src_stride,
                                         int plane_w,
                                         int plane_h,
                                         bool supports_unpack_row_length,
                                         std::vector<uint8_t> *tight)
{
    if (src_stride == plane_w || supports_unpack_row_length) {
        return src;
    }
    const size_t total = static_cast<size_t>(plane_w) * static_cast<size_t>(plane_h);
    tight->resize(total);
    for (int y = 0; y < plane_h; ++y) {
        const uint8_t *row = src + static_cast<size_t>(y) * static_cast<size_t>(src_stride);
        std::memcpy(tight->data() + static_cast<size_t>(y) * plane_w, row,
                    static_cast<size_t>(plane_w));
    }
    return tight->data();
}

size_t CopyPlaneForUpload(uint8_t *dst,
                          const uint8_t *src,
                          int src_stride_bytes,
                          int plane_w,
                          int plane_h,
                          int bytes_per_pixel,
                          bool supports_unpack_row_length)
{
    const int row_bytes = plane_w * bytes_per_pixel;
    if (supports_unpack_row_length && src_stride_bytes != row_bytes) {
        const size_t total =
            static_cast<size_t>(src_stride_bytes) * static_cast<size_t>(plane_h);
        std::memcpy(dst, src, total);
        return total;
    }
    for (int y = 0; y < plane_h; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * row_bytes,
                    src + static_cast<size_t>(y) * static_cast<size_t>(src_stride_bytes),
                    static_cast<size_t>(row_bytes));
    }
    return static_cast<size_t>(row_bytes) * static_cast<size_t>(plane_h);
}

/// 每平面一次 glTexSubImage2D；优先利用 UNPACK_ROW_LENGTH 直接上传带 stride 的平面，
/// 仅在 ES2/不支持扩展时回退到 CPU 紧排。
void UploadPlaneTight(QOpenGLExtraFunctions &gl,
                      GLenum texture_unit,
                      GLuint tex,
                      int plane_w,
                      int plane_h,
                      GLenum internal_format,
                      GLenum format,
                      int bytes_per_pixel,
                      const uint8_t *plane_data,
                      int src_stride_bytes,
                      bool supports_unpack_row_length,
                      I420PlaneTexCache *cached)
{
    gl.glActiveTexture(texture_unit);
    gl.glBindTexture(GL_TEXTURE_2D, tex);
    const bool same_size = cached && cached->w == plane_w && cached->h == plane_h;
    if (cached && !same_size) {
        cached->w = plane_w;
        cached->h = plane_h;
    }
    if (!same_size) {
        gl.glTexImage2D(GL_TEXTURE_2D, 0, internal_format, plane_w, plane_h, 0, format,
                        GL_UNSIGNED_BYTE, nullptr);
    }
    gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (supports_unpack_row_length) {
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, src_stride_bytes / bytes_per_pixel);
    }
    gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, plane_w, plane_h, format, GL_UNSIGNED_BYTE,
                       plane_data);
    if (supports_unpack_row_length) {
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
}

void ResetPlaneUploadPbo(PlaneUploadPbo *pbo)
{
    if (!pbo) {
        return;
    }
    pbo->next_stage_index = 0;
    pbo->ready_index = -1;
    pbo->ready_w = 0;
    pbo->ready_h = 0;
    pbo->ready_stride_bytes = 0;
    pbo->ready_internal_format = GL_LUMINANCE;
    pbo->ready_format = GL_LUMINANCE;
    pbo->ready_bytes_per_pixel = 1;
    pbo->ready = false;
}

bool UploadPlaneFromReadyPbo(QOpenGLExtraFunctions &gl,
                             GLenum texture_unit,
                             GLuint tex,
                             bool supports_unpack_row_length,
                             I420PlaneTexCache *cached,
                             PlaneUploadPbo *pbo)
{
    if (!pbo || !pbo->ready || pbo->ready_index < 0) {
        return false;
    }

    gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo->ids[pbo->ready_index]);
    gl.glActiveTexture(texture_unit);
    gl.glBindTexture(GL_TEXTURE_2D, tex);
    const bool same_size = cached && cached->w == pbo->ready_w && cached->h == pbo->ready_h;
    if (cached && !same_size) {
        cached->w = pbo->ready_w;
        cached->h = pbo->ready_h;
    }
    if (!same_size) {
        gl.glTexImage2D(GL_TEXTURE_2D, 0, pbo->ready_internal_format, pbo->ready_w, pbo->ready_h,
                        0, pbo->ready_format, GL_UNSIGNED_BYTE, nullptr);
    }
    gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (supports_unpack_row_length) {
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH,
                         pbo->ready_stride_bytes / pbo->ready_bytes_per_pixel);
    }
    gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pbo->ready_w, pbo->ready_h, pbo->ready_format,
                       GL_UNSIGNED_BYTE, nullptr);
    if (supports_unpack_row_length) {
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    return true;
}

bool StagePlaneIntoPbo(QOpenGLExtraFunctions &gl,
                       int plane_w,
                       int plane_h,
                       GLenum internal_format,
                       GLenum format,
                       int bytes_per_pixel,
                       const uint8_t *plane_data,
                       int src_stride_bytes,
                       bool supports_unpack_row_length,
                       PlaneUploadPbo *pbo)
{
    if (!pbo || (!pbo->ids[0] && !pbo->ids[1])) {
        return false;
    }

    const int index = pbo->next_stage_index;
    const GLuint upload_pbo = pbo->ids[index];
    pbo->next_stage_index = (index + 1) % 2;

    const size_t upload_bytes =
        (supports_unpack_row_length && src_stride_bytes != plane_w * bytes_per_pixel)
            ? static_cast<size_t>(src_stride_bytes) * static_cast<size_t>(plane_h)
            : static_cast<size_t>(plane_w * bytes_per_pixel) * static_cast<size_t>(plane_h);

    gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, upload_pbo);
    gl.glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(upload_bytes), nullptr,
                    GL_STREAM_DRAW);
    pbo->capacities[index] = upload_bytes;

    void *mapped = gl.glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0,
                                       static_cast<GLsizeiptr>(upload_bytes),
                                       GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    if (!mapped) {
        gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        return false;
    }

    CopyPlaneForUpload(static_cast<uint8_t *>(mapped), plane_data, src_stride_bytes, plane_w,
                       plane_h, bytes_per_pixel, supports_unpack_row_length);
    if (gl.glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER) == GL_FALSE) {
        gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        return false;
    }
    if (supports_unpack_row_length) {
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    pbo->ready_index = index;
    pbo->ready_w = plane_w;
    pbo->ready_h = plane_h;
    pbo->ready_stride_bytes = src_stride_bytes;
    pbo->ready_internal_format = internal_format;
    pbo->ready_format = format;
    pbo->ready_bytes_per_pixel = bytes_per_pixel;
    pbo->ready = true;
    return true;
}

bool UploadPlaneViaPboAsync(QOpenGLExtraFunctions &gl,
                            GLenum texture_unit,
                            GLuint tex,
                            int plane_w,
                            int plane_h,
                            GLenum internal_format,
                            GLenum format,
                            int bytes_per_pixel,
                            const uint8_t *plane_data,
                            int src_stride_bytes,
                            bool supports_unpack_row_length,
                            I420PlaneTexCache *cached,
                            PlaneUploadPbo *pbo)
{
    if (!pbo || (!pbo->ids[0] && !pbo->ids[1])) {
        UploadPlaneTight(gl, texture_unit, tex, plane_w, plane_h, internal_format, format,
                         bytes_per_pixel, plane_data, src_stride_bytes, supports_unpack_row_length,
                         cached);
        return true;
    }

    const bool uploaded = UploadPlaneFromReadyPbo(gl, texture_unit, tex, supports_unpack_row_length,
                                                  cached, pbo);
    if (!StagePlaneIntoPbo(gl, plane_w, plane_h, internal_format, format, bytes_per_pixel,
                           plane_data, src_stride_bytes, supports_unpack_row_length, pbo)) {
        ResetPlaneUploadPbo(pbo);
        UploadPlaneTight(gl, texture_unit, tex, plane_w, plane_h, internal_format, format,
                         bytes_per_pixel, plane_data, src_stride_bytes, supports_unpack_row_length,
                         cached);
        return true;
    }
    return uploaded;
}

}  // namespace

namespace webrtc_demo {

static webrtc::VideoSinkWants BuildVideoSinkWantsForRenderer(const QQuickItem *item)
{
    webrtc::VideoSinkWants wants;
    wants.rotation_applied = true;
    wants.black_frames = false;
    wants.is_active = false;
    // 2 的倍数便于 I420；行 stride==width 理想上应在解码/池化侧保证。
    // 渲染器优先用 UNPACK_ROW_LENGTH 直传带 stride 的平面，仅在 ES2/缺扩展时才回退 CPU 紧排。
    wants.resolution_alignment = 2;
    wants.max_pixel_count = std::numeric_limits<int>::max();
    if (item) {
        const int vw = qMax(1, static_cast<int>(item->width()));
        const int vh = qMax(1, static_cast<int>(item->height()));
        wants.target_pixel_count = vw * vh;
    }
    return wants;
}

/// 已是 I420 内存布局时走 GetI420 + 别名引用，避免无意义的 ToI420() 转换/拷贝；仅其它格式才 ToI420()。
static webrtc::scoped_refptr<webrtc::I420BufferInterface> ResolveI420ForUpload(
    const webrtc::scoped_refptr<webrtc::VideoFrameBuffer> &vfb)
{
    if (vfb->GetI420()) {
        return webrtc::scoped_refptr<webrtc::I420BufferInterface>(
            static_cast<webrtc::I420BufferInterface *>(vfb.get()));
    }
    return vfb->ToI420();
}

}  // namespace webrtc_demo

#ifndef GL_LUMINANCE_ALPHA
#define GL_LUMINANCE_ALPHA 0x190A
#endif

// =============================================================================
class WebRTCVideoRenderNode : public QSGRenderNode, protected QOpenGLExtraFunctions {
public:
    WebRTCVideoRenderNode() = default;

    ~WebRTCVideoRenderNode() override
    {
        destroyGlResources();
    }

    void sync(WebRTCVideoRenderer *item)
    {
        m_videoItem = item;
        m_rect = QRectF(0.0, 0.0, item->width(), item->height());
        updateGeometry();

        webrtc::scoped_refptr<webrtc::VideoFrameBuffer> vfb;
        if (!item->takeFrame(vfb) || !vfb) {
            if (!item->m_hasVideo) {
                m_hasData = false;
                m_heldBuffer = nullptr;
                m_pendingUpload = false;
            }
            m_uploadStatsValidForLastFrame = false;
            m_haveGlFrameTrace = false;
            return;
        }

        m_heldBuffer = vfb;
        m_pendingUpload = true;
        m_glQueueTraceStartMonoUs = item->lastTakenGlQueueTraceStartMonoUs();
        m_glQueueTraceFrameId = item->lastTakenGlQueueTraceFrameId();
        m_glQueueTraceFromTracking = item->lastTakenGlQueueTraceFrameFromTracking();
        m_haveGlFrameTrace = true;
    }

    StateFlags changedStates() const override
    {
        return DepthState | StencilState | ColorState | BlendState | CullState;
    }

    RenderingFlags flags() const override
    {
        return BoundedRectRendering;
    }

    QRectF rect() const override
    {
        return m_rect;
    }

    void render(const RenderState *state) override
    {
        if (!ensureInitialized()) {
            return;
        }

        if (m_pendingUpload) {
            uploadPendingFrame();
        }

        if (!m_hasData) {
            return;
        }

        QElapsedTimer paintTimer;
        paintTimer.start();

        QMatrix4x4 mvp = *state->projectionMatrix();
        if (matrix()) {
            mvp *= *matrix();
        }

#if defined(__ANDROID__)
        QOpenGLShaderProgram *activeProg =
            (m_frameIsOes && m_oesProgramLinked) ? &m_programOes
                                                 : (m_frameIsNv12 ? &m_programNV12 : &m_programI420);
#else
        QOpenGLShaderProgram *activeProg = m_frameIsNv12 ? &m_programNV12 : &m_programI420;
#endif
        activeProg->bind();
        activeProg->setUniformValue("qt_Matrix", mvp);
        activeProg->setAttributeArray(0, GL_FLOAT, m_vertices, 2);
        activeProg->enableAttributeArray(0);
        activeProg->setAttributeArray(1, GL_FLOAT, kTexCoords, 2);
        activeProg->enableAttributeArray(1);

#if defined(__ANDROID__)
        if (m_frameIsOes && m_oesProgramLinked) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_oesTextureId);
            activeProg->setUniformValue("tex_oes", 0);
        } else
#endif
        {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_texY);
            activeProg->setUniformValue("tex_y", 0);
            if (m_frameIsNv12) {
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, m_texUV);
                activeProg->setUniformValue("tex_uv", 1);
            } else {
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, m_texU);
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, m_texV);
                activeProg->setUniformValue("tex_u", 1);
                activeProg->setUniformValue("tex_v", 2);
            }
        }

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        activeProg->disableAttributeArray(0);
        activeProg->disableAttributeArray(1);
        activeProg->release();

        const qint64 tPaintUs = paintTimer.nsecsElapsed() / 1000;
        m_renderFrameCount++;

        if (m_uploadStatsValidForLastFrame && m_haveGlFrameTrace && m_glQueueTraceFromTracking) {
            const uint32_t tid = static_cast<uint32_t>(std::max(0, m_glQueueTraceFrameId));
            if (webrtc_demo::ShouldLogTrackingTimedSampleById(tid)) {
                const int64_t t_after_draw_mono_us = webrtc_demo::DecodeSinkMonotonicUs();
                const qint64 wall_from_queue_us = t_after_draw_mono_us - m_glQueueTraceStartMonoUs;
                const qint64 sum_upload_draw_us = m_lastUploadUs + tPaintUs;
                int64_t decode_pipeline_start_us = 0;
                const bool have_decode_to_render =
                    webrtc_demo::TakeDecodePipelineStartMonotonicUs(tid, &decode_pipeline_start_us);
                const double decode_to_render_ms =
                    have_decode_to_render
                        ? (t_after_draw_mono_us - decode_pipeline_start_us) / 1000.0
                        : -1.0;
                const QString decode_to_render_str =
                    have_decode_to_render
                        ? QString::number(decode_to_render_ms, 'f', 3)
                        : QStringLiteral("—");
                qDebug().noquote() << QString(
                    "【耗时分析】 frame_id=%1 | 上传(CPU发GL): %2 ms | 绘制(draw): %3 ms | 上传+绘制: %4 ms | "
                    "wall(OnFrame入队→render结束): %5 ms | 总(Decode入口→render结束): %6 ms "
                    "(Decode入口与 Mc/DecodeSink 同源单调钟; 与 tracking_id mod 120 对齐)")
                    .arg(m_glQueueTraceFrameId)
                    .arg(m_lastUploadUs / 1000.0, 0, 'f', 3)
                    .arg(tPaintUs / 1000.0, 0, 'f', 3)
                    .arg(sum_upload_draw_us / 1000.0, 0, 'f', 3)
                    .arg(wall_from_queue_us / 1000.0, 0, 'f', 3)
                    .arg(decode_to_render_str);
                webrtc_demo::printLocalTime(m_glQueueTraceFrameId);
                if (m_videoItem) {
                    QMetaObject::invokeMethod(
                        m_videoItem,
                        "applySampledPipelineUi",
                        Qt::QueuedConnection,
                        Q_ARG(int, m_glQueueTraceFrameId),
                        Q_ARG(double, decode_to_render_ms),
                        Q_ARG(double, wall_from_queue_us / 1000.0));
                }
            }
        }
    }

    void releaseResources() override
    {
        destroyGlResources();
    }

private:
    enum class UploadPathKind {
        None,
        Oes,
        Nv12,
        I420,
    };

    static constexpr GLfloat kTexCoords[8] = {
        1.0f, 1.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 0.0f, 0.0f
    };

    bool ensureInitialized()
    {
        if (m_glInitialized) {
            return true;
        }
        QOpenGLContext *const ctx = QOpenGLContext::currentContext();
        if (!ctx) {
            return false;
        }
        initializeOpenGLFunctions();
        m_supportsUnpackRowLength = SupportsUnpackRowLength();
        m_supportsPixelUnpackBuffer = SupportsPixelUnpackBuffer();
        initShaderI420();
        initShaderNV12();
        initTextures();
        initUploadPbos();
#if defined(__ANDROID__)
        initShaderOes();
#endif
        m_glInitialized = true;
        return true;
    }

    void destroyGlResources()
    {
        if (!m_glInitialized || !QOpenGLContext::currentContext()) {
            return;
        }
        glDeleteTextures(1, &m_texY);
        glDeleteTextures(1, &m_texU);
        glDeleteTextures(1, &m_texV);
        glDeleteTextures(1, &m_texUV);
        destroyUploadPbos();
        m_texY = m_texU = m_texV = m_texUV = 0;
#if defined(__ANDROID__)
        m_oesTextureId = 0;
        m_oesProgramLinked = false;
#endif
        m_glInitialized = false;
    }

    void updateGeometry()
    {
        const GLfloat w = static_cast<GLfloat>(m_rect.width());
        const GLfloat h = static_cast<GLfloat>(m_rect.height());
        const GLfloat verts[8] = {
            0.0f, 0.0f, w, 0.0f,
            0.0f, h,    w, h
        };
        std::copy(std::begin(verts), std::end(verts), m_vertices);
    }

    void resetCpuUploadPipeline()
    {
        ResetPlaneUploadPbo(&m_pboY);
        ResetPlaneUploadPbo(&m_pboU);
        ResetPlaneUploadPbo(&m_pboV);
        ResetPlaneUploadPbo(&m_pboUV);
    }

    void uploadPendingFrame()
    {
        m_pendingUpload = false;
        if (!m_heldBuffer) {
            m_uploadStatsValidForLastFrame = false;
            m_hasData = false;
            return;
        }

#if defined(__ANDROID__)
        if (webrtc_demo::OesEglTextureFrameBuffer *const oes =
                webrtc_demo::OesEglTextureFrameBuffer::Cast(m_heldBuffer.get())) {
            if (m_lastUploadPath != UploadPathKind::Oes) {
                resetCpuUploadPipeline();
                m_lastUploadPath = UploadPathKind::Oes;
            }
            if (m_oesProgramLinked) {
                m_frameIsOes = true;
                m_oesTextureId = static_cast<GLuint>(oes->oes_texture_id());
                QElapsedTimer uploadTimer;
                uploadTimer.start();
                oes->RunBeforeSampleOnGlThread();
                m_lastUploadUs = uploadTimer.nsecsElapsed() / 1000;
                m_uploadStatsValidForLastFrame = true;
                m_hasData = true;
                m_heldBuffer = nullptr;
                return;
            }
            m_frameIsOes = false;
            m_uploadStatsValidForLastFrame = false;
            m_hasData = false;
            m_heldBuffer = nullptr;
            return;
        }
#endif
        m_frameIsOes = false;
        m_frameIsNv12 = false;

        if (const webrtc::NV12BufferInterface *nv12 = m_heldBuffer->GetNV12()) {
            if (m_lastUploadPath != UploadPathKind::Nv12) {
                resetCpuUploadPipeline();
                m_lastUploadPath = UploadPathKind::Nv12;
                m_hasData = false;
            }
            const int w = nv12->width();
            const int h = nv12->height();
            const int hw = nv12->ChromaWidth();
            const int hh = nv12->ChromaHeight();

            QElapsedTimer uploadTimer;
            uploadTimer.start();
            bool uploaded = false;
            if (m_supportsPixelUnpackBuffer) {
                const bool y_uploaded = UploadPlaneViaPboAsync(
                    *this, GL_TEXTURE0, m_texY, w, h, GL_LUMINANCE, GL_LUMINANCE, 1,
                    nv12->DataY(), nv12->StrideY(), m_supportsUnpackRowLength, &m_planeCacheY,
                    &m_pboY);
                const bool uv_uploaded = UploadPlaneViaPboAsync(
                    *this, GL_TEXTURE1, m_texUV, hw, hh, GL_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA, 2,
                    nv12->DataUV(), nv12->StrideUV(), m_supportsUnpackRowLength, &m_planeCacheUV,
                    &m_pboUV);
                uploaded = y_uploaded && uv_uploaded;
            } else {
                const uint8_t *const pY = PrepareTightPlaneIfNeeded(
                    nv12->DataY(), nv12->StrideY(), w, h, m_supportsUnpackRowLength, &m_tightY);
                UploadPlaneTight(*this, GL_TEXTURE0, m_texY, w, h, GL_LUMINANCE, GL_LUMINANCE, 1,
                                 pY, m_supportsUnpackRowLength ? nv12->StrideY() : w,
                                 m_supportsUnpackRowLength, &m_planeCacheY);

                const int uv_row_bytes = hw * 2;
                const uint8_t *pUV = nv12->DataUV();
                if (!m_supportsUnpackRowLength && nv12->StrideUV() != uv_row_bytes) {
                    m_tightUV.resize(static_cast<size_t>(uv_row_bytes) * static_cast<size_t>(hh));
                    for (int y = 0; y < hh; ++y) {
                        std::memcpy(m_tightUV.data() + static_cast<size_t>(y) * uv_row_bytes,
                                    nv12->DataUV() +
                                        static_cast<size_t>(y) * static_cast<size_t>(nv12->StrideUV()),
                                    static_cast<size_t>(uv_row_bytes));
                    }
                    pUV = m_tightUV.data();
                }
                UploadPlaneTight(*this, GL_TEXTURE1, m_texUV, hw, hh, GL_LUMINANCE_ALPHA,
                                 GL_LUMINANCE_ALPHA, 2, pUV,
                                 m_supportsUnpackRowLength ? nv12->StrideUV() : uv_row_bytes,
                                 m_supportsUnpackRowLength, &m_planeCacheUV);
                uploaded = true;
            }

            m_lastUploadUs = uploadTimer.nsecsElapsed() / 1000;
            m_uploadStatsValidForLastFrame = uploaded;
            m_hasData = m_hasData || uploaded;
            m_frameIsNv12 = m_hasData;
            m_heldBuffer = nullptr;
            return;
        }

        if (m_lastUploadPath != UploadPathKind::I420) {
            resetCpuUploadPipeline();
            m_lastUploadPath = UploadPathKind::I420;
            m_hasData = false;
        }
        webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 =
            webrtc_demo::ResolveI420ForUpload(m_heldBuffer);
        if (!i420) {
            m_uploadStatsValidForLastFrame = false;
            m_hasData = false;
            m_heldBuffer = nullptr;
            return;
        }
        const int w = i420->width();
        const int h = i420->height();
        const int hw = w / 2;
        const int hh = h / 2;

        QElapsedTimer uploadTimer;
        uploadTimer.start();
        bool uploaded = false;
        if (m_supportsPixelUnpackBuffer) {
            const bool y_uploaded = UploadPlaneViaPboAsync(
                *this, GL_TEXTURE0, m_texY, w, h, GL_LUMINANCE, GL_LUMINANCE, 1, i420->DataY(),
                i420->StrideY(), m_supportsUnpackRowLength, &m_planeCacheY, &m_pboY);
            const bool u_uploaded = UploadPlaneViaPboAsync(
                *this, GL_TEXTURE1, m_texU, hw, hh, GL_LUMINANCE, GL_LUMINANCE, 1, i420->DataU(),
                i420->StrideU(), m_supportsUnpackRowLength, &m_planeCacheU, &m_pboU);
            const bool v_uploaded = UploadPlaneViaPboAsync(
                *this, GL_TEXTURE2, m_texV, hw, hh, GL_LUMINANCE, GL_LUMINANCE, 1, i420->DataV(),
                i420->StrideV(), m_supportsUnpackRowLength, &m_planeCacheV, &m_pboV);
            uploaded = y_uploaded && u_uploaded && v_uploaded;
        } else {
            const uint8_t *const pY = PrepareTightPlaneIfNeeded(
                i420->DataY(), i420->StrideY(), w, h, m_supportsUnpackRowLength, &m_tightY);
            const uint8_t *const pU = PrepareTightPlaneIfNeeded(
                i420->DataU(), i420->StrideU(), hw, hh, m_supportsUnpackRowLength, &m_tightU);
            const uint8_t *const pV = PrepareTightPlaneIfNeeded(
                i420->DataV(), i420->StrideV(), hw, hh, m_supportsUnpackRowLength, &m_tightV);
            UploadPlaneTight(*this, GL_TEXTURE0, m_texY, w, h, GL_LUMINANCE, GL_LUMINANCE, 1, pY,
                             i420->StrideY(), m_supportsUnpackRowLength, &m_planeCacheY);
            UploadPlaneTight(*this, GL_TEXTURE1, m_texU, hw, hh, GL_LUMINANCE, GL_LUMINANCE, 1, pU,
                             i420->StrideU(), m_supportsUnpackRowLength, &m_planeCacheU);
            UploadPlaneTight(*this, GL_TEXTURE2, m_texV, hw, hh, GL_LUMINANCE, GL_LUMINANCE, 1, pV,
                             i420->StrideV(), m_supportsUnpackRowLength, &m_planeCacheV);
            uploaded = true;
        }

        m_lastUploadUs = uploadTimer.nsecsElapsed() / 1000;
        m_uploadStatsValidForLastFrame = uploaded;
        m_hasData = m_hasData || uploaded;
        m_heldBuffer = nullptr;
    }

    void initShaderI420()
    {
        m_programI420.addShaderFromSourceCode(QOpenGLShader::Vertex,
            "uniform mat4 qt_Matrix;"
            "attribute vec4 vertexIn;"
            "attribute vec2 textureIn;"
            "varying vec2 textureOut;"
            "void main() {"
            "  gl_Position = qt_Matrix * vertexIn;"
            "  textureOut = textureIn;"
            "}");

        m_programI420.addShaderFromSourceCode(QOpenGLShader::Fragment,
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
            "}");
        m_programI420.bindAttributeLocation("vertexIn", 0);
        m_programI420.bindAttributeLocation("textureIn", 1);
        m_programI420.link();
    }

    void initShaderNV12()
    {
        m_programNV12.addShaderFromSourceCode(QOpenGLShader::Vertex,
            "uniform mat4 qt_Matrix;"
            "attribute vec4 vertexIn;"
            "attribute vec2 textureIn;"
            "varying vec2 textureOut;"
            "void main() {"
            "  gl_Position = qt_Matrix * vertexIn;"
            "  textureOut = textureIn;"
            "}");

        m_programNV12.addShaderFromSourceCode(QOpenGLShader::Fragment,
            "varying mediump vec2 textureOut;"
            "uniform sampler2D tex_y;"
            "uniform sampler2D tex_uv;"
            "void main() {"
            "  mediump vec3 yuv;"
            "  yuv.x = texture2D(tex_y, textureOut).r;"
            "  mediump vec2 uvv = texture2D(tex_uv, textureOut).ra - vec2(0.5, 0.5);"
            "  yuv.y = uvv.x;"
            "  yuv.z = uvv.y;"
            "  mediump vec3 rgb = mat3(1.0, 1.0, 1.0,"
            "                          0.0, -0.34413, 1.772,"
            "                          1.402, -0.71414, 0.0) * yuv;"
            "  gl_FragColor = vec4(rgb, 1.0);"
            "}");
        m_programNV12.bindAttributeLocation("vertexIn", 0);
        m_programNV12.bindAttributeLocation("textureIn", 1);
        m_programNV12.link();
    }

#if defined(__ANDROID__)
    void initShaderOes()
    {
        m_programOes.addShaderFromSourceCode(
            QOpenGLShader::Vertex,
            "uniform mat4 qt_Matrix;"
            "attribute vec4 vertexIn;"
            "attribute vec2 textureIn;"
            "varying vec2 textureOut;"
            "void main() {"
            "  gl_Position = qt_Matrix * vertexIn;"
            "  textureOut = textureIn;"
            "}");
        m_programOes.addShaderFromSourceCode(
            QOpenGLShader::Fragment,
            "#extension GL_OES_EGL_image_external : require\n"
            "varying mediump vec2 textureOut;\n"
            "uniform samplerExternalOES tex_oes;\n"
            "void main() {\n"
            "  gl_FragColor = texture2D(tex_oes, textureOut);\n"
            "}");
        m_programOes.bindAttributeLocation("vertexIn", 0);
        m_programOes.bindAttributeLocation("textureIn", 1);
        m_oesProgramLinked = m_programOes.link();
        if (!m_oesProgramLinked) {
            qDebug() << "[WebRTCVideoRenderNode] OES external shader link failed"
                     << m_programOes.log();
        }
    }
#endif

    void initTextures()
    {
        GLuint texIds[3];
        glGenTextures(3, texIds);
        m_texY = texIds[0];
        m_texU = texIds[1];
        m_texV = texIds[2];
        glGenTextures(1, &m_texUV);
        for (int i = 0; i < 3; ++i) {
            glBindTexture(GL_TEXTURE_2D, texIds[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, m_texUV);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    void initUploadPbos()
    {
        if (!m_supportsPixelUnpackBuffer) {
            return;
        }
        glGenBuffers(2, m_pboY.ids);
        glGenBuffers(2, m_pboU.ids);
        glGenBuffers(2, m_pboV.ids);
        glGenBuffers(2, m_pboUV.ids);
    }

    void destroyUploadPbos()
    {
        if (!m_supportsPixelUnpackBuffer) {
            return;
        }
        glDeleteBuffers(2, m_pboY.ids);
        glDeleteBuffers(2, m_pboU.ids);
        glDeleteBuffers(2, m_pboV.ids);
        glDeleteBuffers(2, m_pboUV.ids);
        m_pboY = PlaneUploadPbo{};
        m_pboU = PlaneUploadPbo{};
        m_pboV = PlaneUploadPbo{};
        m_pboUV = PlaneUploadPbo{};
    }

    bool m_glInitialized = false;
    QRectF m_rect;
    GLfloat m_vertices[8] = {0};
    QOpenGLShaderProgram m_programI420;
    QOpenGLShaderProgram m_programNV12;
    bool m_supportsUnpackRowLength = false;
    bool m_supportsPixelUnpackBuffer = false;
    bool m_frameIsNv12 = false;
    bool m_frameIsOes = false;
    UploadPathKind m_lastUploadPath = UploadPathKind::None;
#if defined(__ANDROID__)
    QOpenGLShaderProgram m_programOes;
    bool m_oesProgramLinked = false;
    GLuint m_oesTextureId = 0;
#endif
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> m_heldBuffer;
    bool m_pendingUpload = false;
    GLuint m_texY = 0, m_texU = 0, m_texV = 0, m_texUV = 0;
    I420PlaneTexCache m_planeCacheY, m_planeCacheU, m_planeCacheV, m_planeCacheUV;
    PlaneUploadPbo m_pboY, m_pboU, m_pboV, m_pboUV;
    std::vector<uint8_t> m_tightY, m_tightU, m_tightV, m_tightUV;
    bool m_hasData = false;

    int m_renderFrameCount = 0;
    qint64 m_lastUploadUs = 0;
    bool m_uploadStatsValidForLastFrame = false;

    int64_t m_glQueueTraceStartMonoUs = 0;
    int m_glQueueTraceFrameId = -1;
    bool m_glQueueTraceFromTracking = false;
    bool m_haveGlFrameTrace = false;

    WebRTCVideoRenderer *m_videoItem = nullptr;
};

// =============================================================================

WebRTCVideoRenderer::WebRTCVideoRenderer(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    startTimer(1000 / 60);
}

WebRTCVideoRenderer::~WebRTCVideoRenderer()
{
    clearVideoTrack();
}

void WebRTCVideoRenderer::timerEvent(QTimerEvent *)
{
    const int32_t raw = webrtc_demo::GetLastEncodedFrameTrackingIdForUi();
    const int cur = raw >= 0 ? static_cast<int>(raw) : -1;
    if (cur != m_lastPolledEncodedIngressId) {
        m_lastPolledEncodedIngressId = cur;
        Q_EMIT encodedIngressTrackingChanged();
    }
    // 画面重绘由 OnFrame 投递到 GUI 线程后 update()；此处仅轮询编码入站 tracking。
}

void WebRTCVideoRenderer::OnFrame(const webrtc::VideoFrame &frame)
{
    QElapsedTimer onFrameTotalTimer;
    onFrameTotalTimer.start();

    const int64_t t_onframe_us = webrtc_demo::DecodeSinkMonotonicUs();
    const uint32_t rtp_ts = frame.rtp_timestamp();
    int64_t t_after_decoded = 0;
    if (webrtc_demo::DecodeSinkTakeDecodedReturn(rtp_ts, &t_after_decoded)) {
        const int64_t de_us = t_onframe_us - t_after_decoded;
        const uint16_t fid = frame.id();
        if (fid != webrtc::VideoFrame::kNotSetId &&
            webrtc_demo::ShouldLogTrackingTimedSampleById(static_cast<uint32_t>(fid))) {
            qDebug().noquote() << QString(
                "【耗时分析】WebRTC 从Decode -> OnFrame tracking_id=%1 rtp_ts=%2 de=%3us (Decoded返回->OnFrame入口; "
                "WebRTC内部至sink线程; 与 tracking_id mod 120 对齐)")
                .arg(static_cast<uint>(fid))
                .arg(rtp_ts)
                .arg(static_cast<qint64>(de_us));
        }
    }

    static int on_frame_calls = 0;
    ++on_frame_calls;
    if (on_frame_calls == 1 || (on_frame_calls % 30) == 0) {
        qDebug() << "[VideoRenderer] OnFrame 调用#" << on_frame_calls
                 << "rtp_ts=" << frame.rtp_timestamp();
    }

    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer = frame.video_frame_buffer();
    if (!buffer)
        return;

    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> pending;
    int w = 0;
    int h = 0;
    bool oes_native = false;
    bool is_nv12 = false;

    if (webrtc_demo::OesEglTextureFrameBuffer::Cast(buffer.get())) {
        // kNative OES / SurfaceTexture：不经过 ToI420
        pending = buffer;
        w = buffer->width();
        h = buffer->height();
        oes_native = true;
    } else if (buffer->GetNV12()) {
        // 已是 NV12：直接挂接同一缓冲引用，避免退回 ToI420()
        pending = buffer;
        w = buffer->width();
        h = buffer->height();
        is_nv12 = true;
    } else if (buffer->GetI420()) {
        // 已是 I420：只挂接同一缓冲引用，避免 ToI420()
        pending = buffer;
        w = buffer->width();
        h = buffer->height();
    } else {
        webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 = webrtc_demo::ResolveI420ForUpload(buffer);
        if (!i420)
            return;
        w = i420->width();
        h = i420->height();
        if (w <= 0 || h <= 0)
            return;
        pending = i420;
    }

    qint64 tIntervalUs = m_decodeIntervalTimer.isValid() ? m_decodeIntervalTimer.nsecsElapsed() / 1000 : 0;
    m_decodeIntervalTimer.start();

    const uint16_t raw_id = frame.id();

    QElapsedTimer handoffTimer;
    handoffTimer.start();
    qint64 tHandoffUs = 0;
    bool id_changed = false;
    bool from_tracking_for_log = false;
    int display_id_for_log = -1;
    {
        QMutexLocker lock(&m_frameMutex);
        const bool from_tracking = (raw_id != webrtc::VideoFrame::kNotSetId);
        const int display_id =
            from_tracking ? static_cast<int>(raw_id) : static_cast<int>(++m_localPreviewSeq);
        from_tracking_for_log = from_tracking;
        display_id_for_log = display_id;
        m_pendingBuffer = pending;
        m_pendingValid = true;
        m_pendingGlQueueTraceStartMonoUs = webrtc_demo::DecodeSinkMonotonicUs();
        m_pendingGlQueueTraceFrameId = display_id;
        m_pendingGlQueueTraceFromTracking = from_tracking;
        if (m_highlightFrameId != display_id || m_frameIdFromTracking != from_tracking) {
            m_highlightFrameId = display_id;
            m_frameIdFromTracking = from_tracking;
            id_changed = true;
        }
        tHandoffUs = handoffTimer.nsecsElapsed() / 1000;
    }
    if (id_changed) {
        QMetaObject::invokeMethod(
            this, [this]() { Q_EMIT highlightFrameIdChanged(); }, Qt::QueuedConnection);
    }

    m_frameCount++;

    if (!m_hasVideo) {
        m_hasVideo = true;
        QMetaObject::invokeMethod(this, [this]() { Q_EMIT hasVideoChanged(); }, Qt::QueuedConnection);
    }

    // OnFrame 在 WebRTC 工作线程；update() 必须在 GUI 线程调用，否则 Qt 报
    // "Updates can only be scheduled from GUI thread or from QQuickItem::updatePaintNode()"。
    QMetaObject::invokeMethod(
        this,
        [this]() {
            update();
        },
        Qt::QueuedConnection);

    const qint64 tOnFrameTotalUs = onFrameTotalTimer.nsecsElapsed() / 1000;
    if (from_tracking_for_log &&
        webrtc_demo::ShouldLogTrackingTimedSampleById(static_cast<uint32_t>(display_id_for_log))) {
        qDebug().noquote() << QString(
            "【耗时分析】 OnFrame tracking_id=%1 | 投递(锁内挂接buffer,无整帧拷贝): %2 ms | 帧间隔: %3 ms | 数据量: %4 | "
            "OnFrame整体: %5 ms (入口至返回,含NV12/I420/OES适配与锁内挂接; 与 tracking_id mod 120 对齐)")
                .arg(display_id_for_log)
                .arg(tHandoffUs / 1000.0, 0, 'f', 3)
                .arg(tIntervalUs / 1000.0, 0, 'f', 2)
                .arg(oes_native
                         ? QStringLiteral("OES/kNative 纹理")
                         : (is_nv12
                                ? QStringLiteral("NV12 双平面")
                                : QString::number((w * h * 3 / 2) / 1024) + QStringLiteral(" KB")))
                .arg(tOnFrameTotalUs / 1000.0, 0, 'f', 3);
    }
}

int WebRTCVideoRenderer::highlightFrameId() const {
    QMutexLocker lock(&m_frameMutex);
    return m_highlightFrameId;
}

bool WebRTCVideoRenderer::frameIdFromTracking() const {
    QMutexLocker lock(&m_frameMutex);
    return m_frameIdFromTracking;
}

int WebRTCVideoRenderer::encodedIngressTrackingId() const {
    const int32_t v = webrtc_demo::GetLastEncodedFrameTrackingIdForUi();
    return v >= 0 ? static_cast<int>(v) : -1;
}

bool WebRTCVideoRenderer::hasEncodedIngressTracking() const {
    return webrtc_demo::GetLastEncodedFrameTrackingIdForUi() >= 0;
}

bool WebRTCVideoRenderer::takeFrame(webrtc::scoped_refptr<webrtc::VideoFrameBuffer> &out)
{
    QMutexLocker lock(&m_frameMutex);
    if (!m_pendingValid || !m_pendingBuffer)
        return false;
    out = m_pendingBuffer;
    m_lastTakenGlQueueTraceStartMonoUs = m_pendingGlQueueTraceStartMonoUs;
    m_lastTakenGlQueueTraceFrameId = m_pendingGlQueueTraceFrameId;
    m_lastTakenGlQueueTraceFromTracking = m_pendingGlQueueTraceFromTracking;
    m_pendingBuffer = nullptr;
    m_pendingValid = false;
    return true;
}

void WebRTCVideoRenderer::setTraceTargetFrameId(int id)
{
    if (m_traceTargetFrameId == id)
        return;
    m_traceTargetFrameId = id;
    Q_EMIT traceTargetFrameIdChanged();
}

void WebRTCVideoRenderer::applySampledPipelineUi(int glTraceFrameId, double decodeToRenderTotalMs,
                                                 double wallOnFrameToRenderMs)
{
    m_sampledHighlightFrameId = glTraceFrameId;
    m_sampledDecodeToRenderMs = decodeToRenderTotalMs;
    m_sampledWallOnFrameToRenderMs = wallOnFrameToRenderMs;
    m_hasSampledPipelineUi = true;
    Q_EMIT sampledPipelineStatsChanged();
}

QString WebRTCVideoRenderer::sampledPipelineLine() const
{
    if (!m_hasSampledPipelineUi)
        return {};
    const QString dtr = (m_sampledDecodeToRenderMs >= 0.0)
                            ? QString::number(m_sampledDecodeToRenderMs, 'f', 3)
                            : QStringLiteral("—");
    return QStringLiteral("采样 highlight=%1 (=GL trace frame_id) | 总(Decode→render)=%2 ms | "
                          "wall(OnFrame→render)=%3 ms（与 log 同 mod 120 采样）")
        .arg(m_sampledHighlightFrameId)
        .arg(dtr)
        .arg(m_sampledWallOnFrameToRenderMs, 0, 'f', 3);
}

QSGNode *WebRTCVideoRenderer::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    if (!window()) {
        delete oldNode;
        return nullptr;
    }

    const auto api = window()->rendererInterface()->graphicsApi();
    if (api != QSGRendererInterface::OpenGL &&
        api != QSGRendererInterface::OpenGLRhi) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            qWarning() << "[WebRTCVideoRenderer] QSGRenderNode path requires OpenGL scene graph backend,"
                       << "current api =" << api;
        }
        delete oldNode;
        return nullptr;
    }

    auto *node = static_cast<WebRTCVideoRenderNode *>(oldNode);
    if (!node) {
        node = new WebRTCVideoRenderNode();
    }
    node->sync(this);
    return node;
}

void WebRTCVideoRenderer::applyVideoSinkWants()
{
    if (!m_track)
        return;
    m_track->AddOrUpdateSink(this, webrtc_demo::BuildVideoSinkWantsForRenderer(this));
}

void WebRTCVideoRenderer::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (m_track && newGeometry.size() != oldGeometry.size()) {
        applyVideoSinkWants();
    }
    if (newGeometry.size() != oldGeometry.size()) {
        update();
    }
}

void WebRTCVideoRenderer::setVideoTrack(webrtc::scoped_refptr<webrtc::VideoTrackInterface> track)
{
    if (m_track == track)
        return;
    clearVideoTrack();
    m_track = track;
    if (m_track) {
        applyVideoSinkWants();
        qDebug() << "[VideoRenderer] 已绑定视频轨道 (VideoSinkWants+OpenGL)；首帧解码成功后才 hasVideo";
    }
}

void WebRTCVideoRenderer::clearVideoTrack()
{
    if (m_track) {
        m_track->RemoveSink(this);
        m_track = nullptr;
    }
    webrtc_demo::ResetEncodedFrameTrackingForUi();
    m_lastPolledEncodedIngressId = -1;
    bool id_reset = false;
    {
        QMutexLocker lock(&m_frameMutex);
        m_pendingBuffer = nullptr;
        m_pendingValid = false;
        m_highlightFrameId = -1;
        m_frameIdFromTracking = false;
        m_localPreviewSeq = 0;
        m_pendingGlQueueTraceFrameId = -1;
        m_pendingGlQueueTraceStartMonoUs = 0;
        m_pendingGlQueueTraceFromTracking = false;
        m_lastTakenGlQueueTraceFrameId = -1;
        m_lastTakenGlQueueTraceStartMonoUs = 0;
        m_lastTakenGlQueueTraceFromTracking = false;
        id_reset = true;
    }
    if (m_hasVideo) {
        m_hasVideo = false;
        Q_EMIT hasVideoChanged();
    }
    if (id_reset) {
        Q_EMIT highlightFrameIdChanged();
    }
    if (m_hasSampledPipelineUi) {
        m_sampledHighlightFrameId = -1;
        m_sampledDecodeToRenderMs = -1.0;
        m_sampledWallOnFrameToRenderMs = -1.0;
        m_hasSampledPipelineUi = false;
        Q_EMIT sampledPipelineStatsChanged();
    }
    Q_EMIT encodedIngressTrackingChanged();
    QMetaObject::invokeMethod(
        this,
        [this]() {
            update();
        },
        Qt::QueuedConnection);
}
