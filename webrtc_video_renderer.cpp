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
#include "api/video/video_frame_buffer.h"
#include "api/video/video_rotation.h"
#include "api/video/video_source_interface.h"

#define STATS_INTERVAL 60

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
        if (m_renderFrameCount % STATS_INTERVAL == 0) {
            qDebug().noquote() << QString(
                "[VideoPerf-GL] render#%1 | 纹理上传: %2 ms | GPU渲染: %3 ms")
                .arg(m_renderFrameCount)
                .arg(m_lastUploadUs / 1000.0, 0, 'f', 2)
                .arg(tPaintUs / 1000.0, 0, 'f', 2);
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
        if (!vi->takeFrame(i420) || !i420)
            return;

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

        m_lastUploadUs = uploadTimer.nsecsElapsed() / 1000;
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
    if (m_hasVideo)
        update();
}

void WebRTCVideoRenderer::OnFrame(const webrtc::VideoFrame &frame)
{
    const int64_t t_onframe_us = webrtc_demo::DecodeSinkMonotonicUs();
    const uint32_t rtp_ts = frame.rtp_timestamp();
    int64_t t_after_decoded = 0;
    if (webrtc_demo::DecodeSinkTakeDecodedReturn(rtp_ts, &t_after_decoded)) {
        const int64_t de_us = t_onframe_us - t_after_decoded;
        static std::atomic<uint64_t> g_mc_de_seq{0};
        const uint64_t n = ++g_mc_de_seq;
        if (n <= 10u || (n % 30u) == 0u) {
            qDebug().noquote() << QString(
                "[VideoPerf] McDE #%1 rtp_ts=%2 de=%3us (Decoded返回->OnFrame入口; "
                "WebRTC内部至sink线程)")
                .arg(static_cast<quint64>(n))
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
    {
        QMutexLocker lock(&m_frameMutex);
        const bool from_tracking = (raw_id != webrtc::VideoFrame::kNotSetId);
        const int display_id =
            from_tracking ? static_cast<int>(raw_id) : static_cast<int>(++m_localPreviewSeq);
        m_pendingI420 = i420;
        m_pendingValid = true;
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
    if (m_frameCount % STATS_INTERVAL == 0) {
        qDebug().noquote() << QString(
            "[VideoPerf-GL] OnFrame#%1 | 投递(锁内挂接I420,无整帧拷贝): %2 ms | 帧间隔: %3 ms | 数据量: %4 KB")
                .arg(m_frameCount)
                .arg(tHandoffUs / 1000.0, 0, 'f', 3)
                .arg(tIntervalUs / 1000.0, 0, 'f', 2)
                .arg((w * h * 3 / 2) / 1024);
    }

    if (!m_hasVideo) {
        m_hasVideo = true;
        QMetaObject::invokeMethod(this, [this]() { Q_EMIT hasVideoChanged(); }, Qt::QueuedConnection);
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
    m_pendingI420 = nullptr;
    m_pendingValid = false;
    return true;
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
    update();
}
