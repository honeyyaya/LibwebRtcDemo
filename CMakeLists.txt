cmake_minimum_required(VERSION 3.16)

project(LibwebRtcDemo VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.2 COMPONENTS Quick WebSockets Widgets Network REQUIRED)

set(WEBRTC_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include)
set(WEBRTC_LIB_DIR ${CMAKE_SOURCE_DIR}/lib/arm64-v8a)

qt_add_executable(appLibwebRtcDemo
    main.cpp
    webrtcwrapper.h
    webrtcwrapper.cpp
    webrtc_receiver_client.h
    webrtc_receiver_client.cpp
    webrtc_video_renderer.h
    webrtc_video_renderer.cpp
    signaling_client.h
    signaling_client.cpp
)

qt_add_qml_module(appLibwebRtcDemo
    URI LibwebRtcDemo
    VERSION 1.0
    QML_FILES main.qml
    SOURCES cwebrtcbridge.h cwebrtcbridge.cpp
)

# Android: 使用自定义 manifest 允许 ws:// 明文流量，并确保包含 QtActivity（解决 Bad component name 和信令错误）
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
    QT_ANDROID_EXTRA_LIBS "${WEBRTC_LIB_DIR}/libwebrtc.so"
)

target_include_directories(appLibwebRtcDemo PRIVATE
    ${WEBRTC_INCLUDE_DIR}
)

target_compile_definitions(appLibwebRtcDemo PRIVATE
    QT_NO_KEYWORDS
)

add_library(webrtc SHARED IMPORTED)
set_target_properties(webrtc PROPERTIES
    IMPORTED_LOCATION ${WEBRTC_LIB_DIR}/libwebrtc.so
)

target_link_libraries(appLibwebRtcDemo
    PRIVATE
        Qt6::Quick
        Qt6::WebSockets
        Qt6::Widgets
        Qt6::Network
        webrtc
)

include(GNUInstallDirs)
install(TARGETS appLibwebRtcDemo
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
