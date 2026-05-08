import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import LibwebRtcDemo 1.0

Window {
    id: root
    width: 420
    height: 600
    visible: true
    title: qsTr("libRoboFlow Demo")
    color: "#1A1A2E"
    minimumWidth: 360
    minimumHeight: 480

    property string receiverStatus: "未连接"
    property bool connectReady: false
    property string signalingUrl: "192.168.3.20:8765"
    property string deviceId: ""
    property string deviceSecret: ""
    property int streamIndex: 0
    property string titleClockText: ""

    Timer {
        id: titleClockTimer
        interval: 33
        running: true
        repeat: true
        triggeredOnStart: true
        onTriggered: {
            titleClockText = Qt.formatDateTime(new Date(), "yyyy-MM-dd hh:mm:ss.zzz")
        }
    }

    Component.onCompleted: {
        receiverStatus = "等待界面与 SDK 就绪..."
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

                Row {
                    id: titleRow
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 14

                    Text {
                        text: "libRoboFlow Demo"
                        font.pixelSize: 22
                        font.bold: true
                        color: "#E94560"
                    }

                    Text {
                        text: root.titleClockText
                        font.pixelSize: 22
                        font.bold: true
                        font.family: "monospace"
                        color: "#E94560"
                    }
                }

                Text {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                        text: "客户端"
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

                Timer {
                    id: statsHudTimer
                    interval: 1000
                    repeat: true
                    triggeredOnStart: true
                    running: receiverClient.hasConnectionStats
                             || receiverClient.hasDecodeJitterBuffer
                             || videoRenderer.hasVideo
                             || videoRenderer.hasSampledPipelineUi
                    onTriggered: {
                        let block = receiverClient.overlayTelemetryText()
                        if (videoRenderer.hasSampledPipelineUi)
                            block += "\n" + videoRenderer.sampledPipelineLine
                        videoHud.statsText = block
                    }
                }

                Rectangle {
                    id: videoHud
                    property string statsText: ""

                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.margins: 8
                    width: Math.min(parent.width - 16, 360)
                    height: Math.max(hudLabel.contentHeight + 12, 40)
                    radius: 6
                    color: "#CC0B1220"
                    border.width: 1
                    border.color: "#334155"
                    visible: statsHudTimer.running
                             && (receiverClient.hasConnectionStats
                                 || receiverClient.hasDecodeJitterBuffer
                                 || videoRenderer.hasVideo
                                 || videoRenderer.hasSampledPipelineUi)

                    Text {
                        id: hudLabel
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 6
                        text: videoHud.statsText
                        font.pixelSize: 11
                        font.family: "monospace"
                        color: "#E2E8F0"
                        wrapMode: Text.WordWrap
                    }
                }

                // 帧 ID：青色=仅编码入站（与 log 一致，解码失败时仍有）；绿/橙=解码后 VideoFrame::id()
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 1
                    radius: 10
                    color: "transparent"
                    border.width: (videoRenderer.hasVideo && videoRenderer.highlightFrameId >= 0) ? 2 : 0
                    border.color: (videoRenderer.hasVideo && videoRenderer.highlightFrameId >= 0)
                                  ? "#F59E0B" : "#00000000"
                    visible: videoRenderer.hasVideo
                }

                Column {
                    anchors.centerIn: parent
                    width: parent.width - 24
                    spacing: 6
                    visible: !videoRenderer.hasVideo
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "视频将在此显示"
                        font.pixelSize: 14
                        color: "#4B5563"
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "视频将通过 libRoboFlow SDK 的 on_video 回调直接渲染"
                        font.pixelSize: 10
                        color: "#6B7280"
                        horizontalAlignment: Text.AlignHCenter
                        width: parent.width
                        wrapMode: Text.WordWrap
                    }
                }
            }

            // ---------- 控制区 ----------
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 312
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
                            color: (receiverStatus.indexOf("已连接") >= 0
                                    || receiverStatus.indexOf("成功") >= 0
                                    || receiverStatus.indexOf("接收视频") >= 0) ? "#22C55E" : "#6B7280"
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


                    TextField {
                        id: urlField
                        Layout.fillWidth: true
                        Layout.preferredHeight: 36
                        placeholderText: "signal url / host:port"
                        text: root.signalingUrl
                        font.pixelSize: 11
                        color: "#E5E7EB"
                        background: Rectangle {
                            color: "#0F1629"
                            border.color: "#0F3460"
                            radius: 6
                        }
                    }

                    TextField {
                        id: deviceIdField
                        Layout.fillWidth: true
                        Layout.preferredHeight: 36
                        placeholderText: "device_id（可选）"
                        text: root.deviceId
                        font.pixelSize: 11
                        color: "#E5E7EB"
                        background: Rectangle {
                            color: "#0F1629"
                            border.color: "#0F3460"
                            radius: 6
                        }
                    }

                    TextField {
                        id: deviceSecretField
                        Layout.fillWidth: true
                        Layout.preferredHeight: 36
                        placeholderText: "device_secret（可选）"
                        text: root.deviceSecret
                        echoMode: TextInput.Password
                        font.pixelSize: 11
                        color: "#E5E7EB"
                        background: Rectangle {
                            color: "#0F1629"
                            border.color: "#0F3460"
                            radius: 6
                        }
                    }

                    TextField {
                        id: streamIndexField
                        Layout.fillWidth: true
                        Layout.preferredHeight: 36
                        placeholderText: "stream index"
                        text: root.streamIndex.toString()
                        inputMethodHints: Qt.ImhDigitsOnly
                        font.pixelSize: 11
                        color: "#E5E7EB"
                        background: Rectangle {
                            color: "#0F1629"
                            border.color: "#0F3460"
                            radius: 6
                        }
                    }

                    Button {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 32
                        text: "输出诊断日志"
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

                    // 按钮区
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        Button {
                            text: "连接接收"
                            Layout.fillWidth: true
                            Layout.preferredHeight: 44
                            enabled: root.connectReady
                            onClicked: {
                                receiverClient.deviceId = deviceIdField.text
                                receiverClient.deviceSecret = deviceSecretField.text
                                receiverClient.streamIndex = Number(streamIndexField.text || "0")
                                receiverClient.requestPermissionAndConnect(urlField.text)
                            }

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
