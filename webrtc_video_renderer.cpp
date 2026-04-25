#include "webrtc_video_renderer.h"
#include "latency_trace.h"

#include <QMutexLocker>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>

#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif

namespace {

void uploadPlane(QOpenGLExtraFunctions &gl,
                 bool useUnpackRowLength,
                 GLenum textureUnit,
                 GLuint textureId,
                 GLenum format,
                 int bytesPerPixel,
                 int planeWidth,
                 int planeHeight,
                 const uint8_t *data,
                 int strideBytes)
{
    gl.glActiveTexture(textureUnit);
    gl.glBindTexture(GL_TEXTURE_2D, textureId);

    const int packedStride = planeWidth * bytesPerPixel;
    if (strideBytes == packedStride) {
        gl.glTexImage2D(GL_TEXTURE_2D,
                        0,
                        format,
                        planeWidth,
                        planeHeight,
                        0,
                        format,
                        GL_UNSIGNED_BYTE,
                        data);
        return;
    }

    if (useUnpackRowLength && strideBytes % bytesPerPixel == 0) {
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, strideBytes / bytesPerPixel);
        gl.glTexImage2D(GL_TEXTURE_2D,
                        0,
                        format,
                        planeWidth,
                        planeHeight,
                        0,
                        format,
                        GL_UNSIGNED_BYTE,
                        data);
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        return;
    }

    gl.glTexImage2D(GL_TEXTURE_2D,
                    0,
                    format,
                    planeWidth,
                    planeHeight,
                    0,
                    format,
                    GL_UNSIGNED_BYTE,
                    nullptr);
    for (int row = 0; row < planeHeight; ++row) {
        gl.glTexSubImage2D(GL_TEXTURE_2D,
                           0,
                           0,
                           row,
                           planeWidth,
                           1,
                           format,
                           GL_UNSIGNED_BYTE,
                           data + row * strideBytes);
    }
}

bool initProgram(QOpenGLShaderProgram &program, const char *vertexShader, const char *fragmentShader)
{
    if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader)) {
        return false;
    }
    if (!program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader)) {
        return false;
    }
    program.bindAttributeLocation("vertexIn", 0);
    program.bindAttributeLocation("textureIn", 1);
    return program.link();
}

class WebRTCGLRendererImpl : public QQuickFramebufferObject::Renderer, protected QOpenGLExtraFunctions
{
public:
    WebRTCGLRendererImpl()
    {
        initializeOpenGLFunctions();
        initShaders();
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

        static const GLfloat verts[] = {
            -1.0f, -1.0f, 1.0f, -1.0f,
            -1.0f,  1.0f, 1.0f,  1.0f
        };
        static const GLfloat texCoords[] = {
            0.0f, 1.0f, 1.0f, 1.0f,
            0.0f, 0.0f, 1.0f, 0.0f
        };

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_texY);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_texU);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_texV);

        QOpenGLShaderProgram *program = nullptr;
        if (m_frameFormat == RFLOW_CODEC_NV12 && m_nv12Program.isLinked()) {
            program = &m_nv12Program;
        } else if (m_i420Program.isLinked()) {
            program = &m_i420Program;
        }
        if (!program) {
            glClear(GL_COLOR_BUFFER_BIT);
            return;
        }

        program->bind();
        program->setAttributeArray(0, GL_FLOAT, verts, 2);
        program->enableAttributeArray(0);
        program->setAttributeArray(1, GL_FLOAT, texCoords, 2);
        program->enableAttributeArray(1);
        program->setUniformValue("tex_y", 0);
        program->setUniformValue("tex_u", 1);
        program->setUniformValue("tex_v", 2);

        glDisable(GL_DEPTH_TEST);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        program->disableAttributeArray(0);
        program->disableAttributeArray(1);
        program->release();

        if (m_hasFrameId) {
            demo::latency_trace::recordRender(m_currentFrameId);
            m_hasFrameId = false;
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

        librflow_video_frame_t frame = nullptr;
        quint32 frameId = 0;
        if (!rendererItem->takeFrame(frame, frameId) || !frame) {
            return;
        }

        demo::latency_trace::recordSync(frameId);
        m_currentFrameId = frameId;
        m_hasFrameId = true;

        const rflow_codec_t codec = librflow_video_frame_get_codec(frame);
        const uint32_t planeCount = librflow_video_frame_get_plane_count(frame);
        bool uploaded = false;

        if (codec == RFLOW_CODEC_I420 && planeCount >= 3) {
            uploaded = uploadI420(frame);
            if (uploaded) {
                m_frameFormat = RFLOW_CODEC_I420;
            }
        } else if (codec == RFLOW_CODEC_NV12 && planeCount >= 2) {
            uploaded = uploadNV12(frame);
            if (uploaded) {
                m_frameFormat = RFLOW_CODEC_NV12;
            }
        }

        m_hasData = uploaded;
        librflow_video_frame_release(frame);
    }

