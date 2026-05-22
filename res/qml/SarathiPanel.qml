import QtQuick 2.12
import QtQuick.Controls
import QtQuick.Layouts
import "Theme"

// Sarathi Mode booth panel — DJ Treta's live transition suggestion, shown
// inside Mixxx. Manish drives transitions on the FLX4; Treta suggests. This
// bar appears when sarathi_mode is on, surfaces her current suggestion, and
// lets him tap "Do it" (delegate the transition back to her) or "No".
//
// Transport: the DJ Treta daemon serves plain HTTP on :7779 (the WS server's
// process_request hook). QML's XMLHttpRequest does GET only:
//   GET /http/state                          → current state snapshot
//   GET /http/command?cmd=confirm_transition → fire her suggestion
//   GET /http/command?cmd=reject_transition  → drop it + reshape
// No QtWebSockets dependency; one transport for read + write.
Rectangle {
    id: sarathi

    property string daemonBase: "http://localhost:7779"
    property bool sarathiMode: false
    property var suggestion: null
    property string lastMsg: ""

    width: parent ? parent.width : 800
    implicitHeight: sarathiMode ? 46 : 0
    height: implicitHeight
    visible: sarathiMode
    clip: true
    color: suggestion ? "#2a1840" : Theme.toolbarBackgroundColor

    function poll() {
        var xhr = new XMLHttpRequest();
        xhr.onreadystatechange = function () {
            if (xhr.readyState === XMLHttpRequest.DONE && xhr.status === 200) {
                try {
                    var d = JSON.parse(xhr.responseText);
                    sarathi.sarathiMode = !!d.sarathi_mode;
                    sarathi.suggestion = d.pending_suggestion || null;
                } catch (e) {}
            }
        };
        xhr.open("GET", daemonBase + "/http/state");
        xhr.send();
    }

    function sendCmd(cmd, reason) {
        var xhr = new XMLHttpRequest();
        var url = daemonBase + "/http/command?cmd=" + cmd;
        if (reason && reason.length > 0)
            url += "&reason=" + encodeURIComponent(reason);
        xhr.onreadystatechange = function () {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                try {
                    sarathi.lastMsg = JSON.parse(xhr.responseText).result || "";
                } catch (e) {}
                sarathi.poll();
            }
        };
        xhr.open("GET", url);
        xhr.send();
    }

    Timer {
        interval: 1000
        repeat: true
        running: true
        onTriggered: sarathi.poll()
    }
    Component.onCompleted: poll()

    // Active suggestion → technique/track/reason + Do it / No
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        spacing: 14
        visible: sarathi.suggestion !== null

        Text {
            text: "✋ SARATHI"
            color: Theme.green
            font.bold: true
            font.family: Theme.fontFamily
            font.pixelSize: 13
        }
        Text {
            Layout.fillWidth: true
            elide: Text.ElideRight
            color: Theme.white
            font.family: Theme.fontFamily
            font.pixelSize: 12
            text: {
                if (!sarathi.suggestion)
                    return "";
                var s = sarathi.suggestion;
                var line = (s.technique || "crossfade").toUpperCase().replace(/_/g, " ") + "  →  deck " + s.to_deck;
                if (s.track_title)
                    line += "   ·   " + s.track_title;
                if (s.reason)
                    line += "   —   " + s.reason;
                return line;
            }
        }
        Text {
            visible: sarathi.suggestion && sarathi.suggestion.expires_in_s !== undefined
            color: Theme.lightGray
            font.family: Theme.fontFamily
            font.pixelSize: 12
            text: sarathi.suggestion ? (sarathi.suggestion.expires_in_s + "s") : ""
        }
        Button {
            text: "Do it"
            onClicked: sarathi.sendCmd("confirm_transition", "")
        }
        Button {
            text: "No"
            onClicked: sarathi.sendCmd("reject_transition", "")
        }
    }

    // Sarathi on, nothing suggested yet
    Text {
        anchors.centerIn: parent
        visible: sarathi.sarathiMode && sarathi.suggestion === null
        text: "✋ SARATHI — you have the wheel · no suggestion live"
        color: Theme.lightGray
        font.family: Theme.fontFamily
        font.pixelSize: 12
    }
}
