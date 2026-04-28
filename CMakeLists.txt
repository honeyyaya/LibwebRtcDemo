cmake_minimum_required(VERSION 3.16)

project(LibwebRtcDemo VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.2 COMPONENTS Quick WebSockets Widgets Network OpenGL REQUIRED)

# 官方 WebRTC 头文件树根目录（api/、rtc_base/、third_party/ 等）
set(WEBRTC_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include" CACHE PATH "WebRTC headers root")
# Android 按 ABI 分目录；桌面可放在 lib/ 下
if(ANDROID)
  set(WEBRTC_LIB_DIR "${CMAKE_SOURCE_DIR}/lib/${ANDROID_ABI}" CACHE PATH "WebRTC prebuilt lib dir")
else()
  set(WEBRTC_LIB_DIR "${CMAKE_SOURCE_DIR}/lib" CACHE PATH "WebRTC prebuilt lib dir")
endif()
# 全量静态库默认名；若你的文件名不同，可在配置时 -DWEBRTC_STATIC_LIB=...
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
)

# Android: 使用自定义 manifest 允许 ws:// 明文流量，并确保包含 QtActivity（解决 Bad component name 和信令错误）
if(ANDROID)
    set_target_properties(appLibwebRtcDemo PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR ${CMAKE_SOURCE_DIR}/android
        QT_ANDROID_APP_NAME "webDemo"
        QT_ANDROID_PACKAGE_NAME "org.qtproject.example.libwebrtcdemo")
endif()

set_target_properties(appLibwebRtcDemo PROPERTIES
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)
# 静态链接 libwebrtc.a 时，符号打进本应用 .so，无需再向 APK 打入 libwebrtc.so

# Chromium 风格头文件树：absl、libyuv 在 third_party 下，需单独加入 include 路径
# libyuv 头文件使用 #include "libyuv/..."，根目录须为 third_party/libyuv/include
target_include_directories(appLibwebRtcDemo PRIVATE
    ${WEBRTC_INCLUDE_DIR}
    ${WEBRTC_INCLUDE_DIR}/third_party/abseil-cpp
    ${WEBRTC_INCLUDE_DIR}/third_party/libyuv/include
)

# WebRTC 头文件依赖这些宏选择字节序等实现（与 Chromium GN 中 rtc:rtc 一致）
# 未定义 WEBRTC_POSIX 时 rtc_base/byte_order.h 会 #error "Missing byte order functions for this arch."
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

# 预编译 WebRTC 静态库必须存在，否则 Ninja 会报 “missing and no known rule to make it”
if(NOT EXISTS "${WEBRTC_STATIC_LIB}")
  message(FATAL_ERROR
    "未找到 WebRTC 静态库: ${WEBRTC_STATIC_LIB}\n"
    "请将你编译/获得的 libwebrtc.a 复制到:\n"
    "  ${WEBRTC_LIB_DIR}/\n"
    "或重新运行 CMake 并指定绝对路径，例如:\n"
    "  -DWEBRTC_STATIC_LIB=D:/path/to/libwebrtc.a\n"
    "（ANDROID_ABI=${ANDROID_ABI} 时默认在 lib/${ANDROID_ABI}/libwebrtc.a）")
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

# Android 上 WebRTC 静态库常依赖的系统库（与 NDK/GLES 等；无音频时不链 OpenSLES）
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
