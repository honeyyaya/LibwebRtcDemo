cmake_minimum_required(VERSION 3.16)

project(LibwebRtcDemo VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.2 COMPONENTS Quick WebSockets Widgets Network OpenGL REQUIRED)

set(WEBRTC_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include" CACHE PATH "WebRTC headers root")
if(ANDROID)
  set(WEBRTC_LIB_DIR "${CMAKE_SOURCE_DIR}/lib/${ANDROID_ABI}" CACHE PATH "WebRTC prebuilt lib dir")
else()
  set(WEBRTC_LIB_DIR "${CMAKE_SOURCE_DIR}/lib" CACHE PATH "WebRTC prebuilt lib dir")
endif()
set(WEBRTC_STATIC_LIB "${WEBRTC_LIB_DIR}/libwebrtc.a" CACHE FILEPATH "Path to libwebrtc.a")

qt_add_executable(appLibwebRtcDemo
    main.cpp
    webrtc_factory_helper.h
    webrtc_factory_helper.cpp
    webrtc_receiver_client.h
    webrtc_receiver_client.cpp
    webrtc_video_renderer.h
    webrtc_video_renderer.cpp
    video_decode_sink_timing_bridge.h
    video_decode_sink_timing_bridge.cpp
    encoded_tracking_bridge.h
    encoded_tracking_bridge.cpp
    signaling_client.h
    signaling_client.cpp
)

qt_add_qml_module(appLibwebRtcDemo
    URI LibwebRtcDemo
    VERSION 1.0
    QML_FILES main.qml
    SOURCES
        webrtc_video_renderer.h
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

target_include_directories(appLibwebRtcDemo PRIVATE
    ${WEBRTC_INCLUDE_DIR}
    ${WEBRTC_INCLUDE_DIR}/third_party/abseil-cpp
    ${WEBRTC_INCLUDE_DIR}/third_party/libyuv/include
)

set(_webrtc_platform_defs QT_NO_KEYWORDS)
if(ANDROID)
  list(APPEND _webrtc_platform_defs WEBRTC_POSIX WEBRTC_ANDROID)
elseif(APPLE)
  list(APPEND _webrtc_platform_defs WEBRTC_MAC)
elseif(WIN32)
  list(APPEND _webrtc_platform_defs WEBRTC_WIN)
else()
  list(APPEND _webrtc_platform_defs WEBRTC_POSIX)
endif()
target_compile_definitions(appLibwebRtcDemo PRIVATE ${_webrtc_platform_defs})

if(NOT EXISTS "${WEBRTC_STATIC_LIB}")
  message(FATAL_ERROR
    "WebRTC static library not found: ${WEBRTC_STATIC_LIB}\n"
    "Place libwebrtc.a in:\n"
    "  ${WEBRTC_LIB_DIR}/\n"
    "or rerun CMake with:\n"
    "  -DWEBRTC_STATIC_LIB=D:/path/to/libwebrtc.a\n")
endif()

add_library(webrtc STATIC IMPORTED)
set_target_properties(webrtc PROPERTIES
    IMPORTED_LOCATION "${WEBRTC_STATIC_LIB}"
    IMPORTED_LINK_INTERFACE_LANGUAGES CXX
)

target_link_libraries(appLibwebRtcDemo
    PRIVATE
        Qt6::Quick
        Qt6::WebSockets
        Qt6::Widgets
        Qt6::Network
        Qt6::OpenGL
        webrtc
)

if(ANDROID)
  target_sources(appLibwebRtcDemo PRIVATE
      android_hw_video_decoder_factory.cpp
      android_mediacodec_video_decoder.cpp
  )
  target_link_libraries(appLibwebRtcDemo PRIVATE
      log
      android
      GLESv2
      EGL
      mediandk
  )
endif()

include(GNUInstallDirs)
install(TARGETS appLibwebRtcDemo
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
