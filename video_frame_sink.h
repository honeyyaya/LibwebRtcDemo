#ifndef VIDEO_FRAME_SINK_H
#define VIDEO_FRAME_SINK_H

#include <QtPlugin>

extern "C" {
#include "rflow/Client/librflow_client_api.h"
}

class VideoFrameSink
{
public:
    virtual ~VideoFrameSink() = default;

    // Takes ownership of one retained SDK frame reference.
    virtual void presentFrame(librflow_video_frame_t frame) = 0;
    virtual void clearVideoTrack() = 0;
};

#define VideoFrameSink_iid "org.qtproject.example.libwebrtcdemo.VideoFrameSink"
Q_DECLARE_INTERFACE(VideoFrameSink, VideoFrameSink_iid)

#endif
