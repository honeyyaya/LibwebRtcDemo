#include "webrtc_video_renderer.h"

#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QDebug>
#include <QElapsedTimer>
#include <cstring>

#include "api/video/video_frame_buffer.h"
#include "api/video/video_rotation.h"
#include "api/video/video_source_interface.h"

#define STATS_INTERVAL 60

// =============================================================================
class WebRTCGLRenderer : public QQuickFramebufferObject::Renderer, protected QOpenGLFunctions {
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

        YuvFrameData frame;
        if (!vi->takeFrame(frame))
            return;

        QElapsedTimer uploadTimer;
        uploadTimer.start();

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_texY);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE,
                     frame.width, frame.height, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, frame.y.constData());

        int hw = frame.width / 2;
        int hh = frame.height / 2;

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_texU);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE,
                     hw, hh, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, frame.u.constData());

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_texV);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE,
                     hw, hh, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, frame.v.constData());

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
    if (m_hasVideo)
        update();
}

void WebRTCVideoRenderer::OnFrame(const webrtc::VideoFrame &frame)
{
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

    QElapsedTimer copyTimer;
    copyTimer.start();

    YuvFrameData yuvFrame;
    yuvFrame.width = w;
    yuvFrame.height = h;

    int strideY = i420->StrideY();
    int strideU = i420->StrideU();
    int strideV = i420->StrideV();

    if (strideY == w) {
        yuvFrame.y = QByteArray(reinterpret_cast<const char *>(i420->DataY()), w * h);
    } else {
        yuvFrame.y.resize(w * h);
        for (int row = 0; row < h; ++row)
            std::memcpy(yuvFrame.y.data() + row * w, i420->DataY() + row * strideY, w);
    }

    int hw = w / 2, hh = h / 2;
    if (strideU == hw) {
        yuvFrame.u = QByteArray(reinterpret_cast<const char *>(i420->DataU()), hw * hh);
    } else {
        yuvFrame.u.resize(hw * hh);
        for (int row = 0; row < hh; ++row)
            std::memcpy(yuvFrame.u.data() + row * hw, i420->DataU() + row * strideU, hw);
    }

    if (strideV == hw) {
        yuvFrame.v = QByteArray(reinterpret_cast<const char *>(i420->DataV()), hw * hh);
    } else {
        yuvFrame.v.resize(hw * hh);
        for (int row = 0; row < hh; ++row)
            std::memcpy(yuvFrame.v.data() + row * hw, i420->DataV() + row * strideV, hw);
    }

    yuvFrame.valid = true;

    qint64 tCopyUs = copyTimer.nsecsElapsed() / 1000;

    {
        QMutexLocker lock(&m_frameMutex);
        m_pendingFrame = std::move(yuvFrame);
    }

    m_frameCount++;
    if (m_frameCount % STATS_INTERVAL == 0) {
        qDebug().noquote() << QString(
            "[VideoPerf-GL] OnFrame#%1 | YUV拷贝: %2 ms | 帧间隔: %3 ms | 数据量: %4 KB")
                .arg(m_frameCount)
                .arg(tCopyUs / 1000.0, 0, 'f', 2)
                .arg(tIntervalUs / 1000.0, 0, 'f', 2)
                .arg((w * h * 3 / 2) / 1024);
    }

    if (!m_hasVideo) {
        m_hasVideo = true;
        QMetaObject::invokeMethod(this, [this]() { Q_EMIT hasVideoChanged(); }, Qt::QueuedConnection);
    }
}

bool WebRTCVideoRenderer::takeFrame(YuvFrameData &out)
{
    QMutexLocker lock(&m_frameMutex);
    if (!m_pendingFrame.valid)
        return false;
    out = std::move(m_pendingFrame);
    m_pendingFrame.valid = false;
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
        if (!m_hasVideo) {
            m_hasVideo = true;
            Q_EMIT hasVideoChanged();
        }
        qDebug() << "[VideoRenderer] 已绑定视频轨道 (OpenGL)";
    }
}

void WebRTCVideoRenderer::clearVideoTrack()
{
    if (m_track) {
        m_track->RemoveSink(this);
        m_track = nullptr;
    }
    QMutexLocker lock(&m_frameMutex);
    m_pendingFrame.valid = false;
    if (m_hasVideo) {
        m_hasVideo = false;
        Q_EMIT hasVideoChanged();
    }
    update();
}
