#include "webrtc_video_renderer.h"
#include "latency_trace.h"

#include <QByteArray>
#include <QDebug>
#include <QMatrix4x4>
#include <QMutexLocker>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QQuickWindow>
#include <QSGRenderNode>
#include <QSGRendererInterface>
#include <QSurfaceFormat>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <vector>

#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif

namespace {

struct PlaneTextureCache {
    int width = 0;
    int height = 0;
};

bool supportsUnpackRowLength()
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

const uint8_t *prepareTightPlaneIfNeeded(const uint8_t *src,
                                         int srcStrideBytes,
                                         int planeWidth,
                                         int planeHeight,
                                         int bytesPerPixel,
                                         bool canUseUnpackRowLength,
                                         std::vector<uint8_t> &tightBuffer)
{
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

void uploadPlane(QOpenGLExtraFunctions &gl,
                 GLenum textureUnit,
                 GLuint textureId,
                 int planeWidth,
                 int planeHeight,
                 GLenum internalFormat,
                 GLenum format,
                 int bytesPerPixel,
                 const uint8_t *planeData,
                 int strideBytes,
                 bool canUseUnpackRowLength,
                 PlaneTextureCache &cache,
                 std::vector<uint8_t> &tightBuffer)
{
    gl.glActiveTexture(textureUnit);
    gl.glBindTexture(GL_TEXTURE_2D, textureId);

    const bool sameSize = cache.width == planeWidth && cache.height == planeHeight;
    if (!sameSize) {
        cache.width = planeWidth;
        cache.height = planeHeight;
        gl.glTexImage2D(GL_TEXTURE_2D,
                        0,
                        internalFormat,
                        planeWidth,
                        planeHeight,
                        0,
                        format,
                        GL_UNSIGNED_BYTE,
                        nullptr);
    }

    gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    const uint8_t *uploadData = planeData;
    int uploadStrideBytes = strideBytes;
    if (!canUseUnpackRowLength) {
        uploadData = prepareTightPlaneIfNeeded(planeData,
                                               strideBytes,
                                               planeWidth,
                                               planeHeight,
                                               bytesPerPixel,
                                               false,
                                               tightBuffer);
        uploadStrideBytes = planeWidth * bytesPerPixel;
    }

    if (canUseUnpackRowLength) {
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, uploadStrideBytes / bytesPerPixel);
    }
    gl.glTexSubImage2D(GL_TEXTURE_2D,
                       0,
                       0,
                       0,
                       planeWidth,
                       planeHeight,
                       format,
                       GL_UNSIGNED_BYTE,
                       uploadData);
    if (canUseUnpackRowLength) {
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
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

class WebRTCVideoRenderNode : public QSGRenderNode, protected QOpenGLExtraFunctions
{
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

        librflow_video_frame_t frame = nullptr;
        quint32 frameId = 0;
        if (!item->takeFrame(frame, frameId) || !frame) {
            if (!item->m_hasVideo) {
                m_hasData = false;
                clearHeldFrame();
            }
            m_haveFrameId = false;
            return;
        }

        clearHeldFrame();
        m_heldFrame = frame;
        m_currentFrameId = frameId;
        m_haveFrameId = true;
        demo::latency_trace::recordSync(frameId);
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

        if (m_heldFrame) {
            uploadHeldFrame();
        }

        if (!m_hasData) {
            return;
        }

        QMatrix4x4 mvp = *state->projectionMatrix();
        if (const QMatrix4x4 *nodeMatrix = matrix()) {
            mvp *= *nodeMatrix;
        }

        QOpenGLShaderProgram *program = nullptr;
        if (m_frameFormat == RFLOW_CODEC_NV12 && m_nv12Program.isLinked()) {
            program = &m_nv12Program;
        } else if (m_i420Program.isLinked()) {
            program = &m_i420Program;
        }
        if (!program) {
            return;
        }

        static const GLfloat kTexCoords[] = {
            0.0f, 1.0f, 1.0f, 1.0f,
            0.0f, 0.0f, 1.0f, 0.0f
        };

        program->bind();
        program->setUniformValue("qt_Matrix", mvp);
        program->setAttributeArray(0, GL_FLOAT, m_vertices, 2);
        program->enableAttributeArray(0);
        program->setAttributeArray(1, GL_FLOAT, kTexCoords, 2);
        program->enableAttributeArray(1);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_texY);
        program->setUniformValue("tex_y", 0);

        if (m_frameFormat == RFLOW_CODEC_NV12) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, m_texUV);
            program->setUniformValue("tex_uv", 1);
        } else {
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

        program->disableAttributeArray(0);
        program->disableAttributeArray(1);
        program->release();

        if (m_haveFrameId) {
            demo::latency_trace::recordRender(m_currentFrameId);
            m_haveFrameId = false;
        }
    }

    void releaseResources() override
    {
        destroyGlResources();
    }

private:
    bool ensureInitialized()
    {
        if (m_glInitialized) {
            return true;
        }

        if (!QOpenGLContext::currentContext()) {
            return false;
        }

        initializeOpenGLFunctions();
        m_canUseUnpackRowLength = supportsUnpackRowLength();
        initShaders();
        initTextures();
        m_glInitialized = true;
        return true;
    }

