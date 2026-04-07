#include "webrtc_video_renderer.h"
#include "encoded_tracking_bridge.h"
#include "video_decode_sink_timing_bridge.h"

#include <atomic>

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QDebug>
#include <QElapsedTimer>
#include <algorithm>
#include "api/video/video_frame_buffer.h"
#include "api/video/video_rotation.h"
#include "api/video/video_source_interface.h"

#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif

namespace {

void UploadLuminancePlane(QOpenGLExtraFunctions &gl,
                          bool use_unpack_row_length,
                          GLenum texture_unit,
                          GLuint tex,
                          int plane_w,
                          int plane_h,
                          const uint8_t *data,
                          int stride_bytes)
{
    gl.glActiveTexture(texture_unit);
    gl.glBindTexture(GL_TEXTURE_2D, tex);
    if (stride_bytes == plane_w) {
        gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, plane_w, plane_h, 0, GL_LUMINANCE,
                        GL_UNSIGNED_BYTE, data);
        return;
    }
    if (use_unpack_row_length) {
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, stride_bytes);
        gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, plane_w, plane_h, 0, GL_LUMINANCE,
                        GL_UNSIGNED_BYTE, data);
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        return;
    }
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, plane_w, plane_h, 0, GL_LUMINANCE,
                    GL_UNSIGNED_BYTE, nullptr);
    for (int row = 0; row < plane_h; ++row) {
        gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, plane_w, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE,
                           data + row * stride_bytes);
    }
}

}  // namespace

// =============================================================================
class WebRTCGLRenderer : public QQuickFramebufferObject::Renderer, protected QOpenGLExtraFunctions {
public:
    WebRTCGLRenderer()
    {
        initializeOpenGLFunctions();
        initShader();
        initTextures();
    }

    ~WebRTCGLRenderer() override
    {
        glDeleteTextures(1, &m_texY);
        glDeleteTextures(1, &m_texU);
        glDeleteTextures(1, &m_texV);
    }

    void render() override
    {
        QElapsedTimer paintTimer;
        paintTimer.start();

        if (!m_hasData) {
            glClearColor(0.059f, 0.086f, 0.161f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            return;
        }

        glClear(GL_COLOR_BUFFER_BIT);
        m_program.bind();

        static const GLfloat verts[] = {
            -1.0f, -1.0f, 1.0f, -1.0f,
            -1.0f,  1.0f, 1.0f,  1.0f
        };
        static const GLfloat texCoords[] = {
            0.0f, 1.0f, 1.0f, 1.0f,
            0.0f, 0.0f, 1.0f, 0.0f
        };

        m_program.setAttributeArray(0, GL_FLOAT, verts, 2);
        m_program.enableAttributeArray(0);
        m_program.setAttributeArray(1, GL_FLOAT, texCoords, 2);
        m_program.enableAttributeArray(1);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_texY);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_texU);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_texV);

        m_program.setUniformValue("tex_y", 0);
        m_program.setUniformValue("tex_u", 1);
        m_program.setUniformValue("tex_v", 2);

        glDisable(GL_DEPTH_TEST);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        m_program.disableAttributeArray(0);
        m_program.disableAttributeArray(1);
        m_program.release();

        qint64 tPaintUs = paintTimer.nsecsElapsed() / 1000;
        m_renderFrameCount++;

        if (m_uploadStatsValidForLastSync && m_haveGlFrameTrace && m_glQueueTraceFromTracking) {
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
                QString decode_to_render_str =
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
            }
        }
    }

    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override
    {
        QOpenGLFramebufferObjectFormat fmt;
        fmt.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        return new QOpenGLFramebufferObject(size, fmt);
    }

    void synchronize(QQuickFramebufferObject *item) override
    {
        auto *vi = qobject_cast<WebRTCVideoRenderer *>(item);
        if (!vi)
            return;

        if (!m_uploadCapsChecked) {
            m_uploadCapsChecked = true;
            QOpenGLContext *ctx = QOpenGLContext::currentContext();
            if (ctx && ctx->format().majorVersion() >= 3)
                m_useUnpackRowLength = true;
        }

        webrtc::scoped_refptr<webrtc::I420BufferInterface> i420;
        if (!vi->takeFrame(i420) || !i420) {
            // 本帧未上传新纹理；render() 仍会画上一帧内容，m_lastUploadUs 不应再代表「本帧」。
            m_uploadStatsValidForLastSync = false;
            m_haveGlFrameTrace = false;
            return;
        }

        m_glQueueTraceStartMonoUs = vi->lastTakenGlQueueTraceStartMonoUs();
        m_glQueueTraceFrameId = vi->lastTakenGlQueueTraceFrameId();
        m_glQueueTraceFromTracking = vi->lastTakenGlQueueTraceFrameFromTracking();
        m_haveGlFrameTrace = true;

        const int w = i420->width();
        const int h = i420->height();
        const int hw = w / 2;
        const int hh = h / 2;

        QElapsedTimer uploadTimer;
        uploadTimer.start();

        UploadLuminancePlane(*this, m_useUnpackRowLength, GL_TEXTURE0, m_texY, w, h, i420->DataY(),
                             i420->StrideY());
        UploadLuminancePlane(*this, m_useUnpackRowLength, GL_TEXTURE1, m_texU, hw, hh, i420->DataU(),
                             i420->StrideU());
        UploadLuminancePlane(*this, m_useUnpackRowLength, GL_TEXTURE2, m_texV, hw, hh, i420->DataV(),
                             i420->StrideV());

        // CPU 上 glTex(Sub)Image 返回耗时；驱动/GPU 可能异步拷贝，非「纹理已在显存就绪」的墙钟。
        m_lastUploadUs = uploadTimer.nsecsElapsed() / 1000;
        m_uploadStatsValidForLastSync = true;
        m_hasData = true;
    }

