#include "cwebrtcbridge.h"
#include <rtc_mediaconstraints.h>
CWebRTCBridge::CWebRTCBridge() {}

bool CWebRTCBridge::Init()
{
    if (m_initialized_) {
        return true;
    }

    bool ok = libwebrtc::LibWebRTC::Initialize();
    std::cout<<"The result of initialize is ["<<ok<<"]"<<std::endl;

    if (!ok) {
        return false;
    }

    m_factory_ = libwebrtc::LibWebRTC::CreateRTCPeerConnectionFactory();
    bool factoryOk = m_factory_.get() != nullptr;

    if (!factoryOk) {
        libwebrtc::LibWebRTC::Terminate();
        return false;
    }


    m_initialized_ = true;
    return true;
}

void CWebRTCBridge::CleanUp()
{
    if (!m_initialized_)
        return;

    m_factory_ = nullptr;
    libwebrtc::LibWebRTC::Terminate();

    m_initialized_ = false;
}

bool CWebRTCBridge::CreateConnection()
{
    if(!m_initialized_)
    {
        return false;
    }

    m_factory_->Initialize();

    // Create configuration
    libwebrtc::RTCConfiguration config;
    // Set up ICE servers, etc.

    // // Create constraints
    // auto constraints = libwebrtc::RTCMediaConstraints::Create();


    // // Create peer connection
    // auto peer_connection = m_factory_->Create(config, constraints);
    return true;
}