    void initShaders()
    {
        static const char *vertexShader =
            "uniform mat4 qt_Matrix;"
            "attribute vec4 vertexIn;"
            "attribute vec2 textureIn;"
            "varying vec2 textureOut;"
            "void main() {"
            "  gl_Position = qt_Matrix * vertexIn;"
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

        initProgram(m_i420Program, vertexShader, i420FragmentShader);
        initProgram(m_nv12Program, vertexShader, nv12FragmentShader);
    }

    void initTextures()
    {
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
    }

    void destroyGlResources()
    {
        clearHeldFrame();
        if (!m_glInitialized || !QOpenGLContext::currentContext()) {
            return;
        }

        GLuint texIds[] = {m_texY, m_texU, m_texV, m_texUV};
        glDeleteTextures(4, texIds);
        m_texY = 0;
        m_texU = 0;
        m_texV = 0;
        m_texUV = 0;
        m_glInitialized = false;
    }

    void clearHeldFrame()
    {
        if (m_heldFrame) {
            librflow_video_frame_release(m_heldFrame);
            m_heldFrame = nullptr;
        }
    }

    void resetPlaneCaches()
    {
        m_cacheY = {};
        m_cacheU = {};
        m_cacheV = {};
        m_cacheUV = {};
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
                    GL_TEXTURE0,
                    m_texY,
                    static_cast<int>(widthY),
                    static_cast<int>(heightY),
                    GL_LUMINANCE,
                    GL_LUMINANCE,
                    1,
                    planeY,
                    static_cast<int>(strideY),
                    m_canUseUnpackRowLength,
                    m_cacheY,
                    m_tightY);
        uploadPlane(*this,
                    GL_TEXTURE1,
                    m_texU,
                    static_cast<int>(widthU),
                    static_cast<int>(heightU),
                    GL_LUMINANCE,
                    GL_LUMINANCE,
                    1,
                    planeU,
                    static_cast<int>(strideU),
                    m_canUseUnpackRowLength,
                    m_cacheU,
                    m_tightU);
        uploadPlane(*this,
                    GL_TEXTURE2,
                    m_texV,
                    static_cast<int>(widthV),
                    static_cast<int>(heightV),
                    GL_LUMINANCE,
                    GL_LUMINANCE,
                    1,
                    planeV,
                    static_cast<int>(strideV),
                    m_canUseUnpackRowLength,
                    m_cacheV,
                    m_tightV);
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
                    GL_TEXTURE0,
                    m_texY,
                    static_cast<int>(widthY),
                    static_cast<int>(heightY),
                    GL_LUMINANCE,
                    GL_LUMINANCE,
                    1,
                    planeY,
                    static_cast<int>(strideY),
                    m_canUseUnpackRowLength,
                    m_cacheY,
                    m_tightY);
        uploadPlane(*this,
                    GL_TEXTURE1,
                    m_texUV,
                    static_cast<int>(widthUV),
                    static_cast<int>(heightUV),
                    GL_LUMINANCE_ALPHA,
                    GL_LUMINANCE_ALPHA,
                    2,
                    planeUV,
                    static_cast<int>(strideUV),
                    m_canUseUnpackRowLength,
                    m_cacheUV,
                    m_tightUV);
        return true;
    }

    void uploadHeldFrame()
    {
        if (!m_heldFrame) {
            m_hasData = false;
            return;
        }

        const rflow_codec_t codec = librflow_video_frame_get_codec(m_heldFrame);
        bool uploaded = false;

        if (codec != m_frameFormat) {
            resetPlaneCaches();
        }

        if (codec == RFLOW_CODEC_I420) {
            uploaded = uploadI420(m_heldFrame);
            if (uploaded) {
                m_frameFormat = RFLOW_CODEC_I420;
            }
        } else if (codec == RFLOW_CODEC_NV12) {
            uploaded = uploadNV12(m_heldFrame);
            if (uploaded) {
                m_frameFormat = RFLOW_CODEC_NV12;
            }
        }

        m_hasData = uploaded;
        clearHeldFrame();
    }

    WebRTCVideoRenderer *m_videoItem = nullptr;
    QRectF m_rect;
    GLfloat m_vertices[8] = {0};

    bool m_glInitialized = false;
    bool m_canUseUnpackRowLength = false;
    bool m_hasData = false;
    rflow_codec_t m_frameFormat = RFLOW_CODEC_I420;

    QOpenGLShaderProgram m_i420Program;
    QOpenGLShaderProgram m_nv12Program;
    GLuint m_texY = 0;
    GLuint m_texU = 0;
    GLuint m_texV = 0;
    GLuint m_texUV = 0;

    PlaneTextureCache m_cacheY;
    PlaneTextureCache m_cacheU;
    PlaneTextureCache m_cacheV;
    PlaneTextureCache m_cacheUV;
    std::vector<uint8_t> m_tightY;
    std::vector<uint8_t> m_tightU;
    std::vector<uint8_t> m_tightV;
    std::vector<uint8_t> m_tightUV;

    librflow_video_frame_t m_heldFrame = nullptr;
    quint32 m_currentFrameId = 0;
    bool m_haveFrameId = false;
};

}  // namespace

WebRTCVideoRenderer::WebRTCVideoRenderer(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
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

QSGNode *WebRTCVideoRenderer::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
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

    auto *node = static_cast<WebRTCVideoRenderNode *>(oldNode);
    if (!node) {
        node = new WebRTCVideoRenderNode();
    }
    node->sync(this);
    return node;
}

void WebRTCVideoRenderer::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        update();
    }
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
