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
#include <QPointer>
#include <QQuickWindow>
#include <QSGRenderNode>
#include <QSGRendererInterface>
#include <QSurfaceFormat>
#include <QThread>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <vector>

#ifdef Q_OS_ANDROID
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
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

/// 渲染线程在 sync 入口看到的 mailbox 帧若早于此阈值则直接丢弃——避免拉一帧远过期的画面去渲染。
/// 与 rtc_demo_new 的 GL queue stale-drop 阈值同量级。
constexpr qint64 kStaleMailboxFrameDropUs = 100 * 1000; // 100 ms

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
    HardwareBufferEglImage,
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

#ifdef Q_OS_ANDROID
bool hasExtensionToken(const char *extensions, const char *needle)
{
    if (!extensions || !needle || !needle[0]) {
        return false;
    }

    const QByteArray haystack(extensions);
    const QByteArray token(needle);
    int pos = 0;
    while ((pos = haystack.indexOf(token, pos)) >= 0) {
        const bool startOk = pos == 0 || haystack.at(pos - 1) == ' ';
        const int endPos = pos + token.size();
        const bool endOk = endPos == haystack.size() || haystack.at(endPos) == ' ';
        if (startOk && endOk) {
            return true;
        }
        pos = endPos;
    }
    return false;
}
#endif

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

        /// 从 m_renderFrame slot 拉走当前帧；slot 由 onBeforeSynchronizing() 在渲染线程 promote。
        /// 同一 generation 只消费一次，避免重复 sync 同一帧。
        WebRTCVideoRenderer::RenderFrameSnapshot snap;
        if (!item->consumeRenderFrame(m_lastConsumedGeneration, snap)) {
            if (!item->hasVideo()) {
                m_hasData = false;
                clearHeldFrame();
            }
            m_haveFrameId = false;
            m_haveFrameTrace = false;
            return;
        }
        m_lastConsumedGeneration = snap.generation;

        clearHeldFrame();
        m_heldFrame = snap.frame;
        m_currentFrameId = static_cast<quint32>(std::max(0, snap.frameId));
        m_haveFrameId = true;

        m_traceFrameId = snap.frameId;
        m_traceQueueStartUs = snap.queueStartMonoUs;
        m_traceGuiUpdateUs = snap.guiUpdateDispatchMonoUs;
        m_traceSyncStartUs = snap.syncStartMonoUs;
        m_traceMailboxAgeUs = snap.mailboxAgeUs;
        m_tracePacerWaitUs = snap.pacerWaitUs;
        m_haveFrameTrace = true;

        demo::latency_trace::recordSync(m_currentFrameId);
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

        const qint64 uploadStartUs = demo::latency_trace::monotonicUs();

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
        const bool useNativeTexture = m_nativePath != NativeFramePath::None && m_oesProgram.isLinked();
        if (useNativeTexture) {
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
        static const GLfloat kTexCoordsMirroredX[] = {
            1.0f, 1.0f, 0.0f, 1.0f,
            1.0f, 0.0f, 0.0f, 0.0f
        };
        const GLfloat *texCoords = useNativeTexture ? kTexCoordsMirroredX : kTexCoords;

        program->bind();
        program->setUniformValue("qt_Matrix", mvp);
        program->setAttributeArray(0, GL_FLOAT, m_vertices, 2);
        program->enableAttributeArray(0);
        program->setAttributeArray(1, GL_FLOAT, texCoords, 2);
        program->enableAttributeArray(1);

        if (useNativeTexture) {
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

        releaseSamplingAfterDraw();

        /// 端到端渲染采样：与 rtc_demo_new 的 tracking_id mod 120 路径同源。
        /// 采样指标（落本地日志 + 投到 GUI 驱动 HUD）：
        ///   pacer_wait      = sync_start - GUI dispatch（== Qt threaded RL 排队 + vsync 等待）
        ///   sync_to_render  = upload_start - sync_start
        ///   upload+draw     = uploadHeldFrame 上传耗时 + glDraw 耗时
        ///   wall(present->render) = nowUs - presentFrame 入队时刻
        ///   total(SDK->render)    = nowUs - SDK 回调入口（peekSdkCallbackUs）
        if (m_uploadStatsValidForLastFrame && m_haveFrameTrace && m_traceFrameId >= 0) {
            const quint32 fid = static_cast<quint32>(m_traceFrameId);
            const qint64 nowUs = demo::latency_trace::monotonicUs();
            const qint64 wallFromPresentUs = nowUs - m_traceQueueStartUs;
            const qint64 syncToRenderUs = std::max<qint64>(0, uploadStartUs - m_traceSyncStartUs);
            const qint64 uploadDrawUs = m_lastUploadUs + drawUs;

            qint64 sdkCallbackUs = 0;
            double decodeToRenderMs = -1.0;
            if (demo::latency_trace::peekSdkCallbackUs(fid, &sdkCallbackUs) && sdkCallbackUs > 0) {
                decodeToRenderMs = (nowUs - sdkCallbackUs) / 1000.0;
            }

            if (m_renderedFrames <= 5 || (fid % 120) == 0) {
                const QString decodeToRenderStr =
                    decodeToRenderMs >= 0.0 ? QString::number(decodeToRenderMs, 'f', 3)
                                            : QStringLiteral("—");
                qInfo().noquote()
                    << QStringLiteral("[RenderPerf] frame=%1 mailbox_age=%2ms pacer_wait=%3ms "
                                      "sync_to_render=%4ms upload+draw=%5ms wall=%6ms total=%7ms")
                           .arg(fid)
                           .arg(m_traceMailboxAgeUs / 1000.0, 0, 'f', 3)
                           .arg(m_tracePacerWaitUs / 1000.0, 0, 'f', 3)
                           .arg(syncToRenderUs / 1000.0, 0, 'f', 3)
                           .arg(uploadDrawUs / 1000.0, 0, 'f', 3)
                           .arg(wallFromPresentUs / 1000.0, 0, 'f', 3)
                           .arg(decodeToRenderStr);

                if (m_videoItem) {
                    QMetaObject::invokeMethod(
                        m_videoItem, "applySampledPipelineUi", Qt::QueuedConnection,
                        Q_ARG(int, m_traceFrameId),
                        Q_ARG(double, m_tracePacerWaitUs / 1000.0),
                        Q_ARG(double, syncToRenderUs / 1000.0),
                        Q_ARG(double, uploadDrawUs / 1000.0),
                        Q_ARG(double, decodeToRenderMs),
                        Q_ARG(double, wallFromPresentUs / 1000.0));
                }
            }
        }
        m_haveFrameTrace = false;

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
#ifdef Q_OS_ANDROID
        initializeAndroidNativeInterop();
#endif
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

#ifdef Q_OS_ANDROID
        if (m_canUseExternalOesSampling && m_importTextureId == 0) {
            glGenTextures(1, &m_importTextureId);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_importTextureId);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
#endif
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
#ifdef Q_OS_ANDROID
        destroyImportedEglImage();
        if (m_importTextureId != 0) {
            glDeleteTextures(1, &m_importTextureId);
            m_importTextureId = 0;
        }
#endif
        m_nativeTextureId = 0;
        m_glInitialized = false;
    }

    void clearHeldFrame()
    {
        releaseSamplingAfterDraw();
#ifdef Q_OS_ANDROID
        destroyImportedEglImage();
#endif
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

#ifdef Q_OS_ANDROID
    void initializeAndroidNativeInterop()
    {
        if (m_androidInteropInitialized) {
            return;
        }

        m_eglDisplay = eglGetCurrentDisplay();
        if (m_eglDisplay == EGL_NO_DISPLAY) {
            return;
        }

        const char *eglExts = eglQueryString(m_eglDisplay, EGL_EXTENSIONS);
        const bool hasImageBase = hasExtensionToken(eglExts, "EGL_KHR_image_base");
        const bool hasNativeBuffer = hasExtensionToken(eglExts, "EGL_ANDROID_image_native_buffer");
        const bool hasGetNativeBuffer = hasExtensionToken(eglExts, "EGL_ANDROID_get_native_client_buffer");
        const bool hasGlEglImage = QOpenGLContext::currentContext()->hasExtension(QByteArrayLiteral("GL_OES_EGL_image"));

        if (hasImageBase && hasNativeBuffer && hasGetNativeBuffer && hasGlEglImage &&
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
                                        m_eglGetNativeClientBufferANDROID && m_glEGLImageTargetTexture2DOES;
        }

        m_androidInteropInitialized = true;
    }

    void destroyImportedEglImage()
    {
        if (m_currentEglImage != EGL_NO_IMAGE_KHR && m_eglDestroyImageKHR && m_eglDisplay != EGL_NO_DISPLAY) {
            m_eglDestroyImageKHR(m_eglDisplay, m_currentEglImage);
        }
        m_currentEglImage = EGL_NO_IMAGE_KHR;
    }

    bool importHardwareBufferFrame(librflow_video_frame_t frame,
                                   rflow_video_frame_backend_t backend,
                                   rflow_native_handle_type_t nativeHandleType)
    {
        if (!m_canImportHardwareBuffer || m_importTextureId == 0 || m_eglDisplay == EGL_NO_DISPLAY) {
            qWarning().noquote()
                << QStringLiteral("[RenderPerf] AHardwareBuffer import unavailable backend=%1 handle=%2")
                       .arg(backendToString(backend))
                       .arg(nativeHandleToString(nativeHandleType));
            return false;
        }

        auto *hardwareBuffer = static_cast<AHardwareBuffer *>(
            librflow_video_frame_get_android_hardware_buffer(frame));
        if (!hardwareBuffer) {
            qWarning().noquote() << QStringLiteral("[RenderPerf] AHardwareBuffer handle is null");
            return false;
        }

        destroyImportedEglImage();

        EGLClientBuffer nativeClientBuffer = m_eglGetNativeClientBufferANDROID(hardwareBuffer);
        if (!nativeClientBuffer) {
            qWarning().noquote() << QStringLiteral("[RenderPerf] eglGetNativeClientBufferANDROID failed");
            return false;
        }

        const EGLint attrs[] = {
            EGL_IMAGE_PRESERVED_KHR, EGL_FALSE,
            EGL_NONE
        };
        m_currentEglImage = m_eglCreateImageKHR(
            m_eglDisplay, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, nativeClientBuffer, attrs);
        if (m_currentEglImage == EGL_NO_IMAGE_KHR) {
            qWarning().noquote() << QStringLiteral("[RenderPerf] eglCreateImageKHR failed for AHardwareBuffer");
            return false;
        }

        glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_importTextureId);
        m_glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES,
                                       reinterpret_cast<GLeglImageOES>(m_currentEglImage));

        const int32_t syncFenceFd = librflow_video_frame_get_sync_fence_fd(frame);
        if (syncFenceFd >= 0 && (m_renderedFrames <= 5 || (m_renderedFrames % 120) == 0)) {
            qInfo().noquote()
                << QStringLiteral("[RenderPerf] AHardwareBuffer frame carries sync fence fd=%1; relying on SDK "
                                  "acquire/prepare ordering")
                       .arg(syncFenceFd);
        }

        m_nativePath = NativeFramePath::HardwareBufferEglImage;
        m_nativeTextureId = m_importTextureId;
        m_lastUploadUs = 0;
        m_uploadStatsValidForLastFrame = true;
        m_hasData = true;
        return true;
    }
