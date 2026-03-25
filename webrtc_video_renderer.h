#ifndef WEBRTC_VIDEO_RENDERER_H
#define WEBRTC_VIDEO_RENDERER_H

#include <QQuickFramebufferObject>
#include <QByteArray>
#include <QMutex>
#include <QElapsedTimer>

#include "libwebrtc.h"
#include "rtc_video_track.h"
#include "rtc_video_frame.h"
#include "rtc_video_renderer.h"

struct YuvFrameData {
    QByteArray y, u, v;
    int width = 0;
    int height = 0;
    bool valid = false;
};

class WebRTCVideoRenderer
    : public QQuickFramebufferObject
    , public libwebrtc::RTCVideoRenderer<libwebrtc::scoped_refptr<libwebrtc::RTCVideoFrame>>
{
    Q_OBJECT
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY hasVideoChanged)
    QML_ELEMENT

public:
    explicit WebRTCVideoRenderer(QQuickItem *parent = nullptr);
    ~WebRTCVideoRenderer() override;

    void OnFrame(libwebrtc::scoped_refptr<libwebrtc::RTCVideoFrame> frame) override;

    Q_INVOKABLE void setVideoTrack(libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack> track);
    Q_INVOKABLE void clearVideoTrack();

    bool hasVideo() const { return m_hasVideo; }
    Q_SIGNAL void hasVideoChanged();

    Renderer *createRenderer() const override;

    bool takeFrame(YuvFrameData &out);

protected:
    void timerEvent(QTimerEvent *event) override;

private:
    libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack> m_track;
    bool m_hasVideo = false;

    QMutex m_frameMutex;
    YuvFrameData m_pendingFrame;

    int m_frameCount = 0;
    QElapsedTimer m_decodeIntervalTimer;
};

#endif // WEBRTC_VIDEO_RENDERER_H
