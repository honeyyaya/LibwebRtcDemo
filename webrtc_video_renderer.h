#ifndef WEBRTC_VIDEO_RENDERER_H
#define WEBRTC_VIDEO_RENDERER_H

#include <atomic>

#include <QElapsedTimer>
#include <QMutex>
#include <QQuickItem>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include "api/media_stream_interface.h"
#include "api/scoped_refptr.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_buffer.h"
#include "api/video/video_sink_interface.h"

class QSGNode;
class WebRTCVideoRenderNode;

class WebRTCVideoRenderer : public QQuickItem,
                            public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
  Q_OBJECT
  friend class WebRTCVideoRenderNode;
  Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY hasVideoChanged)
  Q_PROPERTY(int highlightFrameId READ highlightFrameId NOTIFY highlightFrameIdChanged)
  Q_PROPERTY(bool frameIdFromTracking READ frameIdFromTracking NOTIFY highlightFrameIdChanged)
  Q_PROPERTY(int encodedIngressTrackingId READ encodedIngressTrackingId NOTIFY encodedIngressTrackingChanged)
  Q_PROPERTY(bool hasEncodedIngressTracking READ hasEncodedIngressTracking NOTIFY encodedIngressTrackingChanged)
  Q_PROPERTY(int traceTargetFrameId READ traceTargetFrameId WRITE setTraceTargetFrameId NOTIFY
                 traceTargetFrameIdChanged)
  Q_PROPERTY(int sampledHighlightFrameId READ sampledHighlightFrameId NOTIFY sampledPipelineStatsChanged)
  Q_PROPERTY(double sampledDecodeToRenderMs READ sampledDecodeToRenderMs NOTIFY sampledPipelineStatsChanged)
  Q_PROPERTY(double sampledWallOnFrameToRenderMs READ sampledWallOnFrameToRenderMs NOTIFY
                 sampledPipelineStatsChanged)
  Q_PROPERTY(QString sampledPipelineLine READ sampledPipelineLine NOTIFY sampledPipelineStatsChanged)
  Q_PROPERTY(bool hasSampledPipelineUi READ hasSampledPipelineUi NOTIFY sampledPipelineStatsChanged)
  QML_ELEMENT

 public:
  explicit WebRTCVideoRenderer(QQuickItem* parent = nullptr);
  ~WebRTCVideoRenderer() override;

  void OnFrame(const webrtc::VideoFrame& frame) override;

  Q_INVOKABLE void setVideoTrack(webrtc::scoped_refptr<webrtc::VideoTrackInterface> track);
  Q_INVOKABLE void clearVideoTrack();

  bool hasVideo() const { return m_hasVideo.load(std::memory_order_acquire); }
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

  Q_INVOKABLE void applySampledPipelineUi(int glTraceFrameId, double decodeToRenderTotalMs,
                                          double wallOnFrameToRenderMs);

  bool takeFrame(webrtc::scoped_refptr<webrtc::VideoFrameBuffer>& outBuffer,
                 int64_t& outQueueStartMonoUs, int64_t& outGuiUpdateDispatchMonoUs,
                 int& outFrameId, bool& outFromTracking);

 protected:
  void timerEvent(QTimerEvent* event) override;
  QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;
  void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

 private:
  webrtc::scoped_refptr<webrtc::VideoTrackInterface> m_track;
  std::atomic<bool> m_hasVideo{false};

  mutable QMutex m_frameMutex;
  webrtc::scoped_refptr<webrtc::VideoFrameBuffer> m_pendingBuffer;
  bool m_pendingValid = false;
  bool m_updatePending = false;

  int m_highlightFrameId = -1;
  bool m_frameIdFromTracking = false;
  uint32_t m_localPreviewSeq = 0;

  int m_frameCount = 0;
  QElapsedTimer m_decodeIntervalTimer;

  int m_lastPolledEncodedIngressId = -1;
  int m_traceTargetFrameId = -1;

  int64_t m_pendingGlQueueTraceStartMonoUs = 0;
  int64_t m_pendingGuiUpdateDispatchMonoUs = 0;
  int m_pendingGlQueueTraceFrameId = -1;
  bool m_pendingGlQueueTraceFromTracking = false;
  int64_t m_inFlightGuiUpdateDispatchMonoUs = 0;
  int64_t m_lastHighlightSignalMonoUs = 0;

  int m_sampledHighlightFrameId = -1;
  double m_sampledDecodeToRenderMs = -1.0;
  double m_sampledWallOnFrameToRenderMs = -1.0;
  bool m_hasSampledPipelineUi = false;
};

#endif
