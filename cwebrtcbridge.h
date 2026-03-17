#ifndef CWEBRTCBRIDGE_H
#define CWEBRTCBRIDGE_H
#include <iostream>
#include "libwebrtc.h"

#include <rtc_peerconnection_factory.h>
#include <base/scoped_ref_ptr.h>
class CWebRTCBridge
{
public:
    CWebRTCBridge();

    // 初始化WebRtc
    bool Init();
    void CleanUp();

    //
    bool CreateConnection();

private:
    // 声明工厂入口
    libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnectionFactory> m_factory_ = nullptr;
    bool m_initialized_ =false;
};

#endif // CWEBRTCBRIDGE_H