#endif

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

        if (nativeHandleType == RFLOW_NATIVE_HANDLE_ANDROID_HARDWARE_BUFFER) {
#ifdef Q_OS_ANDROID
            return importHardwareBufferFrame(frame, backend, nativeHandleType);
#else
            qWarning().noquote()
                << QStringLiteral("[RenderPerf] AHardwareBuffer backend received on non-Android build");
            releaseSamplingAfterDraw();
            return false;
#endif
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
#ifdef Q_OS_ANDROID
    bool m_androidInteropInitialized = false;
    bool m_canImportHardwareBuffer = false;
    EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
    EGLImageKHR m_currentEglImage = EGL_NO_IMAGE_KHR;
    PFNEGLCREATEIMAGEKHRPROC m_eglCreateImageKHR = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC m_eglDestroyImageKHR = nullptr;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC m_eglGetNativeClientBufferANDROID = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_glEGLImageTargetTexture2DOES = nullptr;
    GLuint m_importTextureId = 0;
#endif
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

    /// QSGRenderNode 仅在 sync()/render() 同一线程读写——sync 时从 WebRTCVideoRenderer::m_renderFrame 拷入；
    /// render 末尾消费一次后置 m_haveFrameTrace=false，避免对同一帧重复采样。
    int m_traceFrameId = -1;
    qint64 m_traceQueueStartUs = 0;
    qint64 m_traceGuiUpdateUs = 0;
    qint64 m_traceSyncStartUs = 0;
    qint64 m_traceMailboxAgeUs = 0;
    qint64 m_tracePacerWaitUs = 0;
    bool m_haveFrameTrace = false;

    quint64 m_lastConsumedGeneration = 0;
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

    const qint64 presentEntryUs = demo::latency_trace::monotonicUs();

    const uint32_t width = librflow_video_frame_get_width(frame);
    const uint32_t height = librflow_video_frame_get_height(frame);
    const quint32 frameId = librflow_video_frame_get_seq(frame);
    if (width == 0 || height == 0) {
        releaseFrame(frame);
        return;
    }

    librflow_video_frame_t oldFrame = nullptr;
    bool highlightChanged = false;
    bool wasMailboxOccupied = false;
    {
        QMutexLocker locker(&m_frameMutex);
        publishMailboxFrameLocked(frame, presentEntryUs, static_cast<int>(frameId), &oldFrame);
        wasMailboxOccupied = (oldFrame != nullptr);
        if (m_highlightFrameId != static_cast<int>(frameId)) {
            m_highlightFrameId = static_cast<int>(frameId);
            highlightChanged = true;
        }
    }

    if (oldFrame) {
        releaseFrame(oldFrame);
        m_mailboxDropCount.fetch_add(1, std::memory_order_relaxed);
    }

    demo::latency_trace::recordPresent(frameId, static_cast<int>(width), static_cast<int>(height));

    const quint64 publishedTotal = m_publishedFrames.fetch_add(1, std::memory_order_relaxed) + 1;

    bool needHasVideoEmit = false;
    if (!m_hasVideo.load(std::memory_order_acquire)) {
        m_hasVideo.store(true, std::memory_order_release);
        needHasVideoEmit = true;
    }

    /// hasVideoChanged / highlightFrameIdChanged 信号必须在 GUI 线程发射，
    /// 走 QueuedConnection 委托到本对象所属线程。
    if (needHasVideoEmit) {
        QMetaObject::invokeMethod(this, [this]() { Q_EMIT hasVideoChanged(); }, Qt::QueuedConnection);
    }
    if (highlightChanged) {
        QMetaObject::invokeMethod(
            this, [this]() { Q_EMIT highlightFrameIdChanged(); }, Qt::QueuedConnection);
    }

    if (publishedTotal <= 5 || (frameId % 600) == 0) {
        qInfo().noquote()
            << QStringLiteral("[RenderQueue] mailbox publish frame=%1 %2x%3 "
                              "drop_total=%4 mailbox_was_occupied=%5")
                   .arg(frameId)
                   .arg(width)
                   .arg(height)
                   .arg(m_mailboxDropCount.load(std::memory_order_relaxed))
                   .arg(wasMailboxOccupied ? QStringLiteral("true") : QStringLiteral("false"));
    }

    /// 唤醒渲染线程：先确保 beforeSynchronizing 已挂上（一次性，GUI 线程），
    /// 然后通过 QQuickWindow::update() (thread-safe) 让 RenderLoop 跑下一轮。
    if (!m_renderSchedulingActive.load(std::memory_order_acquire)) {
        if (!m_renderSchedulingStartPending.exchange(true, std::memory_order_acq_rel)) {
            QMetaObject::invokeMethod(
                this, [this]() { ensureRenderSchedulingActive(); }, Qt::QueuedConnection);
        }
    } else {
        queueMailboxRenderUpdate();
    }
}

