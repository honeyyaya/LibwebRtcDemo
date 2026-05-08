#ifndef WEBRTC_VIDEO_RENDERER_H
#define WEBRTC_VIDEO_RENDERER_H

#include <QMutex>
#include <QQuickItem>
#include <QString>

#include "video_frame_sink.h"

class QSGNode;

class WebRTCVideoRenderer : public QQuickItem, public VideoFrameSink
{
    Q_OBJECT
    Q_INTERFACES(VideoFrameSink)
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY hasVideoChanged)
    Q_PROPERTY(int highlightFrameId READ highlightFrameId NOTIFY highlightFrameIdChanged)
    Q_PROPERTY(QString sampledPipelineLine READ sampledPipelineLine NOTIFY sampledPipelineStatsChanged)
    Q_PROPERTY(bool hasSampledPipelineUi READ hasSampledPipelineUi NOTIFY sampledPipelineStatsChanged)

public:
    explicit WebRTCVideoRenderer(QQuickItem *parent = nullptr);
    ~WebRTCVideoRenderer() override;

    void presentFrame(librflow_video_frame_t frame) override;
    Q_INVOKABLE void clearVideoTrack() override;

    bool hasVideo() const { return m_hasVideo; }
    int highlightFrameId() const;
    QString sampledPipelineLine() const;
    bool hasSampledPipelineUi() const { return m_hasSampledPipelineUi; }

    Q_INVOKABLE void applySampledPipelineUi(int glTraceFrameId,
                                            double decodeToRenderTotalMs,
                                            double wallOnFrameToRenderMs);

    bool takeFrame(librflow_video_frame_t &outFrame, quint32 &outFrameId);

    /// 渲染节点在 sync(this) 内调用，读取最近一次 takeFrame 对应的 GL queue trace 元数据。
    /// 三元组在 presentFrame 入口锁内记录、takeFrame 时拷出，仅在 GL/scene-graph 线程读取。
    qint64 lastTakenGlQueueTraceStartMonoUs() const { return m_lastTakenGlQueueTraceStartMonoUs; }
    int lastTakenGlQueueTraceFrameId() const { return m_lastTakenGlQueueTraceFrameId; }
    bool lastTakenGlQueueTraceFrameFromTracking() const
    {
        return m_lastTakenGlQueueTraceFromTracking;
    }

Q_SIGNALS:
    void hasVideoChanged();
    void highlightFrameIdChanged();
    void sampledPipelineStatsChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

public:
      bool m_hasVideo = false;
private:
    friend class WebRTCVideoRenderNode;

    mutable QMutex m_frameMutex;
    librflow_video_frame_t m_pendingFrame = nullptr;
    bool m_pendingValid = false;
    bool m_updatePending = false;

    int m_highlightFrameId = -1;

    int m_sampledHighlightFrameId = -1;
    double m_sampledDecodeToRenderMs = -1.0;
    double m_sampledWallOnFrameToRenderMs = -1.0;
    bool m_hasSampledPipelineUi = false;

    /// presentFrame 入队 → render() 完成的 wall 时延采样起点（与 latency_trace::monotonicUs 同源）。
    /// pendingXxx 在 GUI/SDK 投递线程锁内更新；lastTakenXxx 在 takeFrame 时同步拷出，
    /// 仅由 QSGRenderNode::sync()/render() 在 scene-graph 线程读取，避免跨线程使用同一字段。
    qint64 m_pendingGlQueueTraceStartMonoUs = 0;
    int m_pendingGlQueueTraceFrameId = -1;
    bool m_pendingGlQueueTraceFromTracking = false;

    qint64 m_lastTakenGlQueueTraceStartMonoUs = 0;
    int m_lastTakenGlQueueTraceFrameId = -1;
    bool m_lastTakenGlQueueTraceFromTracking = false;

    /// 帧间隔（presentFrame 间），仅用于采样日志展示，非渲染逻辑依赖。
    qint64 m_lastPresentMonoUs = 0;
    quint64 m_presentCount = 0;
};

#endif
