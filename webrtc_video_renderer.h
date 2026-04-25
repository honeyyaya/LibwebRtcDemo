#ifndef WEBRTC_VIDEO_RENDERER_H
#define WEBRTC_VIDEO_RENDERER_H

#include <QQuickFramebufferObject>
#include <QRectF>
#include <QMutex>
#include <QElapsedTimer>
#include <QString>

#include "api/media_stream_interface.h"
#include "api/scoped_refptr.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_buffer.h"
#include "api/video/video_sink_interface.h"

class WebRTCGLRenderer;

/// 远程帧路径（典型）：OnFrame(非主线程) → 队列 update() → 下一场景图帧里 synchronize() → render() → 再经窗口合成，故常见约 1 帧(≈16ms@60Hz) 的隐藏时延，threaded S.G. 时偶发 2 帧。
/// 缓解：主程序可用 QSG_RENDER_LOOP=basic（见 main.cpp）；最省延迟需改结构（OES+同上下文、QSG 纹理节点、QQuickRenderControl/原生子窗口 等）而非本类单独修完。
class WebRTCVideoRenderer : public QQuickFramebufferObject,
                            public webrtc::VideoSinkInterface<webrtc::VideoFrame>
{
    Q_OBJECT
    friend class WebRTCGLRenderer;
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY hasVideoChanged)
    /// 当前用于高亮的帧标识：来自 VideoFrame::id()（RTP 扩展/Field Trial）时为真实 ID；否则为本地预览序号（未联通服务器时仍可见）。
    Q_PROPERTY(int highlightFrameId READ highlightFrameId NOTIFY highlightFrameIdChanged)
    Q_PROPERTY(bool frameIdFromTracking READ frameIdFromTracking NOTIFY highlightFrameIdChanged)
    /// 编码入站（Decode 路径）最后一帧的 VideoFrameTrackingId；不依赖解码/出画，可与 log 对齐。
    Q_PROPERTY(int encodedIngressTrackingId READ encodedIngressTrackingId NOTIFY
                   encodedIngressTrackingChanged)
    Q_PROPERTY(bool hasEncodedIngressTracking READ hasEncodedIngressTracking NOTIFY
                   encodedIngressTrackingChanged)
    /// 设为 >=0 时仅对该 VideoFrame::id（或预览序号）打「上传+渲染」跟踪日志；-1 表示不筛选（仍按 STATS 间隔输出带 frame_id 的行）。
    Q_PROPERTY(int traceTargetFrameId READ traceTargetFrameId WRITE setTraceTargetFrameId NOTIFY
                   traceTargetFrameIdChanged)
    /// 与 m_glQueueTraceFrameId 一致，仅在 tracking_id%120 采样刷新（Decode→render 总耗时展示行）。
    Q_PROPERTY(int sampledHighlightFrameId READ sampledHighlightFrameId NOTIFY sampledPipelineStatsChanged)
    Q_PROPERTY(double sampledDecodeToRenderMs READ sampledDecodeToRenderMs NOTIFY
                   sampledPipelineStatsChanged)
    Q_PROPERTY(double sampledWallOnFrameToRenderMs READ sampledWallOnFrameToRenderMs NOTIFY
                   sampledPipelineStatsChanged)
    Q_PROPERTY(QString sampledPipelineLine READ sampledPipelineLine NOTIFY sampledPipelineStatsChanged)
    Q_PROPERTY(bool hasSampledPipelineUi READ hasSampledPipelineUi NOTIFY sampledPipelineStatsChanged)
    QML_ELEMENT

public:
    explicit WebRTCVideoRenderer(QQuickItem *parent = nullptr);
    ~WebRTCVideoRenderer() override;

    void OnFrame(const webrtc::VideoFrame &frame) override;

    Q_INVOKABLE void setVideoTrack(webrtc::scoped_refptr<webrtc::VideoTrackInterface> track);
    Q_INVOKABLE void clearVideoTrack();

    bool hasVideo() const { return m_hasVideo; }
    Q_SIGNAL void hasVideoChanged();

    int highlightFrameId() const;
    bool frameIdFromTracking() const;
    Q_SIGNAL void highlightFrameIdChanged();

    int encodedIngressTrackingId() const;
    bool hasEncodedIngressTracking() const;
    Q_SIGNAL void encodedIngressTrackingChanged();

    int traceTargetFrameId() const { return m_traceTargetFrameId; }
    void setTraceTargetFrameId(int id);
    Q_SIGNAL void traceTargetFrameIdChanged();

    int sampledHighlightFrameId() const { return m_sampledHighlightFrameId; }
    double sampledDecodeToRenderMs() const { return m_sampledDecodeToRenderMs; }
    double sampledWallOnFrameToRenderMs() const { return m_sampledWallOnFrameToRenderMs; }
    QString sampledPipelineLine() const;
    bool hasSampledPipelineUi() const { return m_hasSampledPipelineUi; }
    Q_SIGNAL void sampledPipelineStatsChanged();

    /// 由 WebRTCGLRenderer::render 经 QMetaObject::invokeMethod 投递到 GUI 线程；glTraceFrameId 即 m_glQueueTraceFrameId。
    Q_INVOKABLE void applySampledPipelineUi(int glTraceFrameId, double decodeToRenderTotalMs,
                                            double wallOnFrameToRenderMs);

    Renderer *createRenderer() const override;

    /// 传出本帧的 VideoFrameBuffer：可为 I420 平面缓冲，或 kNative 的 OesEglTextureFrameBuffer（OES / EGLImage 外纹理）；
    /// 纹理在 GL 线程与 synchronize 中绑定/更新，OnFrame 仅持引用、不整帧 CPU 拷。
    bool takeFrame(webrtc::scoped_refptr<webrtc::VideoFrameBuffer> &out);

    // 最近一次 takeFrame 对应的跟踪元数据（仅 GL 线程在 takeFrame 之后读取）。
    int64_t lastTakenGlQueueTraceStartMonoUs() const { return m_lastTakenGlQueueTraceStartMonoUs; }
    int lastTakenGlQueueTraceFrameId() const { return m_lastTakenGlQueueTraceFrameId; }
    bool lastTakenGlQueueTraceFrameFromTracking() const { return m_lastTakenGlQueueTraceFromTracking; }

protected:
    void timerEvent(QTimerEvent *event) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    void applyVideoSinkWants();
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> m_track;
    bool m_hasVideo = false;

    mutable QMutex m_frameMutex;
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> m_pendingBuffer;
    bool m_pendingValid = false;

    int m_highlightFrameId = -1;
    bool m_frameIdFromTracking = false;
    uint32_t m_localPreviewSeq = 0;

    int m_frameCount = 0;
    QElapsedTimer m_decodeIntervalTimer;

    int m_lastPolledEncodedIngressId = -1;

    int m_traceTargetFrameId = -1;

    /// OnFrame 将本帧挂入 pending 时刻（steady 微秒），与 decode 链 McMonotonicUs 同源。
    int64_t m_pendingGlQueueTraceStartMonoUs = 0;
    int m_pendingGlQueueTraceFrameId = -1;

    int64_t m_lastTakenGlQueueTraceStartMonoUs = 0;
    int m_lastTakenGlQueueTraceFrameId = -1;
    bool m_pendingGlQueueTraceFromTracking = false;
    bool m_lastTakenGlQueueTraceFromTracking = false;

    int m_sampledHighlightFrameId = -1;
    double m_sampledDecodeToRenderMs = -1.0;
    double m_sampledWallOnFrameToRenderMs = -1.0;
    bool m_hasSampledPipelineUi = false;
};

#endif
