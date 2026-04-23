#include "webrtc_video_renderer.h"

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QMutexLocker>

#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif

namespace {

void uploadLuminancePlane(QOpenGLExtraFunctions &gl,
                          bool useUnpackRowLength,
                          GLenum textureUnit,
                          GLuint textureId,
                          int planeWidth,
                          int planeHeight,
                          const uint8_t *data,
                          int strideBytes)
{
    gl.glActiveTexture(textureUnit);
    gl.glBindTexture(GL_TEXTURE_2D, textureId);

    if (strideBytes == planeWidth) {
        gl.glTexImage2D(GL_TEXTURE_2D,
                        0,
                        GL_LUMINANCE,
                        planeWidth,
                        planeHeight,
                        0,
                        GL_LUMINANCE,
                        GL_UNSIGNED_BYTE,
                        data);
        return;
    }

    if (useUnpackRowLength) {
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, strideBytes);
        gl.glTexImage2D(GL_TEXTURE_2D,
                        0,
                        GL_LUMINANCE,
                        planeWidth,
                        planeHeight,
                        0,
                        GL_LUMINANCE,
                        GL_UNSIGNED_BYTE,
                        data);
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        return;
    }

    gl.glTexImage2D(GL_TEXTURE_2D,
                    0,
                    GL_LUMINANCE,
                    planeWidth,
                    planeHeight,
                    0,
                    GL_LUMINANCE,
                    GL_UNSIGNED_BYTE,
                    nullptr);
    for (int row = 0; row < planeHeight; ++row) {
        gl.glTexSubImage2D(GL_TEXTURE_2D,
                           0,
                           0,
                           row,
                           planeWidth,
                           1,
                           GL_LUMINANCE,
                           GL_UNSIGNED_BYTE,
                           data + row * strideBytes);
    }
}

class WebRTCGLRendererImpl : public QQuickFramebufferObject::Renderer, protected QOpenGLExtraFunctions
{
public:
    WebRTCGLRendererImpl()
    {
        initializeOpenGLFunctions();
        initShader();
        initTextures();
    }

    ~WebRTCGLRendererImpl() override
    {
        glDeleteTextures(1, &m_texY);
        glDeleteTextures(1, &m_texU);
        glDeleteTextures(1, &m_texV);
    }

    void render() override
    {
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
    }

    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override
    {
        QOpenGLFramebufferObjectFormat fmt;
        fmt.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        return new QOpenGLFramebufferObject(size, fmt);
    }

    void synchronize(QQuickFramebufferObject *item) override
    {
        auto *rendererItem = qobject_cast<WebRTCVideoRenderer *>(item);
        if (!rendererItem) {
            return;
        }

        if (!m_uploadCapsChecked) {
            m_uploadCapsChecked = true;
            if (QOpenGLContext *ctx = QOpenGLContext::currentContext()) {
                m_useUnpackRowLength = ctx->format().majorVersion() >= 3;
            }
        }

        QByteArray frame;
        int width = 0;
        int height = 0;
        if (!rendererItem->takeFrame(frame, width, height) || frame.isEmpty() || width <= 0 || height <= 0) {
            return;
        }

        const int ySize = width * height;
        const int chromaWidth = width / 2;
        const int chromaHeight = height / 2;
        const uint8_t *data = reinterpret_cast<const uint8_t *>(frame.constData());
        const uint8_t *planeY = data;
        const uint8_t *planeU = planeY + ySize;
        const uint8_t *planeV = planeU + chromaWidth * chromaHeight;

        uploadLuminancePlane(*this, m_useUnpackRowLength, GL_TEXTURE0, m_texY, width, height, planeY, width);
        uploadLuminancePlane(*this,
                             m_useUnpackRowLength,
                             GL_TEXTURE1,
                             m_texU,
                             chromaWidth,
                             chromaHeight,
                             planeU,
                             chromaWidth);
        uploadLuminancePlane(*this,
                             m_useUnpackRowLength,
                             GL_TEXTURE2,
                             m_texV,
                             chromaWidth,
                             chromaHeight,
                             planeV,
                             chromaWidth);
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

        for (GLuint texId : texIds) {
            glBindTexture(GL_TEXTURE_2D, texId);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }

    QOpenGLShaderProgram m_program;
    GLuint m_texY = 0;
    GLuint m_texU = 0;
    GLuint m_texV = 0;
    bool m_hasData = false;
    bool m_uploadCapsChecked = false;
    bool m_useUnpackRowLength = false;
};

}  // namespace

