cmake_minimum_required(VERSION 3.16)

project(LibwebRtcDemo VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Qt WebView：Android 使用系统 WebView（Chromium）；网页内标准 WebRTC（谷歌栈）
# 信令仍为 C++ TCP（SignalingClient）；QWebChannel 经本机 WebSocket 与页面通信
find_package(Qt6 6.2 REQUIRED COMPONENTS
    Quick
    Network
    WebSockets
    WebChannel
    WebView
    WebViewQuick
    Widgets
)

qt_add_executable(appLibwebRtcDemo
    main.cpp
    signaling_client.h
    signaling_client.cpp
    webchannel_websocket_transport.h
    webchannel_websocket_transport.cpp
    webrtc_webview_receiver.h
    webrtc_webview_receiver.cpp
)

qt_add_qml_module(appLibwebRtcDemo
    URI LibwebRtcDemo
    VERSION 1.0
    QML_FILES
        main.qml
        WebReceiverView.qml
    SOURCES
)

qt_add_resources(appLibwebRtcDemo "libwebrtc_demo_web"
    PREFIX "/LibwebRtcDemo"
    FILES
        webreceiver.html
        qwebchannel.js
)

if(ANDROID)
    set_target_properties(appLibwebRtcDemo PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR ${CMAKE_SOURCE_DIR}/android
        QT_ANDROID_PACKAGE_NAME "org.qtproject.example.libwebrtcdemo")
endif()

set_target_properties(appLibwebRtcDemo PROPERTIES
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(appLibwebRtcDemo PRIVATE
    Qt6::Quick
    Qt6::Network
    Qt6::WebSockets
    Qt6::WebChannel
    Qt6::WebView
    Qt6::WebViewQuick
    Qt6::Widgets
)

message(STATUS "LibwebRtcDemo: WebView + 页面 WebRTC（本机 ws QWebChannel），入口 qrc:/LibwebRtcDemo/main.qml")

include(GNUInstallDirs)
install(TARGETS appLibwebRtcDemo
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
