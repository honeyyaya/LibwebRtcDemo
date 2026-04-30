import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import LibwebRtcDemo 1.0

Window {
    id: root
    width: 420
    height: 600
    visible: true
    title: qsTr("webDemo")
    color: "#1A1A2E"
    minimumWidth: 360
    minimumHeight: 480

    property string receiverStatus: "未连接"
    // 方向3: 启动后延迟 1.5 秒再允许连接，确保 Activity/JNI 就绪后再连信令
    property bool connectReady: false
    // 信令地址：TCP 直连信令服务器（JSON-per-line 协议）
    property string signalingUrl: "192.168.3.20:8765?stream_id=demo_device:0"
    property string titleClockText: ""

    Timer {
        id: titleClockTimer
        // 毫秒位需高频刷新；1000ms 只会每秒改一次，看起来像“表坏了”
        interval: 33
        running: true
        repeat: true
        triggeredOnStart: true
        onTriggered: {
            titleClockText = Qt.formatDateTime(new Date(), "yyyy-MM-dd hh:mm:ss.zzz")
        }
    }

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

                Row {
                    id: titleRow
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 14

                    Text {
                        text: "webDemo"
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

                // 帧 ID：青色=仅编码入站（与 log 一致，解码失败时仍有）；绿/橙=解码后 VideoFrame::id()
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 1
                    radius: 10
                    color: "transparent"
                    border.width: (videoRenderer.hasEncodedIngressTracking
                                   || (videoRenderer.hasVideo && videoRenderer.highlightFrameId >= 0)) ? 2 : 0
                    border.color: (videoRenderer.hasVideo && videoRenderer.highlightFrameId >= 0)
                                  ? (videoRenderer.frameIdFromTracking ? "#22C55E" : "#F59E0B")
                                  : (videoRenderer.hasEncodedIngressTracking ? "#06B6D4" : "#00000000")
                    visible: videoRenderer.hasEncodedIngressTracking || videoRenderer.hasVideo
                }

                Rectangle {
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.margins: 10
                    implicitWidth:Math.max(bufferline.implicitWidth,rttline.implicitWidth,idPipeline.implicitWidth) + 16
                    implicitHeight: idColumn.implicitHeight + 16
                    radius: 4
                    color: "#B3000000"
                    visible: videoRenderer.hasEncodedIngressTracking
                             || (videoRenderer.hasVideo && videoRenderer.highlightFrameId >= 0)
                             || videoRenderer.hasSampledPipelineUi
                    Row {
                        id: idColumn
                        anchors.centerIn: parent
                        spacing: 4
                        // Text {
                        //     id: idIngress
                        //     width: Math.min(280, root.width - 64)
                        //     visible: videoRenderer.hasEncodedIngressTracking
                        //     wrapMode: Text.WordWrap
                        //     text: "编码入站 ID: " + videoRenderer.encodedIngressTrackingId
                        //           + "（Decode 路径，与 logcat EncodedFrame 一致）"
                        //     font.pixelSize: 11
                        //     font.bold: true
                        //     color: "#A5F3FC"
                        // }
                        // Text {
                        //     id: idDecoded
                        //     width: Math.min(280, root.width - 64)
                        //     visible: videoRenderer.hasVideo && videoRenderer.highlightFrameId >= 0
                        //     wrapMode: Text.WordWrap
                        //     text: !videoRenderer.hasVideo || videoRenderer.highlightFrameId < 0
                        //           ? ""
                        //           : (videoRenderer.frameIdFromTracking
                        //              ? ("解码帧 ID: " + videoRenderer.highlightFrameId)
                        //              : ("解码预览 #" + videoRenderer.highlightFrameId))
                        //     font.pixelSize: 11
                        //     font.bold: videoRenderer.frameIdFromTracking
                        //     color: videoRenderer.frameIdFromTracking ? "#BBF7D0" : "#FDE68A"
                        // }
                        Text {
                            id: idPipeline
                            width: Math.min(280, root.width - 64)
                            visible: videoRenderer.hasSampledPipelineUi
                            wrapMode: Text.WordWrap
                            text: videoRenderer.sampledPipelineLine
                            font.pixelSize: 11
                            font.bold: true
                            color: "#93C5FD"
                        }

                        Text {
                            id:bufferline
                            Layout.fillWidth: true
                            font.pixelSize: 11
                            font.family: "monospace"
                            color: "#9CA3AF"
                            wrapMode: Text.WordWrap
                            text: receiverClient.hasConnectionStats
                                  ? ("抖动缓冲(帧平均): "
                                     + receiverClient.jitterBufferMs.toFixed(1) + " ms")
                                  : "抖动缓冲(帧平均): —"
                        }
                        Text {
                            id:rttline
                            Layout.fillWidth: true
                            font.pixelSize: 11
                            font.family: "monospace"
                            color: "#9CA3AF"
                            wrapMode: Text.WordWrap
                            text: receiverClient.hasConnectionStats
                                  ? ("RTT 当前: " + receiverClient.rttCurrentMs.toFixed(1) + " ms"
                                     + "  |  平均: " + receiverClient.rttAvgMs.toFixed(1) + " ms")
                                  : "RTT 当前 / 平均: —"
                        }
                    }


                }

                Column {
                    anchors.centerIn: parent
                    width: parent.width - 24
                    spacing: 6
                    visible: !videoRenderer.hasVideo && !videoRenderer.hasEncodedIngressTracking
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "视频将在此显示"
                        font.pixelSize: 14
                        color: "#4B5563"
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "青框+编码入站 ID=仅进解码器（黑屏时仍可见）；绿/橙=已解码出画"
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
                Layout.preferredHeight: 228
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
                        placeholderText: "host:port?stream_id=demo_device:0"
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