private:
    void initShader()
    {
        m_program.addShaderFromSourceCode(QOpenGLShader::Vertex,
            "attribute vec4 vertexIn;"
            "attribute vec2 textureIn;"
            "varying vec2 textureOut;"
            "void main() {"
            "  gl_Position = vertexIn;"
            "  textureOut = textureIn;"
            "}");

        m_program.addShaderFromSourceCode(QOpenGLShader::Fragment,
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

        m_program.bindAttributeLocation("vertexIn", 0);
        m_program.bindAttributeLocation("textureIn", 1);
        m_program.link();
    }

    void initTextures()
    {
        GLuint texIds[3];
        glGenTextures(3, texIds);
        m_texY = texIds[0];
        m_texU = texIds[1];
        m_texV = texIds[2];

        for (int i = 0; i < 3; ++i) {
            glBindTexture(GL_TEXTURE_2D, texIds[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }

    QOpenGLShaderProgram m_program;
    GLuint m_texY = 0, m_texU = 0, m_texV = 0;
    bool m_hasData = false;

    int m_renderFrameCount = 0;
    qint64 m_lastUploadUs = 0;
    /// 最近一次 synchronize 是否成功执行了 Y/U/V 上传（与随后 render 对应）。
    bool m_uploadStatsValidForLastSync = false;

    int64_t m_glQueueTraceStartMonoUs = 0;
    int m_glQueueTraceFrameId = -1;
    bool m_glQueueTraceFromTracking = false;
    bool m_haveGlFrameTrace = false;

    bool m_uploadCapsChecked = false;
    bool m_useUnpackRowLength = false;
};

// =============================================================================

WebRTCVideoRenderer::WebRTCVideoRenderer(QQuickItem *parent)
    : QQuickFramebufferObject(parent)
{
    setMirrorVertically(true);
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
    webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 = buffer->ToI420();
    if (!i420)
        return;

    const int w = i420->width();
    const int h = i420->height();
    if (w <= 0 || h <= 0)
        return;

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
        m_pendingI420 = i420;
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
            "【耗时分析】 OnFrame tracking_id=%1 | 投递(锁内挂接I420,无整帧拷贝): %2 ms | 帧间隔: %3 ms | 数据量: %4 KB | "
            "OnFrame整体: %5 ms (入口至返回,含ToI420与锁内挂接; 与 tracking_id mod 120 对齐)")
                .arg(display_id_for_log)
                .arg(tHandoffUs / 1000.0, 0, 'f', 3)
                .arg(tIntervalUs / 1000.0, 0, 'f', 2)
                .arg((w * h * 3 / 2) / 1024)
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

bool WebRTCVideoRenderer::takeFrame(webrtc::scoped_refptr<webrtc::I420BufferInterface> &out)
{
    QMutexLocker lock(&m_frameMutex);
    if (!m_pendingValid || !m_pendingI420)
        return false;
    out = m_pendingI420;
    m_lastTakenGlQueueTraceStartMonoUs = m_pendingGlQueueTraceStartMonoUs;
    m_lastTakenGlQueueTraceFrameId = m_pendingGlQueueTraceFrameId;
    m_lastTakenGlQueueTraceFromTracking = m_pendingGlQueueTraceFromTracking;
    m_pendingI420 = nullptr;
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

QQuickFramebufferObject::Renderer *WebRTCVideoRenderer::createRenderer() const
{
    return new WebRTCGLRenderer();
}

void WebRTCVideoRenderer::setVideoTrack(webrtc::scoped_refptr<webrtc::VideoTrackInterface> track)
{
    if (m_track == track)
        return;
    clearVideoTrack();
    m_track = track;
    if (m_track) {
        m_track->AddOrUpdateSink(this, webrtc::VideoSinkWants());
        qDebug() << "[VideoRenderer] 已绑定视频轨道 (OpenGL)；首帧解码成功后才 hasVideo";
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
        m_pendingI420 = nullptr;
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
    Q_EMIT encodedIngressTrackingChanged();
    QMetaObject::invokeMethod(
        this,
        [this]() {
            update();
        },
        Qt::QueuedConnection);
}
