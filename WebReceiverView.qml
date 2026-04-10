import QtQuick
import QtWebView

// 系统 WebView（Android 为 WebView/Chromium）；WebRTC 在页面内由谷歌栈处理
// 注意：Qt WebView 不支持 qrc，Android 上由 C++ 把页面拷到 AppData 后用 file:// 加载
Item {
    anchors.fill: parent

    WebView {
        anchors.fill: parent
        url: webrtcReceiver.pageUrl

        settings.javaScriptEnabled: true
        settings.allowFileAccess: true
        settings.localContentCanAccessFileUrls: true
    }
}
