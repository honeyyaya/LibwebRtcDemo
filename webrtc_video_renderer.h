#ifndef WEBRTC_VIDEO_RENDERER_H
#define WEBRTC_VIDEO_RENDERER_H

#include <QQuickFramebufferObject>
#include <QMutex>
#include <QElapsedTimer>

#include "api/media_stream_interface.h"
#include "api/scoped_refptr.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_buffer.h"
#include "api/video/video_sink_interface.h"

class WebRTCVideoRenderer : public QQuickFramebufferObject,
                            public webrtc::VideoSinkInterface<webrtc::VideoFrame>
{
    Q_OBJECT
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY hasVideoChanged)
    /// 当前用于高亮的帧标识：来自 VideoFrame::id()（RTP 扩展/Field Trial）时为真实 ID；否则为本地预览序号（未联通服务器时仍可见）。
    Q_PROPERTY(int highlightFrameId READ highlightFrameId NOTIFY highlightFrameIdChanged)
    Q_PROPERTY(bool frameIdFromTracking READ frameIdFromTracking NOTIFY highlightFrameIdChanged)
    /// 编码入站（Decode 路径）最后一帧的 VideoFrameTrackingId；不依赖解码/出画，可与 log 对齐。
    Q_PROPERTY(int encodedIngressTrackingId READ encodedIngressTrackingId NOTIFY
                   encodedIngressTrackingChanged)
    Q_PROPERTY(bool hasEncodedIngressTracking READ hasEncodedIngressTracking NOTIFY
                   encodedIngressTrackingChanged)
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

    Renderer *createRenderer() const override;

    // 传出 I420 引用；纹理上传在 GL 线程完成，避免 OnFrame 整帧 memcpy。
    bool takeFrame(webrtc::scoped_refptr<webrtc::I420BufferInterface> &out);

protected:
    void timerEvent(QTimerEvent *event) override;

private:
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> m_track;
    bool m_hasVideo = false;

    mutable QMutex m_frameMutex;
    webrtc::scoped_refptr<webrtc::I420BufferInterface> m_pendingI420;
    bool m_pendingValid = false;

    int m_highlightFrameId = -1;
    bool m_frameIdFromTracking = false;
    uint32_t m_localPreviewSeq = 0;

    int m_frameCount = 0;
    QElapsedTimer m_decodeIntervalTimer;

    int m_lastPolledEncodedIngressId = -1;
};

#endif
