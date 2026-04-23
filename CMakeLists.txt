cmake_minimum_required(VERSION 3.16)

project(LibwebRtcDemo VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.2 COMPONENTS Quick OpenGL REQUIRED)

if(ANDROID)
  set(RFLOW_LIB_DIR "${CMAKE_SOURCE_DIR}/lib/${ANDROID_ABI}" CACHE PATH "libRoboFlow prebuilt lib dir")
else()
  set(RFLOW_LIB_DIR "${CMAKE_SOURCE_DIR}/lib" CACHE PATH "libRoboFlow prebuilt lib dir")
endif()
set(RFLOW_CLIENT_SHARED_LIB "${RFLOW_LIB_DIR}/librflow_client.so" CACHE FILEPATH "Path to librflow_client.so")

qt_add_executable(appLibwebRtcDemo
    main.cpp
    webrtc_receiver_client.h
    webrtc_receiver_client.cpp
    webrtc_video_renderer.h
    webrtc_video_renderer.cpp
)

qt_add_qml_module(appLibwebRtcDemo
    URI LibwebRtcDemo
    VERSION 1.0
    QML_FILES main.qml
    SOURCES
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
    ${CMAKE_SOURCE_DIR}/include
)

if(NOT EXISTS "${RFLOW_CLIENT_SHARED_LIB}")
  message(FATAL_ERROR
    "未找到 libRoboFlow 客户端库: ${RFLOW_CLIENT_SHARED_LIB}\n"
    "请将 librflow_client.so 放到:\n"
    "  ${RFLOW_LIB_DIR}/\n"
    "或重新运行 CMake 并指定绝对路径，例如:\n"
    "  -DRFLOW_CLIENT_SHARED_LIB=D:/path/to/librflow_client.so")
endif()

add_library(librflow_client SHARED IMPORTED)
set_target_properties(librflow_client PROPERTIES
    IMPORTED_LOCATION "${RFLOW_CLIENT_SHARED_LIB}"
    IMPORTED_NO_SONAME TRUE
)

target_link_libraries(appLibwebRtcDemo
    PRIVATE
        Qt6::Quick
        Qt6::OpenGL
        librflow_client
)

if(ANDROID)
  set_target_properties(appLibwebRtcDemo PROPERTIES
      QT_ANDROID_EXTRA_LIBS "${RFLOW_CLIENT_SHARED_LIB}")
  target_link_libraries(appLibwebRtcDemo PRIVATE
      log
      android
      GLESv2
      EGL
  )
endif()

include(GNUInstallDirs)
install(TARGETS appLibwebRtcDemo
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
