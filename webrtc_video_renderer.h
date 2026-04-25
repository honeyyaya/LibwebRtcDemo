#ifndef WEBRTC_VIDEO_RENDERER_H
#define WEBRTC_VIDEO_RENDERER_H

#include <QMutex>
#include <QQuickFramebufferObject>
#include <QString>

#include "video_frame_sink.h"

class WebRTCGLRenderer;

class WebRTCVideoRenderer : public QQuickFramebufferObject, public VideoFrameSink
{
    Q_OBJECT
    Q_INTERFACES(VideoFrameSink)
    friend class WebRTCGLRenderer;
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY hasVideoChanged)
    Q_PROPERTY(int highlightFrameId READ highlightFrameId NOTIFY highlightFrameIdChanged)
    Q_PROPERTY(bool frameIdFromTracking READ frameIdFromTracking NOTIFY highlightFrameIdChanged)
    Q_PROPERTY(int encodedIngressTrackingId READ encodedIngressTrackingId NOTIFY encodedIngressTrackingChanged)
    Q_PROPERTY(bool hasEncodedIngressTracking READ hasEncodedIngressTracking NOTIFY encodedIngressTrackingChanged)
    Q_PROPERTY(int traceTargetFrameId READ traceTargetFrameId WRITE setTraceTargetFrameId NOTIFY traceTargetFrameIdChanged)
    Q_PROPERTY(int sampledHighlightFrameId READ sampledHighlightFrameId NOTIFY sampledPipelineStatsChanged)
    Q_PROPERTY(double sampledDecodeToRenderMs READ sampledDecodeToRenderMs NOTIFY sampledPipelineStatsChanged)
    Q_PROPERTY(double sampledWallOnFrameToRenderMs READ sampledWallOnFrameToRenderMs NOTIFY sampledPipelineStatsChanged)
    Q_PROPERTY(QString sampledPipelineLine READ sampledPipelineLine NOTIFY sampledPipelineStatsChanged)
    Q_PROPERTY(bool hasSampledPipelineUi READ hasSampledPipelineUi NOTIFY sampledPipelineStatsChanged)

public:
    explicit WebRTCVideoRenderer(QQuickItem *parent = nullptr);
    ~WebRTCVideoRenderer() override;

    void presentFrame(librflow_video_frame_t frame) override;
    Q_INVOKABLE void clearVideoTrack() override;

    bool hasVideo() const { return m_hasVideo; }
    int highlightFrameId() const;
    bool frameIdFromTracking() const;
    int encodedIngressTrackingId() const { return -1; }
    bool hasEncodedIngressTracking() const { return false; }

    int traceTargetFrameId() const { return m_traceTargetFrameId; }
    void setTraceTargetFrameId(int id);

    int sampledHighlightFrameId() const { return m_sampledHighlightFrameId; }
    double sampledDecodeToRenderMs() const { return m_sampledDecodeToRenderMs; }
    double sampledWallOnFrameToRenderMs() const { return m_sampledWallOnFrameToRenderMs; }
    QString sampledPipelineLine() const;
    bool hasSampledPipelineUi() const { return m_hasSampledPipelineUi; }

    Q_INVOKABLE void applySampledPipelineUi(int glTraceFrameId,
                                            double decodeToRenderTotalMs,
                                            double wallOnFrameToRenderMs);

    Renderer *createRenderer() const override;

    bool takeFrame(librflow_video_frame_t &outFrame, quint32 &outFrameId);

Q_SIGNALS:
    void hasVideoChanged();
    void highlightFrameIdChanged();
    void encodedIngressTrackingChanged();
    void traceTargetFrameIdChanged();
    void sampledPipelineStatsChanged();

private:
    mutable QMutex m_frameMutex;
    librflow_video_frame_t m_pendingFrame = nullptr;
    bool m_pendingValid = false;

    bool m_hasVideo = false;
    int m_highlightFrameId = -1;
    bool m_frameIdFromTracking = false;

    int m_traceTargetFrameId = -1;

    int m_sampledHighlightFrameId = -1;
    double m_sampledDecodeToRenderMs = -1.0;
    double m_sampledWallOnFrameToRenderMs = -1.0;
    bool m_hasSampledPipelineUi = false;
};

#endif
