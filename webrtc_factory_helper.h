#ifndef WEBRTC_FACTORY_HELPER_H
#define WEBRTC_FACTORY_HELPER_H

#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"

namespace webrtc {
class Thread;
}

namespace webrtc_demo {

// 创建 PeerConnectionFactory：Dummy ADM + 内置音频编解码 + CreateBuiltinVideoDecoderFactory（全量 WebRTC 时）。
webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> CreatePeerConnectionFactory();

// 调试用：Factory 使用的 network / signaling 线程（signaling 现为专用线程，非 nullptr）。
webrtc::Thread *PeerConnectionFactoryNetworkThread();
webrtc::Thread *PeerConnectionFactorySignalingThread();

}  // namespace webrtc_demo

#endif
