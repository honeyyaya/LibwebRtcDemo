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
    QML_ELEMENT

public:
    explicit WebRTCVideoRenderer(QQuickItem *parent = nullptr);
    ~WebRTCVideoRenderer() override;

    void OnFrame(const webrtc::VideoFrame &frame) override;

    Q_INVOKABLE void setVideoTrack(webrtc::scoped_refptr<webrtc::VideoTrackInterface> track);
    Q_INVOKABLE void clearVideoTrack();

    bool hasVideo() const { return m_hasVideo; }
    Q_SIGNAL void hasVideoChanged();

    Renderer *createRenderer() const override;

    // 传出 I420 引用；纹理上传在 GL 线程完成，避免 OnFrame 整帧 memcpy。
    bool takeFrame(webrtc::scoped_refptr<webrtc::I420BufferInterface> &out);

protected:
    void timerEvent(QTimerEvent *event) override;

private:
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> m_track;
    bool m_hasVideo = false;

    QMutex m_frameMutex;
    webrtc::scoped_refptr<webrtc::I420BufferInterface> m_pendingI420;
    bool m_pendingValid = false;

    int m_frameCount = 0;
    QElapsedTimer m_decodeIntervalTimer;
};

#endif
