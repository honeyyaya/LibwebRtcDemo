#ifndef WEBRTC_VIDEO_RENDERER_H
#define WEBRTC_VIDEO_RENDERER_H

#include <QQuickPaintedItem>
#include <QImage>
#include <QMutex>
#include <QElapsedTimer>

#include "libwebrtc.h"
#include "rtc_video_track.h"
#include "rtc_video_frame.h"
#include "rtc_video_renderer.h"

/**
 * @brief WebRTC 视频渲染器
 *
 * 实现 libwebrtc::RTCVideoRenderer，将接收到的视频帧绘制到 QML 界面。
 * 在 OnFrame 回调中将 YUV 转换为 ARGB，并通过 QQuickPaintedItem 渲染。
 */
class WebRTCVideoRenderer
    : public QQuickPaintedItem
    , public libwebrtc::RTCVideoRenderer<libwebrtc::scoped_refptr<libwebrtc::RTCVideoFrame>>
{
    Q_OBJECT
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY hasVideoChanged)
    QML_ELEMENT

public:
    explicit WebRTCVideoRenderer(QQuickItem *parent = nullptr);
    ~WebRTCVideoRenderer() override;

    // RTCVideoRenderer 接口：WebRTC 线程回调
    void OnFrame(libwebrtc::scoped_refptr<libwebrtc::RTCVideoFrame> frame) override;

    // 设置/清除视频轨道（从 remoteVideoTrackReady 信号传入）
    Q_INVOKABLE void setVideoTrack(libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack> track);
    Q_INVOKABLE void clearVideoTrack();

    bool hasVideo() const { return m_hasVideo; }
    Q_SIGNAL void hasVideoChanged();

    void paint(QPainter *painter) override;

private:
    void updateFrameImage(libwebrtc::scoped_refptr<libwebrtc::RTCVideoFrame> frame, qint64 tDecodeUs);

    libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack> m_track;
    QImage m_image;
    QMutex m_imageMutex;
    bool m_hasVideo = false;
    int m_frameCount = 0;  // 用于耗时打印采样（每 N 帧输出一次）
    QElapsedTimer m_decodeIntervalTimer;   // 帧间隔计时（OnFrame 调用间隔）
    QElapsedTimer m_receiveTimer;          // 接收时刻，用于计算线程队列延迟
    qint64 m_lastFrameTotalUs = 0;         // 上帧处理总耗时（不含 paint），供 paint 打印全链路
};

#endif // WEBRTC_VIDEO_RENDERER_H
