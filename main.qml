import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import LibwebRtcDemo 1.0

Window {
    id: root
    width: 420
    height: 560
    visible: true
    title: qsTr("LibWebRTC Demo")
    color: "#1A1A2E"
    minimumWidth: 360
    minimumHeight: 480

    property string receiverStatus: "未连接"
    // 方向3: 启动后延迟 1.5 秒再允许连接，确保 Activity/JNI 就绪后再连信令
    property bool connectReady: false
    // 信令地址：TCP 直连信令服务器（JSON-per-line 协议）
    property string signalingUrl: "192.168.3.20:8765"

    Component.onCompleted: {
        receiverStatus = "方向3 验证: 等待 Activity 就绪 (1.5s)..."
        connectReadyTimer.start()
    }
    Timer {
        id: connectReadyTimer
        interval: 1500
        repeat: false
        onTriggered: {
            connectReady = true
            receiverStatus = "未连接"
        }
    }

    Connections {
        target: receiverClient
        function onStatusChanged(status) {
            receiverStatus = status
        }
    }

    ScrollView {
        id: scrollView
        anchors.fill: parent
        anchors.margins: 20
        clip: true
        ScrollBar.vertical.policy: ScrollBar.AsNeeded
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        contentWidth: availableWidth
        contentHeight: contentLayout.implicitHeight

        ColumnLayout {
            id: contentLayout
            width: scrollView.availableWidth
            spacing: 16

            // ---------- 标题区 ----------
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 48

                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: "LibWebRTC Demo"
                    font.pixelSize: 22
                    font.bold: true
                    color: "#E94560"
                }

                Text {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: "接收端"
                    font.pixelSize: 12
                    color: "#6B7280"
                }
            }

            // ---------- 视频预览区（WebRTC 渲染器）----------
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 240
                Layout.minimumHeight: 160
                radius: 12
                color: "#0F1629"
                border.color: "#0F3460"
                border.width: 1
                clip: true

                WebRTCVideoRenderer {
                    id: videoRenderer
                    anchors.fill: parent
                    anchors.margins: 2

                    Component.onCompleted: {
                        receiverClient.setVideoRenderer(videoRenderer)
                    }
                }

                Text {
                    anchors.centerIn: parent
                    text: videoRenderer.hasVideo ? "" : "视频将在此显示"
                    font.pixelSize: 14
                    color: "#4B5563"
                    visible: !videoRenderer.hasVideo
                }
            }

            // ---------- 控制区 ----------
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 180
                radius: 12
                color: "#16213E"
                border.color: "#0F3460"
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 12

                    // 状态行
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Rectangle {
                            width: 8
                            height: 8
                            radius: 4
                            color: receiverStatus.indexOf("已连接") >= 0 ? "#22C55E" : "#6B7280"
                        }
                        Text {
                            Layout.fillWidth: true
                            text: receiverStatus
                            font.pixelSize: 13
                            color: "#E5E7EB"
                            wrapMode: Text.WordWrap
                            elide: Text.ElideRight
                            maximumLineCount: 2
                        }
                    }

                    // 信令服务器地址（可编辑，连接时使用 urlField.text）
                    TextField {
                        id: urlField
                        Layout.fillWidth: true
                        Layout.preferredHeight: 36
                        placeholderText: "host:port (如 192.168.3.20:8765)"
                        text: root.signalingUrl
                        font.pixelSize: 11
                        color: "#E5E7EB"
                        background: Rectangle {
                            color: "#0F1629"
                            border.color: "#0F3460"
                            radius: 6
                        }
                    }

                    // 验证诊断：logcat 过滤 VERIFY（含延迟初始化、纯视频无录音权限、ABI 等）
                    Button {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 32
                        text: "验证诊断 (输出到 logcat)"
                        onClicked: receiverClient.runVerificationDiagnostic()
                        background: Rectangle {
                            radius: 6
                            color: parent.pressed ? "#4B5563" : "#374151"
                        }
                        contentItem: Text {
                            text: parent.text
                            color: "#9CA3AF"
                            font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }

                    // // 编解码器信息打印：输出到 logcat，过滤 CODEC 查看
                    // Button {
                    //     Layout.fillWidth: true
                    //     Layout.preferredHeight: 32
                    //     text: "打印编解码器信息"
                    //     onClicked: codecPrinter.printAllCodecs()
                    //     background: Rectangle {
                    //         radius: 6
                    //         color: parent.pressed ? "#4B5563" : "#374151"
                    //     }
                    //     contentItem: Text {
                    //         text: parent.text
                    //         color: "#9CA3AF"
                    //         font.pixelSize: 12
                    //         horizontalAlignment: Text.AlignHCenter
                    //         verticalAlignment: Text.AlignVCenter
                    //     }
                    // }

                    // 按钮区
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        Button {
                            text: "连接接收"
                            Layout.fillWidth: true
                            Layout.preferredHeight: 44
                            enabled: root.connectReady
                            onClicked: receiverClient.requestPermissionAndConnect(urlField.text)

                            background: Rectangle {
                                radius: 8
                                color: parent.enabled ? (parent.pressed ? "#16A34A" : "#22C55E") : "#374151"
                            }
                            contentItem: Text {
                                text: parent.text
                                color: "white"
                                font.pixelSize: 15
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }

                        Button {
                            text: "断开"
                            Layout.fillWidth: true
                            Layout.preferredHeight: 44
                            onClicked: receiverClient.disconnect()

                            background: Rectangle {
                                radius: 8
                                color: parent.enabled ? (parent.pressed ? "#B91C1C" : "#EF4444") : "#374151"
                            }
                            contentItem: Text {
                                text: parent.text
                                color: "white"
                                font.pixelSize: 15
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                }
            }
        }
    }
}