WebRTCVideoRenderer::WebRTCVideoRenderer(QQuickItem *parent)
    : QQuickFramebufferObject(parent)
{
    setMirrorVertically(true);
}

WebRTCVideoRenderer::~WebRTCVideoRenderer()
{
    clearVideoTrack();
}

void WebRTCVideoRenderer::presentFrame(const QByteArray &i420, int width, int height, quint32 frameId)
{
    if (width <= 0 || height <= 0) {
        return;
    }

    const int expectedSize = width * height * 3 / 2;
    if (i420.size() < expectedSize) {
        return;
    }

    bool highlightChanged = false;
    {
        QMutexLocker locker(&m_frameMutex);
        m_pendingFrame = i420.left(expectedSize);
        m_pendingWidth = width;
        m_pendingHeight = height;
        m_pendingValid = true;
        if (m_highlightFrameId != static_cast<int>(frameId) || m_frameIdFromTracking) {
            m_highlightFrameId = static_cast<int>(frameId);
            m_frameIdFromTracking = false;
            highlightChanged = true;
        }
    }

    if (!m_hasVideo) {
        m_hasVideo = true;
        Q_EMIT hasVideoChanged();
    }
    if (highlightChanged) {
        Q_EMIT highlightFrameIdChanged();
    }
    update();
}

void WebRTCVideoRenderer::clearVideoTrack()
{
    bool hadVideo = m_hasVideo;
    {
        QMutexLocker locker(&m_frameMutex);
        m_pendingFrame.clear();
        m_pendingWidth = 0;
        m_pendingHeight = 0;
        m_pendingValid = false;
        m_highlightFrameId = -1;
        m_frameIdFromTracking = false;
    }

    m_hasVideo = false;
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

int WebRTCVideoRenderer::highlightFrameId() const
{
    QMutexLocker locker(&m_frameMutex);
    return m_highlightFrameId;
}

bool WebRTCVideoRenderer::frameIdFromTracking() const
{
    QMutexLocker locker(&m_frameMutex);
    return m_frameIdFromTracking;
}

void WebRTCVideoRenderer::setTraceTargetFrameId(int id)
{
    if (m_traceTargetFrameId == id) {
        return;
    }
    m_traceTargetFrameId = id;
    Q_EMIT traceTargetFrameIdChanged();
}

QString WebRTCVideoRenderer::sampledPipelineLine() const
{
    if (!m_hasSampledPipelineUi) {
        return {};
    }
    const QString decodeToRender =
        m_sampledDecodeToRenderMs >= 0.0 ? QString::number(m_sampledDecodeToRenderMs, 'f', 3)
                                         : QStringLiteral("—");
    return QStringLiteral("采样 frame=%1 | 总耗时=%2 ms | UI 排队=%3 ms")
        .arg(m_sampledHighlightFrameId)
        .arg(decodeToRender)
        .arg(m_sampledWallOnFrameToRenderMs, 0, 'f', 3);
}

void WebRTCVideoRenderer::applySampledPipelineUi(int glTraceFrameId,
                                                 double decodeToRenderTotalMs,
                                                 double wallOnFrameToRenderMs)
{
    m_sampledHighlightFrameId = glTraceFrameId;
    m_sampledDecodeToRenderMs = decodeToRenderTotalMs;
    m_sampledWallOnFrameToRenderMs = wallOnFrameToRenderMs;
    m_hasSampledPipelineUi = true;
    Q_EMIT sampledPipelineStatsChanged();
}

QQuickFramebufferObject::Renderer *WebRTCVideoRenderer::createRenderer() const
{
    return new WebRTCGLRendererImpl();
}

bool WebRTCVideoRenderer::takeFrame(QByteArray &outFrame, int &outWidth, int &outHeight)
{
    QMutexLocker locker(&m_frameMutex);
    if (!m_pendingValid || m_pendingFrame.isEmpty()) {
        return false;
    }
    outFrame = m_pendingFrame;
    outWidth = m_pendingWidth;
    outHeight = m_pendingHeight;
    m_pendingFrame.clear();
    m_pendingValid = false;
    return true;
}
