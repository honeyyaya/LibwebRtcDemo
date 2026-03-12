import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Window {
    width: 400
    height: 600
    visible: true
    title: qsTr("LibWebRTC Demo")
    color: "#1A1A2E"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 16

        Text {
            text: "LibWebRTC Demo"
            font.pixelSize: 28
            font.bold: true
            color: "#E94560"
            Layout.alignment: Qt.AlignHCenter
        }

        Text {
            text: webrtc.version
            font.pixelSize: 14
            color: "#8888AA"
            Layout.alignment: Qt.AlignHCenter
        }

        Rectangle {
            Layout.fillWidth: true
            height: 60
            radius: 12
            color: webrtc.initialized ? "#2E7D32" : "#16213E"
            border.color: webrtc.initialized ? "#4CAF50" : "#0F3460"
            border.width: 2

            Text {
                anchors.centerIn: parent
                text: webrtc.initialized ? "INITIALIZED" : "NOT INITIALIZED"
                font.pixelSize: 18
                font.bold: true
                color: webrtc.initialized ? "#C8E6C9" : "#8888AA"
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Button {
                text: "Initialize"
                Layout.fillWidth: true
                enabled: !webrtc.initialized
                onClicked: webrtc.initialize()

                background: Rectangle {
                    radius: 8
                    color: parent.enabled ? (parent.pressed ? "#C62828" : "#E94560") : "#444"
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
                text: "Cleanup"
                Layout.fillWidth: true
                enabled: webrtc.initialized
                onClicked: webrtc.cleanup()

                background: Rectangle {
                    radius: 8
                    color: parent.enabled ? (parent.pressed ? "#0D47A1" : "#0F3460") : "#444"
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

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: 12
            color: "#16213E"
            border.color: "#0F3460"
            border.width: 1

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 4

                Text {
                    text: "LOG"
                    font.pixelSize: 12
                    font.bold: true
                    color: "#8888AA"
                }

                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true

                    TextArea {
                        readOnly: true
                        text: webrtc.statusLog
                        color: "#C0C0D0"
                        font.family: "monospace"
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                        background: null
                    }
                }
            }
        }
    }
}
