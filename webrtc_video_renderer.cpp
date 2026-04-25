#include "webrtc_video_renderer.h"
#include "latency_trace.h"

#include <QByteArray>
#include <QDebug>
#include <QElapsedTimer>
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

#ifdef Q_OS_ANDROID
#include <GLES2/gl2ext.h>
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
#ifndef GL_LUMINANCE_ALPHA
#define GL_LUMINANCE_ALPHA 0x190A
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

enum class UploadPathKind {
    None,
    I420,
    NV12,
};

enum class NativeFramePath {
    None,
    ExternalOes,
};

QString backendToString(rflow_video_frame_backend_t backend)
{
    switch (backend) {
    case RFLOW_VIDEO_FRAME_BACKEND_CPU_PLANAR:
        return QStringLiteral("CPU_PLANAR");
    case RFLOW_VIDEO_FRAME_BACKEND_GPU_EXTERNAL:
        return QStringLiteral("GPU_EXTERNAL");
    case RFLOW_VIDEO_FRAME_BACKEND_HARDWARE_BUFFER:
        return QStringLiteral("HARDWARE_BUFFER");
    case RFLOW_VIDEO_FRAME_BACKEND_UNKNOWN:
    default:
        return QStringLiteral("UNKNOWN");
    }
}

QString nativeHandleToString(rflow_native_handle_type_t handleType)
{
    switch (handleType) {
    case RFLOW_NATIVE_HANDLE_ANDROID_OES_TEXTURE:
        return QStringLiteral("ANDROID_OES_TEXTURE");
    case RFLOW_NATIVE_HANDLE_ANDROID_HARDWARE_BUFFER:
        return QStringLiteral("ANDROID_HARDWARE_BUFFER");
    case RFLOW_NATIVE_HANDLE_NONE:
    default:
        return QStringLiteral("NONE");
    }
}

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

bool supportsPixelUnpackBuffer()
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

