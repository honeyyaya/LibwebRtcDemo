cmake_minimum_required(VERSION 3.16)

project(LibwebRtcDemo VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.2 COMPONENTS Quick REQUIRED)

set(WEBRTC_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include)
set(WEBRTC_LIB_DIR ${CMAKE_SOURCE_DIR}/lib/arm64-v8a)

qt_add_executable(appLibwebRtcDemo
    main.cpp
    webrtcwrapper.h
    webrtcwrapper.cpp
)

qt_add_qml_module(appLibwebRtcDemo
    URI LibwebRtcDemo
    VERSION 1.0
    QML_FILES main.qml
)

set_target_properties(appLibwebRtcDemo PROPERTIES
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
    QT_ANDROID_EXTRA_LIBS "${WEBRTC_LIB_DIR}/libwebrtc.so;${WEBRTC_LIB_DIR}/libc++_shared.so"
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
        webrtc
)

include(GNUInstallDirs)
install(TARGETS appLibwebRtcDemo
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
