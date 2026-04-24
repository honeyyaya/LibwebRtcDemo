#ifndef VIDEO_FRAME_SINK_H
#define VIDEO_FRAME_SINK_H

#include <QByteArray>
#include <QtPlugin>

class VideoFrameSink
{
public:
    virtual ~VideoFrameSink() = default;

    virtual void presentFrame(QByteArray i420, int width, int height, quint32 frameId) = 0;
    virtual void clearVideoTrack() = 0;
};

#define VideoFrameSink_iid "org.qtproject.example.libwebrtcdemo.VideoFrameSink"
Q_DECLARE_INTERFACE(VideoFrameSink, VideoFrameSink_iid)

#endif