private:
    void initShaders()
    {
        static const char *vertexShader =
            "attribute vec4 vertexIn;"
            "attribute vec2 textureIn;"
            "varying vec2 textureOut;"
            "void main() {"
            "  gl_Position = vertexIn;"
            "  textureOut = textureIn;"
            "}";

        static const char *i420FragmentShader =
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

        static const char *nv12FragmentShader =
            "varying mediump vec2 textureOut;"
            "uniform sampler2D tex_y;"
            "uniform sampler2D tex_u;"
            "uniform sampler2D tex_v;"
            "void main() {"
            "  mediump float y = texture2D(tex_y, textureOut).r;"
            "  mediump vec2 uv = texture2D(tex_u, textureOut).ra - vec2(0.5, 0.5);"
            "  mediump vec3 yuv = vec3(y, uv.x, uv.y);"
            "  mediump vec3 rgb = mat3(1.0, 1.0, 1.0,"
            "                          0.0, -0.34413, 1.772,"
            "                          1.402, -0.71414, 0.0) * yuv;"
            "  gl_FragColor = vec4(rgb, 1.0);"
            "}";

        initProgram(m_i420Program, vertexShader, i420FragmentShader);
        initProgram(m_nv12Program, vertexShader, nv12FragmentShader);
    }

    bool uploadI420(librflow_video_frame_t frame)
    {
        const uint8_t *planeY = librflow_video_frame_get_plane_data(frame, 0);
        const uint8_t *planeU = librflow_video_frame_get_plane_data(frame, 1);
        const uint8_t *planeV = librflow_video_frame_get_plane_data(frame, 2);
        const uint32_t strideY = librflow_video_frame_get_plane_stride(frame, 0);
        const uint32_t strideU = librflow_video_frame_get_plane_stride(frame, 1);
        const uint32_t strideV = librflow_video_frame_get_plane_stride(frame, 2);
        const uint32_t widthY = librflow_video_frame_get_plane_width(frame, 0);
        const uint32_t heightY = librflow_video_frame_get_plane_height(frame, 0);
        const uint32_t widthU = librflow_video_frame_get_plane_width(frame, 1);
        const uint32_t heightU = librflow_video_frame_get_plane_height(frame, 1);
        const uint32_t widthV = librflow_video_frame_get_plane_width(frame, 2);
        const uint32_t heightV = librflow_video_frame_get_plane_height(frame, 2);

        if (!planeY || !planeU || !planeV || widthY == 0 || heightY == 0 || widthU == 0 || heightU == 0 ||
            widthV == 0 || heightV == 0 || strideY < widthY || strideU < widthU || strideV < widthV) {
            return false;
        }

        uploadPlane(*this,
                    m_useUnpackRowLength,
                    GL_TEXTURE0,
                    m_texY,
                    GL_LUMINANCE,
                    1,
                    static_cast<int>(widthY),
                    static_cast<int>(heightY),
                    planeY,
                    static_cast<int>(strideY));
        uploadPlane(*this,
                    m_useUnpackRowLength,
                    GL_TEXTURE1,
                    m_texU,
                    GL_LUMINANCE,
                    1,
                    static_cast<int>(widthU),
                    static_cast<int>(heightU),
                    planeU,
                    static_cast<int>(strideU));
        uploadPlane(*this,
                    m_useUnpackRowLength,
                    GL_TEXTURE2,
                    m_texV,
                    GL_LUMINANCE,
                    1,
                    static_cast<int>(widthV),
                    static_cast<int>(heightV),
                    planeV,
                    static_cast<int>(strideV));
        return true;
    }

    bool uploadNV12(librflow_video_frame_t frame)
    {
        const uint8_t *planeY = librflow_video_frame_get_plane_data(frame, 0);
        const uint8_t *planeUV = librflow_video_frame_get_plane_data(frame, 1);
        const uint32_t strideY = librflow_video_frame_get_plane_stride(frame, 0);
        const uint32_t strideUV = librflow_video_frame_get_plane_stride(frame, 1);
        const uint32_t widthY = librflow_video_frame_get_plane_width(frame, 0);
        const uint32_t heightY = librflow_video_frame_get_plane_height(frame, 0);
        const uint32_t widthUV = librflow_video_frame_get_plane_width(frame, 1);
        const uint32_t heightUV = librflow_video_frame_get_plane_height(frame, 1);

        if (!planeY || !planeUV || widthY == 0 || heightY == 0 || widthUV == 0 || heightUV == 0 ||
            strideY < widthY || strideUV < widthUV * 2) {
            return false;
        }

        uploadPlane(*this,
                    m_useUnpackRowLength,
                    GL_TEXTURE0,
                    m_texY,
                    GL_LUMINANCE,
                    1,
                    static_cast<int>(widthY),
                    static_cast<int>(heightY),
                    planeY,
                    static_cast<int>(strideY));
        uploadPlane(*this,
                    m_useUnpackRowLength,
                    GL_TEXTURE1,
                    m_texU,
                    GL_LUMINANCE_ALPHA,
                    2,
                    static_cast<int>(widthUV),
                    static_cast<int>(heightUV),
                    planeUV,
                    static_cast<int>(strideUV));
        return true;
    }

    void initTextures()
    {
        GLuint texIds[3] = {0, 0, 0};
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

    QOpenGLShaderProgram m_i420Program;
    QOpenGLShaderProgram m_nv12Program;
    GLuint m_texY = 0;
    GLuint m_texU = 0;
    GLuint m_texV = 0;
    bool m_hasData = false;
    rflow_codec_t m_frameFormat = RFLOW_CODEC_I420;
    quint32 m_currentFrameId = 0;
    bool m_hasFrameId = false;
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

void WebRTCVideoRenderer::presentFrame(librflow_video_frame_t frame)
{
    if (!frame) {
        return;
    }

    const uint32_t width = librflow_video_frame_get_width(frame);
    const uint32_t height = librflow_video_frame_get_height(frame);
    const quint32 frameId = librflow_video_frame_get_seq(frame);
    if (width == 0 || height == 0) {
        librflow_video_frame_release(frame);
        return;
    }

    bool highlightChanged = false;
    {
        QMutexLocker locker(&m_frameMutex);
        if (m_pendingFrame) {
            librflow_video_frame_release(m_pendingFrame);
        }
        m_pendingFrame = frame;
        m_pendingValid = true;
        if (m_highlightFrameId != static_cast<int>(frameId) || m_frameIdFromTracking) {
            m_highlightFrameId = static_cast<int>(frameId);
            m_frameIdFromTracking = false;
            highlightChanged = true;
        }
    }

    demo::latency_trace::recordPresent(frameId, static_cast<int>(width), static_cast<int>(height));

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
        if (m_pendingFrame) {
            librflow_video_frame_release(m_pendingFrame);
            m_pendingFrame = nullptr;
        }
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
                                         : QStringLiteral("N/A");
    return QStringLiteral("sample frame=%1 | total=%2 ms | UI queue=%3 ms")
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

bool WebRTCVideoRenderer::takeFrame(librflow_video_frame_t &outFrame, quint32 &outFrameId)
{
    QMutexLocker locker(&m_frameMutex);
    if (!m_pendingValid || !m_pendingFrame) {
        return false;
    }

    outFrame = m_pendingFrame;
    m_pendingFrame = nullptr;
    outFrameId = static_cast<quint32>(m_highlightFrameId >= 0 ? m_highlightFrameId : 0);
    m_pendingValid = false;
    return true;
}