void WebRTCVideoRenderer::publishMailboxFrameLocked(librflow_video_frame_t frame,
                                                    qint64 queueStartMonoUs,
                                                    int frameId,
                                                    librflow_video_frame_t *outOldFrame)
{
    if (outOldFrame) {
        *outOldFrame = m_mailboxFrame.frame;
    }
    m_mailboxFrame.frame = frame;
    m_mailboxFrame.queueStartMonoUs = queueStartMonoUs;
    m_mailboxFrame.guiUpdateDispatchMonoUs = 0;
    m_mailboxFrame.frameId = frameId;
    ++m_mailboxFrame.generation;
}

bool WebRTCVideoRenderer::takeMailboxFrameForRender(qint64 syncStartMonoUs)
{
    librflow_video_frame_t toRelease = nullptr;
    librflow_video_frame_t prevRender = nullptr;
    bool ready = false;
    {
        QMutexLocker locker(&m_frameMutex);
        if (!m_mailboxFrame.frame ||
            m_mailboxFrame.generation == m_lastResolvedMailboxGeneration) {
            return false;
        }

        const qint64 mailboxAgeUs = std::max<qint64>(0, syncStartMonoUs - m_mailboxFrame.queueStartMonoUs);
        if (mailboxAgeUs > kStaleMailboxFrameDropUs) {
            toRelease = m_mailboxFrame.frame;
            m_mailboxFrame.frame = nullptr;
            m_lastResolvedMailboxGeneration = m_mailboxFrame.generation;
        } else {
            const qint64 guiDispatch =
                m_mailboxFrame.guiUpdateDispatchMonoUs > 0 ? m_mailboxFrame.guiUpdateDispatchMonoUs
                : m_renderUpdateDispatchMonoUs > 0         ? m_renderUpdateDispatchMonoUs
                                                           : syncStartMonoUs;
            const qint64 pacerWaitUs = std::max<qint64>(0, syncStartMonoUs - guiDispatch);

            prevRender = m_renderFrame.frame;
            m_renderFrame.frame = m_mailboxFrame.frame;
            m_renderFrame.queueStartMonoUs = m_mailboxFrame.queueStartMonoUs;
            m_renderFrame.guiUpdateDispatchMonoUs = guiDispatch;
            m_renderFrame.syncStartMonoUs = syncStartMonoUs;
            m_renderFrame.mailboxAgeUs = mailboxAgeUs;
            m_renderFrame.pacerWaitUs = pacerWaitUs;
            m_renderFrame.frameId = m_mailboxFrame.frameId;
            m_renderFrame.generation = m_mailboxFrame.generation;
            m_renderFrame.ready = true;
            m_lastResolvedMailboxGeneration = m_mailboxFrame.generation;
            m_mailboxFrame.frame = nullptr;
            ready = true;
        }
    }

    if (toRelease) {
        m_mailboxDropCount.fetch_add(1, std::memory_order_relaxed);
        releaseFrame(toRelease);
        return false;
    }
    if (prevRender) {
        /// renderFrame 上一帧还没被 node->sync 消费就被新帧覆盖（理论上少见，做兜底）
        m_mailboxDropCount.fetch_add(1, std::memory_order_relaxed);
        releaseFrame(prevRender);
    }
    return ready;
}

