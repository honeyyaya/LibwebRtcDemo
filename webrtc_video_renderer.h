#ifndef WEBRTC_VIDEO_RENDERER_H
#define WEBRTC_VIDEO_RENDERER_H

#include <QQuickFramebufferObject>
#include <QByteArray>
#include <QMutex>
#include <QElapsedTimer>

#include "api/media_stream_interface.h"
#include "api/scoped_refptr.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"

struct YuvFrameData {
    QByteArray y, u, v;
    int width = 0;
    int height = 0;
    bool valid = false;
};

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

    bool takeFrame(YuvFrameData &out);

protected:
    void timerEvent(QTimerEvent *event) override;

private:
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> m_track;
    bool m_hasVideo = false;

    QMutex m_frameMutex;
    YuvFrameData m_pendingFrame;

    int m_frameCount = 0;
    QElapsedTimer m_decodeIntervalTimer;
};

#endif
