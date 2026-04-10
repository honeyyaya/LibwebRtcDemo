import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import LibwebRtcDemo 1.0

Window {
    id: root
    width: 420
    height: 600
    visible: true
    title: qsTr("WebRTC 接收 (WebView)")
    color: "#1A1A2E"
    minimumWidth: 360
    minimumHeight: 480

    property string receiverStatus: "未连接"
    property bool connectReady: false
    property string signalingUrl: "192.168.3.20:8765"
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
        if (Qt.platform.os === "android") {
            receiverStatus = "等待 Activity 就绪 (1.5s)…"
            connectReadyTimer.start()
        } else {
            connectReady = true
            receiverStatus = "未连接"
        }
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
        target: webrtcReceiver
        function onStatusChanged() {
            receiverStatus = webrtcReceiver.status
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

            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 48

                Row {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 14

                    Text {
                        text: "WebView WebRTC"
                        font.pixelSize: 22
                        font.bold: true
                        color: "#E94560"
                    }

                    Text {
                        text: root.titleClockText
                        font.pixelSize: 40
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

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 240
                Layout.minimumHeight: 160
                radius: 12
                color: "#0F1629"
                border.color: "#0F3460"
                border.width: 1
                clip: true

                WebReceiverView {
                    anchors.fill: parent
                    anchors.margins: 2
                }
            }

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
                            maximumLineCount: 3
                        }
                    }

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

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        Button {
                            text: "连接接收"
                            Layout.fillWidth: true
                            Layout.preferredHeight: 44
                            enabled: root.connectReady && webrtcReceiver.webChannelPort > 0
                            onClicked: webrtcReceiver.requestPermissionAndConnect(urlField.text)

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
                            onClicked: webrtcReceiver.disconnect()

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