void WebRTCVideoRenderer::queueMailboxRenderUpdate()
{
    if (m_renderUpdateInvokePending.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (QThread::currentThread() == this->thread()) {
        m_renderUpdateInvokePending.store(false, std::memory_order_release);
        requestMailboxRenderUpdate();
        return;
    }
    QMetaObject::invokeMethod(this,
                              [this]() {
                                  m_renderUpdateInvokePending.store(false, std::memory_order_release);
                                  requestMailboxRenderUpdate();
                              },
                              Qt::QueuedConnection);
}

void WebRTCVideoRenderer::requestMailboxRenderUpdate()
{
    if (!m_renderSchedulingActive.load(std::memory_order_acquire)) {
        return;
    }
    if (!window()) {
        return;
    }
    if (m_renderUpdatePending.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    const qint64 dispatchUs = demo::latency_trace::monotonicUs();
    {
        QMutexLocker locker(&m_frameMutex);
        m_renderUpdateDispatchMonoUs = dispatchUs;
        if (m_mailboxFrame.frame && m_mailboxFrame.guiUpdateDispatchMonoUs == 0) {
            m_mailboxFrame.guiUpdateDispatchMonoUs = dispatchUs;
        }
    }

    update();           // 标脏 item，触发 updatePaintNode
    window()->update(); // 唤醒渲染线程（thread-safe）
}

void WebRTCVideoRenderer::ensureRenderSchedulingActive()
{
    m_renderSchedulingStartPending.store(false, std::memory_order_release);
    if (!hasVideo()) {
        return;
    }
    if (!window()) {
        /// 还未 attach 到 window；等 itemChange(ItemSceneChange) 把 onWindowChanged 接好后会再触发。
        return;
    }
    if (m_observedWindow != window()) {
        onWindowChanged(window());
    }
    m_renderSchedulingActive.store(true, std::memory_order_release);
    queueMailboxRenderUpdate();
}

void WebRTCVideoRenderer::onWindowChanged(QQuickWindow *win)
{
    if (m_beforeSynchronizingConnection) {
        QObject::disconnect(m_beforeSynchronizingConnection);
        m_beforeSynchronizingConnection = {};
    }
    m_observedWindow = win;
    if (!win) {
        return;
    }
    win->setPersistentGraphics(true);
    win->setPersistentSceneGraph(true);
    /// beforeSynchronizing 在渲染线程发射，DirectConnection 让我们就地从 mailbox 拉帧。
    m_beforeSynchronizingConnection =
        connect(win, &QQuickWindow::beforeSynchronizing, this,
                &WebRTCVideoRenderer::onBeforeSynchronizing, Qt::DirectConnection);
}

void WebRTCVideoRenderer::onBeforeSynchronizing()
{
    /// 渲染线程：把 mailbox 提升到 m_renderFrame，供随后的 updatePaintNode->sync 消费。
    /// 多消费一次（loop 直到没有新帧），避免渲染线程一帧只吃一格。
    const qint64 syncStartUs = demo::latency_trace::monotonicUs();
    bool got = false;
    while (takeMailboxFrameForRender(syncStartUs)) {
        got = true;
    }

    /// renderUpdatePending 在渲染线程清掉，让下一次 presentFrame 能再触发一次 update()。
    m_renderUpdatePending.store(false, std::memory_order_release);

    if (!got) {
        /// 没拉到新帧也无所谓——可能 mailbox 还没填，或被 stale-drop 了。
        return;
    }
}

void WebRTCVideoRenderer::releaseFrame(librflow_video_frame_t frame)
{
    if (!frame) {
        return;
    }
    /// 假设 librflow_video_frame_release 跨线程安全（与 rtc_demo_new 等价的前提）。
    librflow_video_frame_release(frame);
}

void WebRTCVideoRenderer::clearVideoTrack()
{
    const bool hadVideo = m_hasVideo.exchange(false, std::memory_order_acq_rel);

    librflow_video_frame_t mailboxFrame = nullptr;
    librflow_video_frame_t renderFrame = nullptr;
    {
        QMutexLocker locker(&m_frameMutex);
        mailboxFrame = m_mailboxFrame.frame;
        m_mailboxFrame.frame = nullptr;
        m_mailboxFrame.queueStartMonoUs = 0;
        m_mailboxFrame.guiUpdateDispatchMonoUs = 0;
        m_mailboxFrame.frameId = -1;
        ++m_mailboxFrame.generation;

        renderFrame = m_renderFrame.frame;
        m_renderFrame.frame = nullptr;
        m_renderFrame.ready = false;
        m_renderFrame.frameId = -1;
        m_renderFrame.generation = 0;
        m_renderFrame.queueStartMonoUs = 0;
        m_renderFrame.guiUpdateDispatchMonoUs = 0;
        m_renderFrame.syncStartMonoUs = 0;
        m_renderFrame.mailboxAgeUs = 0;
        m_renderFrame.pacerWaitUs = 0;

        m_lastResolvedMailboxGeneration = 0;
        m_renderUpdateDispatchMonoUs = 0;
        m_highlightFrameId = -1;
    }

    if (mailboxFrame) {
        releaseFrame(mailboxFrame);
    }
    if (renderFrame) {
        releaseFrame(renderFrame);
    }

    m_publishedFrames.store(0, std::memory_order_relaxed);
    m_mailboxDropCount.store(0, std::memory_order_relaxed);
    m_renderUpdatePending.store(false, std::memory_order_release);
    m_renderUpdateInvokePending.store(false, std::memory_order_release);
    m_renderSchedulingStartPending.store(false, std::memory_order_release);
    m_renderSchedulingActive.store(false, std::memory_order_release);

    if (hadVideo) {
        Q_EMIT hasVideoChanged();
    }
    Q_EMIT highlightFrameIdChanged();
    if (m_hasSampledPipelineUi) {
        m_sampledHighlightFrameId = -1;
        m_sampledPacerWaitMs = -1.0;
        m_sampledSyncToRenderMs = -1.0;
        m_sampledUploadDrawMs = -1.0;
        m_sampledDecodeToRenderMs = -1.0;
        m_sampledWallPresentToRenderMs = -1.0;
        m_hasSampledPipelineUi = false;
        Q_EMIT sampledPipelineStatsChanged();
    }

    if (window()) {
        update();
        window()->update();
    }
}

bool WebRTCVideoRenderer::consumeRenderFrame(quint64 lastConsumedGeneration,
                                             RenderFrameSnapshot &outSnapshot)
{
    QMutexLocker locker(&m_frameMutex);
    if (!m_renderFrame.ready || !m_renderFrame.frame ||
        m_renderFrame.generation == lastConsumedGeneration) {
        return false;
    }
    outSnapshot.frame = m_renderFrame.frame;
    outSnapshot.queueStartMonoUs = m_renderFrame.queueStartMonoUs;
    outSnapshot.guiUpdateDispatchMonoUs = m_renderFrame.guiUpdateDispatchMonoUs;
    outSnapshot.syncStartMonoUs = m_renderFrame.syncStartMonoUs;
    outSnapshot.mailboxAgeUs = m_renderFrame.mailboxAgeUs;
    outSnapshot.pacerWaitUs = m_renderFrame.pacerWaitUs;
    outSnapshot.frameId = m_renderFrame.frameId;
    outSnapshot.generation = m_renderFrame.generation;

    m_renderFrame.frame = nullptr;
    m_renderFrame.ready = false;
    return true;
}

int WebRTCVideoRenderer::highlightFrameId() const
{
    QMutexLocker locker(&m_frameMutex);
    return m_highlightFrameId;
}

QString WebRTCVideoRenderer::sampledPipelineLine() const
{
    if (!m_hasSampledPipelineUi) {
        return {};
    }
    const QString decodeToRender =
        m_sampledDecodeToRenderMs >= 0.0 ? QString::number(m_sampledDecodeToRenderMs, 'f', 3)
                                         : QStringLiteral("N/A");
    return QStringLiteral("帧%1 | pacer %2ms | sync→render %3ms | upload+draw %4ms | wall %5ms | total %6ms | drop %7")
        .arg(m_sampledHighlightFrameId)
        .arg(m_sampledPacerWaitMs, 0, 'f', 2)
        .arg(m_sampledSyncToRenderMs, 0, 'f', 2)
        .arg(m_sampledUploadDrawMs, 0, 'f', 2)
        .arg(m_sampledWallPresentToRenderMs, 0, 'f', 2)
        .arg(decodeToRender)
        .arg(static_cast<qulonglong>(m_mailboxDropCount.load(std::memory_order_acquire)));
}

void WebRTCVideoRenderer::applySampledPipelineUi(int frameId,
                                                 double pacerWaitMs,
                                                 double syncToRenderMs,
                                                 double uploadDrawMs,
                                                 double decodeToRenderTotalMs,
                                                 double wallPresentToRenderMs)
{
    m_sampledHighlightFrameId = frameId;
    m_sampledPacerWaitMs = pacerWaitMs;
    m_sampledSyncToRenderMs = syncToRenderMs;
    m_sampledUploadDrawMs = uploadDrawMs;
    m_sampledDecodeToRenderMs = decodeToRenderTotalMs;
    m_sampledWallPresentToRenderMs = wallPresentToRenderMs;
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

void WebRTCVideoRenderer::itemChange(ItemChange change, const ItemChangeData &value)
{
    if (change == ItemSceneChange) {
        onWindowChanged(value.window);
        if (value.window && hasVideo()) {
            m_renderSchedulingActive.store(true, std::memory_order_release);
            queueMailboxRenderUpdate();
        } else {
            m_renderSchedulingActive.store(false, std::memory_order_release);
        }
    }
    QQuickItem::itemChange(change, value);
}