bool supportsExternalOesSampling()
{
    QOpenGLContext *const ctx = QOpenGLContext::currentContext();
    if (!ctx || !ctx->isOpenGLES()) {
        return false;
    }
    return ctx->hasExtension(QByteArrayLiteral("GL_OES_EGL_image_external"));
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

void resetPlaneUploadPbo(PlaneUploadPbo &pbo)
{
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

size_t copyPlaneForUpload(uint8_t *dst,
                          const uint8_t *src,
                          int srcStrideBytes,
                          int planeWidth,
                          int planeHeight,
                          int bytesPerPixel,
                          bool canUseUnpackRowLength)
{
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

bool uploadPlaneFromReadyPbo(QOpenGLExtraFunctions &gl,
                             GLenum textureUnit,
                             GLuint textureId,
                             bool canUseUnpackRowLength,
                             PlaneTextureCache &cache,
                             PlaneUploadPbo &pbo)
{
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
        gl.glTexImage2D(GL_TEXTURE_2D,
                        0,
                        pbo.readyInternalFormat,
                        pbo.readyWidth,
                        pbo.readyHeight,
                        0,
                        pbo.readyFormat,
                        GL_UNSIGNED_BYTE,
                        nullptr);
    }

    gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (canUseUnpackRowLength) {
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, pbo.readyStrideBytes / pbo.readyBytesPerPixel);
    }
    gl.glTexSubImage2D(GL_TEXTURE_2D,
                       0,
                       0,
                       0,
                       pbo.readyWidth,
                       pbo.readyHeight,
                       pbo.readyFormat,
                       GL_UNSIGNED_BYTE,
                       nullptr);
    if (canUseUnpackRowLength) {
        gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    return true;
}

bool stagePlaneIntoPbo(QOpenGLExtraFunctions &gl,
                       int planeWidth,
                       int planeHeight,
                       GLenum internalFormat,
                       GLenum format,
                       int bytesPerPixel,
                       const uint8_t *planeData,
                       int srcStrideBytes,
                       bool canUseUnpackRowLength,
                       PlaneUploadPbo &pbo)
{
    if (!pbo.ids[0] || !pbo.ids[1]) {
        return false;
    }

    const int index = pbo.nextStageIndex;
    pbo.nextStageIndex = (index + 1) % 2;

    const size_t uploadBytes =
        (canUseUnpackRowLength && srcStrideBytes != planeWidth * bytesPerPixel)
            ? static_cast<size_t>(srcStrideBytes) * static_cast<size_t>(planeHeight)
            : static_cast<size_t>(planeWidth * bytesPerPixel) * static_cast<size_t>(planeHeight);

    gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo.ids[index]);
    gl.glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(uploadBytes), nullptr, GL_STREAM_DRAW);

    void *mapped = gl.glMapBufferRange(GL_PIXEL_UNPACK_BUFFER,
                                       0,
                                       static_cast<GLsizeiptr>(uploadBytes),
                                       GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    if (!mapped) {
        gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        return false;
    }

    copyPlaneForUpload(static_cast<uint8_t *>(mapped),
                       planeData,
                       srcStrideBytes,
                       planeWidth,
                       planeHeight,
                       bytesPerPixel,
                       canUseUnpackRowLength);
    if (gl.glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER) == GL_FALSE) {
        gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        return false;
    }

    gl.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    pbo.readyIndex = index;
    pbo.readyWidth = planeWidth;
    pbo.readyHeight = planeHeight;
    pbo.readyStrideBytes = srcStrideBytes;
    pbo.readyInternalFormat = internalFormat;
    pbo.readyFormat = format;
    pbo.readyBytesPerPixel = bytesPerPixel;
    pbo.ready = true;
    return true;
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

        QElapsedTimer drawTimer;
        drawTimer.start();

        QMatrix4x4 mvp = *state->projectionMatrix();
        if (const QMatrix4x4 *nodeMatrix = matrix()) {
            mvp *= *nodeMatrix;
        }

        QOpenGLShaderProgram *program = nullptr;
        const bool useNativeOes = m_nativePath == NativeFramePath::ExternalOes && m_oesProgram.isLinked();
        if (useNativeOes) {
            program = &m_oesProgram;
        } else if (m_frameFormat == RFLOW_CODEC_NV12 && m_nv12Program.isLinked()) {
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

        if (useNativeOes) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_nativeTextureId);
            program->setUniformValue("tex", 0);
        } else {
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
        }

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        program->disableAttributeArray(0);
        program->disableAttributeArray(1);
        program->release();

        const qint64 drawUs = drawTimer.nsecsElapsed() / 1000;
        ++m_renderedFrames;
        if (m_uploadStatsValidForLastFrame &&
            (m_renderedFrames <= 5 || (m_renderedFrames % 120) == 0)) {
            qInfo().noquote()
                << QStringLiteral("[RenderPerf] frame=%1 upload=%2 ms draw=%3 ms total=%4 ms path=%5")
                       .arg(m_currentFrameId)
                       .arg(m_lastUploadUs / 1000.0, 0, 'f', 3)
                       .arg(drawUs / 1000.0, 0, 'f', 3)
                       .arg((m_lastUploadUs + drawUs) / 1000.0, 0, 'f', 3)
                       .arg(useNativeOes ? QStringLiteral("OES")
                                         : (m_frameFormat == RFLOW_CODEC_NV12 ? QStringLiteral("NV12")
                                                                              : QStringLiteral("I420")));
        }

        releaseSamplingAfterDraw();

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
        m_canUsePixelUnpackBuffer = supportsPixelUnpackBuffer();
        m_canUseExternalOesSampling = supportsExternalOesSampling();
        initShaders();
        initTextures();
        initUploadPbos();
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

        static const char *oesFragmentShader =
            "#extension GL_OES_EGL_image_external : require\n"
            "varying mediump vec2 textureOut;\n"
            "uniform samplerExternalOES tex;\n"
            "void main() {\n"
            "  gl_FragColor = texture2D(tex, textureOut);\n"
            "}\n";

        initProgram(m_i420Program, vertexShader, i420FragmentShader);
        initProgram(m_nv12Program, vertexShader, nv12FragmentShader);
        if (m_canUseExternalOesSampling) {
            initProgram(m_oesProgram, vertexShader, oesFragmentShader);
        }
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

        destroyUploadPbos();
        GLuint texIds[] = {m_texY, m_texU, m_texV, m_texUV};
        glDeleteTextures(4, texIds);
        m_texY = 0;
        m_texU = 0;
        m_texV = 0;
        m_texUV = 0;
        m_nativeTextureId = 0;
        m_glInitialized = false;
    }

    void clearHeldFrame()
    {
        releaseSamplingAfterDraw();
        if (m_heldFrame) {
            librflow_video_frame_release(m_heldFrame);
            m_heldFrame = nullptr;
        }
        resetNativeFrameState();
    }

    void resetPlaneCaches()
    {
        m_cacheY = {};
        m_cacheU = {};
        m_cacheV = {};
        m_cacheUV = {};
    }

    void resetCpuUploadPipeline()
    {
        resetPlaneUploadPbo(m_pboY);
        resetPlaneUploadPbo(m_pboU);
        resetPlaneUploadPbo(m_pboV);
        resetPlaneUploadPbo(m_pboUV);
        resetPlaneCaches();
    }

    void resetNativeFrameState()
    {
        m_nativePath = NativeFramePath::None;
        m_nativeTextureId = 0;
        m_nativePrepareFn = nullptr;
        m_nativePrepareUserdata = nullptr;
    }

    void releaseSamplingAfterDraw()
    {
        if (m_nativeSamplingAcquired && m_heldFrame) {
            librflow_video_frame_release_after_sampling(m_heldFrame);
            m_nativeSamplingAcquired = false;
        }
        m_nativePrepareFn = nullptr;
        m_nativePrepareUserdata = nullptr;
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

    void initUploadPbos()
    {
        if (!m_canUsePixelUnpackBuffer) {
            return;
        }
        glGenBuffers(2, m_pboY.ids);
        glGenBuffers(2, m_pboU.ids);
        glGenBuffers(2, m_pboV.ids);
        glGenBuffers(2, m_pboUV.ids);
    }

    void destroyUploadPbos()
    {
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

    bool uploadPlaneViaPboAsync(GLenum textureUnit,
                                GLuint textureId,
                                int planeWidth,
                                int planeHeight,
                                GLenum internalFormat,
                                GLenum format,
                                int bytesPerPixel,
                                const uint8_t *planeData,
                                int strideBytes,
                                PlaneTextureCache &cache,
                                PlaneUploadPbo &pbo,
                                std::vector<uint8_t> &tightBuffer)
    {
        if (!m_canUsePixelUnpackBuffer || (!pbo.ids[0] && !pbo.ids[1])) {
            uploadPlane(*this,
                        textureUnit,
                        textureId,
                        planeWidth,
                        planeHeight,
                        internalFormat,
                        format,
                        bytesPerPixel,
                        planeData,
                        strideBytes,
                        m_canUseUnpackRowLength,
                        cache,
                        tightBuffer);
            return true;
        }

        const bool uploaded = uploadPlaneFromReadyPbo(*this,
                                                      textureUnit,
                                                      textureId,
                                                      m_canUseUnpackRowLength,
                                                      cache,
                                                      pbo);
        if (!stagePlaneIntoPbo(*this,
                               planeWidth,
                               planeHeight,
                               internalFormat,
                               format,
                               bytesPerPixel,
                               planeData,
                               strideBytes,
                               m_canUseUnpackRowLength,
                               pbo)) {
            resetPlaneUploadPbo(pbo);
            uploadPlane(*this,
                        textureUnit,
                        textureId,
                        planeWidth,
                        planeHeight,
                        internalFormat,
                        format,
                        bytesPerPixel,
                        planeData,
                        strideBytes,
                        m_canUseUnpackRowLength,
                        cache,
                        tightBuffer);
            return true;
        }
        return uploaded;
    }

    bool prepareNativeFrame(librflow_video_frame_t frame,
                            rflow_video_frame_backend_t backend,
                            rflow_native_handle_type_t nativeHandleType)
    {
        if (librflow_video_frame_acquire_for_sampling(frame) != RFLOW_OK) {
            qWarning().noquote()
                << QStringLiteral("[RenderPerf] acquire_for_sampling failed backend=%1 handle=%2")
                       .arg(backendToString(backend))
                       .arg(nativeHandleToString(nativeHandleType));
            return false;
        }
        m_nativeSamplingAcquired = true;

        librflow_gl_prepare_fn prepareFn = nullptr;
        void *prepareUserdata = nullptr;
        const rflow_err_t prepareErr =
            librflow_video_frame_get_gl_prepare_callback(frame, &prepareFn, &prepareUserdata);
        if (prepareErr == RFLOW_OK && prepareFn) {
            prepareFn(prepareUserdata);
            m_nativePrepareFn = prepareFn;
            m_nativePrepareUserdata = prepareUserdata;
        } else if (prepareErr != RFLOW_OK && prepareErr != RFLOW_ERR_NOT_SUPPORT) {
            qWarning().noquote()
                << QStringLiteral("[RenderPerf] get_gl_prepare_callback failed err=%1 backend=%2 handle=%3")
                       .arg(prepareErr)
                       .arg(backendToString(backend))
                       .arg(nativeHandleToString(nativeHandleType));
            releaseSamplingAfterDraw();
            return false;
        }

        if (nativeHandleType == RFLOW_NATIVE_HANDLE_ANDROID_OES_TEXTURE) {
            if (!m_canUseExternalOesSampling || !m_oesProgram.isLinked()) {
                qWarning().noquote()
                    << QStringLiteral("[RenderPerf] OES backend negotiated but OpenGL external texture sampling "
                                      "is unavailable on this context");
                releaseSamplingAfterDraw();
                return false;
            }

            const GLuint textureId = librflow_video_frame_get_oes_texture_id(frame);
            if (textureId == 0) {
                qWarning().noquote()
                    << QStringLiteral("[RenderPerf] OES backend negotiated but texture id is 0");
                releaseSamplingAfterDraw();
                return false;
            }

            m_nativePath = NativeFramePath::ExternalOes;
            m_nativeTextureId = textureId;
            m_lastUploadUs = 0;
            m_uploadStatsValidForLastFrame = true;
            m_hasData = true;
            return true;
        }

        qWarning().noquote()
            << QStringLiteral("[RenderPerf] native frame backend=%1 handle=%2 is not wired into renderer yet")
                   .arg(backendToString(backend))
                   .arg(nativeHandleToString(nativeHandleType));
        releaseSamplingAfterDraw();
        return false;
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

        const bool yUploaded = uploadPlaneViaPboAsync(GL_TEXTURE0,
                                                      m_texY,
                                                      static_cast<int>(widthY),
                                                      static_cast<int>(heightY),
                                                      GL_LUMINANCE,
                                                      GL_LUMINANCE,
                                                      1,
                                                      planeY,
                                                      static_cast<int>(strideY),
                                                      m_cacheY,
                                                      m_pboY,
                                                      m_tightY);
        const bool uUploaded = uploadPlaneViaPboAsync(GL_TEXTURE1,
                                                      m_texU,
                                                      static_cast<int>(widthU),
                                                      static_cast<int>(heightU),
                                                      GL_LUMINANCE,
                                                      GL_LUMINANCE,
                                                      1,
                                                      planeU,
                                                      static_cast<int>(strideU),
                                                      m_cacheU,
                                                      m_pboU,
                                                      m_tightU);
        const bool vUploaded = uploadPlaneViaPboAsync(GL_TEXTURE2,
                                                      m_texV,
                                                      static_cast<int>(widthV),
                                                      static_cast<int>(heightV),
                                                      GL_LUMINANCE,
                                                      GL_LUMINANCE,
                                                      1,
                                                      planeV,
                                                      static_cast<int>(strideV),
                                                      m_cacheV,
                                                      m_pboV,
                                                      m_tightV);
        return yUploaded && uUploaded && vUploaded;
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

        const bool yUploaded = uploadPlaneViaPboAsync(GL_TEXTURE0,
                                                      m_texY,
                                                      static_cast<int>(widthY),
                                                      static_cast<int>(heightY),
                                                      GL_LUMINANCE,
                                                      GL_LUMINANCE,
                                                      1,
                                                      planeY,
                                                      static_cast<int>(strideY),
                                                      m_cacheY,
                                                      m_pboY,
                                                      m_tightY);
        const bool uvUploaded = uploadPlaneViaPboAsync(GL_TEXTURE1,
                                                       m_texUV,
                                                       static_cast<int>(widthUV),
                                                       static_cast<int>(heightUV),
                                                       GL_LUMINANCE_ALPHA,
                                                       GL_LUMINANCE_ALPHA,
                                                       2,
                                                       planeUV,
                                                       static_cast<int>(strideUV),
                                                       m_cacheUV,
                                                       m_pboUV,
                                                       m_tightUV);
        return yUploaded && uvUploaded;
    }

    void uploadHeldFrame()
    {
        if (!m_heldFrame) {
            m_hasData = false;
            m_uploadStatsValidForLastFrame = false;
            resetNativeFrameState();
            return;
        }

        const rflow_video_frame_backend_t backend = librflow_video_frame_get_backend(m_heldFrame);
        const rflow_native_handle_type_t nativeHandleType =
            librflow_video_frame_get_native_handle_type(m_heldFrame);
        resetNativeFrameState();

        if (backend == RFLOW_VIDEO_FRAME_BACKEND_GPU_EXTERNAL ||
            backend == RFLOW_VIDEO_FRAME_BACKEND_HARDWARE_BUFFER) {
            if ((backend != m_lastFrameBackend || nativeHandleType != m_lastNativeHandleType ||
                 m_renderedFrames < 5) &&
                (nativeHandleType == RFLOW_NATIVE_HANDLE_ANDROID_OES_TEXTURE ||
                 nativeHandleType == RFLOW_NATIVE_HANDLE_ANDROID_HARDWARE_BUFFER)) {
                qInfo().noquote()
                    << QStringLiteral("[RenderPerf] native frame path backend=%1 handle=%2")
                           .arg(backendToString(backend))
                           .arg(nativeHandleToString(nativeHandleType));
            }
            const bool prepared = prepareNativeFrame(m_heldFrame, backend, nativeHandleType);
            m_lastFrameBackend = backend;
            m_lastNativeHandleType = nativeHandleType;
            if (!prepared) {
                m_hasData = false;
                m_uploadStatsValidForLastFrame = false;
                clearHeldFrame();
            }
            return;
        }

        const rflow_codec_t codec = librflow_video_frame_get_codec(m_heldFrame);
        bool uploaded = false;
        if ((codec == RFLOW_CODEC_I420 && m_lastUploadPath != UploadPathKind::I420) ||
            (codec == RFLOW_CODEC_NV12 && m_lastUploadPath != UploadPathKind::NV12)) {
            resetCpuUploadPipeline();
            qInfo().noquote()
                << QStringLiteral("[RenderPerf] upload path switch codec=%1").arg(codec);
        }

        QElapsedTimer uploadTimer;
        uploadTimer.start();

        if (codec == RFLOW_CODEC_I420) {
            uploaded = uploadI420(m_heldFrame);
            if (uploaded) {
                m_frameFormat = RFLOW_CODEC_I420;
                m_lastUploadPath = UploadPathKind::I420;
            }
        } else if (codec == RFLOW_CODEC_NV12) {
            uploaded = uploadNV12(m_heldFrame);
            if (uploaded) {
                m_frameFormat = RFLOW_CODEC_NV12;
                m_lastUploadPath = UploadPathKind::NV12;
            }
        }

        m_lastUploadUs = uploadTimer.nsecsElapsed() / 1000;
        m_uploadStatsValidForLastFrame = uploaded;
        m_hasData = uploaded;
        m_lastFrameBackend = backend;
        m_lastNativeHandleType = nativeHandleType;
        clearHeldFrame();
    }

    WebRTCVideoRenderer *m_videoItem = nullptr;
    QRectF m_rect;
    GLfloat m_vertices[8] = {0};

    bool m_glInitialized = false;
    bool m_canUseUnpackRowLength = false;
    bool m_canUsePixelUnpackBuffer = false;
    bool m_canUseExternalOesSampling = false;
    bool m_hasData = false;
    rflow_codec_t m_frameFormat = RFLOW_CODEC_I420;
    UploadPathKind m_lastUploadPath = UploadPathKind::None;
    rflow_video_frame_backend_t m_lastFrameBackend = RFLOW_VIDEO_FRAME_BACKEND_UNKNOWN;
    rflow_native_handle_type_t m_lastNativeHandleType = RFLOW_NATIVE_HANDLE_NONE;
    NativeFramePath m_nativePath = NativeFramePath::None;
    GLuint m_nativeTextureId = 0;
    bool m_nativeSamplingAcquired = false;
    librflow_gl_prepare_fn m_nativePrepareFn = nullptr;
    void *m_nativePrepareUserdata = nullptr;

    QOpenGLShaderProgram m_i420Program;
    QOpenGLShaderProgram m_nv12Program;
    QOpenGLShaderProgram m_oesProgram;
    GLuint m_texY = 0;
    GLuint m_texU = 0;
    GLuint m_texV = 0;
    GLuint m_texUV = 0;

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

    librflow_video_frame_t m_heldFrame = nullptr;
    quint32 m_currentFrameId = 0;
    bool m_haveFrameId = false;
    qint64 m_lastUploadUs = 0;
    bool m_uploadStatsValidForLastFrame = false;
    quint64 m_renderedFrames = 0;
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
