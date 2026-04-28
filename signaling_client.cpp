#include "signaling_client.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <sstream>

namespace webrtc_demo {

namespace {

constexpr const char* kDefaultDemoDeviceId = "demo_device";
constexpr int kDefaultDemoStreamIndex = 0;

QString NormalizeAddressForUrl(const std::string& addr) {
    QString input = QString::fromStdString(addr);
    if (!input.contains("://")) {
        input.prepend("tcp://");
    }
    return input;
}

void ParseHostPort(const std::string& addr, std::string& host, uint16_t& port) {
    const QUrl url(NormalizeAddressForUrl(addr));
    if (url.isValid() && !url.host().isEmpty()) {
        host = url.host().toStdString();
        port = static_cast<uint16_t>(url.port(8765));
        return;
    }

    std::string s = addr;
    const size_t query_pos = s.find('?');
    if (query_pos != std::string::npos) {
        s = s.substr(0, query_pos);
    }
    if (s.find("ws://") == 0) {
        s = s.substr(5);
    } else if (s.find("tcp://") == 0) {
        s = s.substr(6);
    }
    const size_t colon = s.rfind(':');
    if (colon != std::string::npos) {
        host = s.substr(0, colon);
        port = static_cast<uint16_t>(std::atoi(s.c_str() + colon + 1));
    } else {
        host = s;
        port = 8765;
    }
}

std::string JsonValueToStdString(const QJsonValue& value) {
    return value.isString() ? value.toString().toStdString() : std::string();
}

QString PreviewLine(const std::string& line, int limit = 240) {
    QString text = QString::fromStdString(line);
    text.replace("\r", "\\r");
    text.replace("\n", "\\n");
    if (text.size() > limit) {
        text = text.left(limit) + "...";
    }
    return text;
}

int DialTcp(const std::string& host, uint16_t port) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    const std::string port_str = std::to_string(static_cast<int>(port));
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0) {
        return -1;
    }

    int fd = -1;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (::connect(fd, p->ai_addr, static_cast<socklen_t>(p->ai_addrlen)) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    return fd;
}

}  // namespace

SignalingClient::SignalingClient(const std::string& server_addr, const std::string& role)
    : server_addr_(server_addr), role_(role) {
    ParseHostPort(server_addr, host_, port_);

    const QUrl url(NormalizeAddressForUrl(server_addr));
    if (url.isValid()) {
        const QUrlQuery query(url);
        stream_id_ = query.queryItemValue("stream_id").toStdString();
        device_id_ = query.queryItemValue("device_id").toStdString();
        bool ok = false;
        const int stream_index = query.queryItemValue("stream_index").toInt(&ok);
        if (ok) {
            stream_index_ = stream_index;
        }
        if (!stream_id_.empty()) {
            const size_t colon = stream_id_.rfind(':');
            if (colon != std::string::npos) {
                if (device_id_.empty()) {
                    device_id_ = stream_id_.substr(0, colon);
                }
                bool parsed_index = false;
                const QString index_text = QString::fromStdString(stream_id_.substr(colon + 1));
                const int index = index_text.toInt(&parsed_index);
                if (parsed_index && !ok) {
                    stream_index_ = index;
                }
            } else if (device_id_.empty()) {
                device_id_ = stream_id_;
            }
        }
    }
}

SignalingClient::~SignalingClient() {
    Stop();
}

bool SignalingClient::Connect() {
    std::cout << "[Signaling] connect " << host_ << ":" << port_ << " (role=" << role_ << ")" << std::endl;
    qInfo().noquote() << QString("[Signaling] connect host=%1 port=%2 role=%3 device_id=\"%4\" stream_index=%5 server_addr=\"%6\"")
                             .arg(QString::fromStdString(host_))
                             .arg(port_)
                             .arg(QString::fromStdString(role_))
                             .arg(QString::fromStdString(device_id_))
                             .arg(stream_index_)
                             .arg(QString::fromStdString(server_addr_));
    qInfo().noquote() << QString("[Signaling] stream_id=\"%1\"")
                             .arg(QString::fromStdString(stream_id_));
    sock_fd_ = DialTcp(host_, port_);
    if (sock_fd_ < 0) {
        qWarning() << "[Signaling] connect failed";
        if (on_error_) on_error_("connect failed");
        return false;
    }

    const std::string msg = BuildRegisterLine() + "\n";
    qInfo().noquote() << "[Signaling] register raw =" << PreviewLine(msg);
    size_t offset = 0;
    while (offset < msg.size()) {
        const ssize_t sent =
            send(sock_fd_, msg.data() + offset, msg.size() - offset, MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        qWarning().noquote() << QString("[Signaling] send register failed errno=%1 err=%2")
                                    .arg(errno)
                                    .arg(strerror(errno));
        if (on_error_) on_error_("send register failed");
        close(sock_fd_);
        sock_fd_ = -1;
        return false;
    }

    std::cout << "[Signaling] register " << msg;
    return true;
}

bool SignalingClient::Start() {
    if (running_) return true;
    if (!Connect()) return false;
    running_ = true;
    reader_thread_ = std::make_unique<std::thread>(&SignalingClient::ReaderLoop, this);
    return true;
}

void SignalingClient::Stop() {
    running_ = false;
    if (sock_fd_ >= 0) {
        shutdown(sock_fd_, SHUT_RDWR);
        close(sock_fd_);
        sock_fd_ = -1;
    }
    if (reader_thread_ && reader_thread_->joinable()) {
        reader_thread_->join();
        reader_thread_.reset();
    }
}

void SignalingClient::SendLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (sock_fd_ < 0) return;

    const std::string msg = line + "\n";
    std::cout << msg;
    qInfo().noquote() << "[Signaling] send line =" << PreviewLine(msg);

    size_t offset = 0;
    while (offset < msg.size()) {
        const ssize_t sent =
            send(sock_fd_, msg.data() + offset, msg.size() - offset, MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        qWarning().noquote() << QString("[Signaling] send failed errno=%1 err=%2 line=%3")
                                    .arg(errno)
                                    .arg(strerror(errno))
                                    .arg(PreviewLine(msg));
        if (on_error_) on_error_(std::string("send: ") + strerror(errno));
        return;
    }
}

void SignalingClient::SendOffer(const std::string& sdp) {
    std::ostringstream oss;
    oss << "{\"type\":\"offer\",\"sdp\":\"" << EscapeJsonString(sdp) << "\"";
    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        if (!last_remote_peer_id_.empty()) {
            oss << ",\"to\":\"" << EscapeJsonString(last_remote_peer_id_) << "\"";
        }
    }
    oss << "}";
    SendLine(oss.str());
}

void SignalingClient::SendAnswer(const std::string& sdp) {
    std::ostringstream oss;
    oss << "{\"type\":\"answer\",\"sdp\":\"" << EscapeJsonString(sdp) << "\"";
    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        if (!last_remote_peer_id_.empty()) {
            oss << ",\"to\":\"" << EscapeJsonString(last_remote_peer_id_) << "\"";
        }
    }
    oss << "}";
    SendLine(oss.str());
}

void SignalingClient::SendIceCandidate(const std::string& mid,
                                       int mline_index,
                                       const std::string& candidate) {
    qDebug().noquote() << "[Signaling] send ICE mid=" << QString::fromStdString(mid)
                       << "mline=" << mline_index
                       << "len=" << static_cast<int>(candidate.size());

    std::ostringstream oss;
    oss << "{\"type\":\"ice\",\"mid\":\"" << EscapeJsonString(mid)
        << "\",\"mlineIndex\":" << mline_index
        << ",\"candidate\":\"" << EscapeJsonString(candidate) << "\"";
    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        if (!last_remote_peer_id_.empty()) {
            oss << ",\"to\":\"" << EscapeJsonString(last_remote_peer_id_) << "\"";
        }
    }
    oss << "}";
    SendLine(oss.str());
}

void SignalingClient::ReaderLoop() {
    std::string buf;
    char tmp[65536];
    while (running_ && sock_fd_ >= 0) {
        const ssize_t n = recv(sock_fd_, tmp, sizeof(tmp) - 1, 0);
        if (n > 0) {
            tmp[n] = '\0';
            qInfo().noquote() << QString("[Signaling] recv bytes=%1 chunk=%2")
                                     .arg(static_cast<qlonglong>(n))
                                     .arg(PreviewLine(std::string(tmp, static_cast<size_t>(n))));
            buf += tmp;
            size_t pos = 0;
            while ((pos = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                ParseAndDispatch(line);
            }
            continue;
        }
        if (n == 0) {
            qWarning() << "[Signaling] recv EOF from server";
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            qWarning().noquote() << QString("[Signaling] recv failed errno=%1 err=%2")
                                        .arg(errno)
                                        .arg(strerror(errno));
            break;
        }
    }
    qInfo() << "[Signaling] reader loop exit";
}

void SignalingClient::ParseAndDispatch(const std::string& line) {
    if (line.empty()) return;

    qInfo().noquote() << "[Signaling] raw line =" << PreviewLine(line);
    std::string normalized = line;
    if (!normalized.empty() && normalized.back() == '\r') {
        qInfo() << "[Signaling] trimming trailing CR before JSON parse";
        normalized.pop_back();
    }

    QJsonParseError parse_error;
    const QJsonDocument doc =
        QJsonDocument::fromJson(QByteArray::fromStdString(normalized), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning().noquote() << QString("[Signaling] invalid json err=%1 raw=%2")
                                    .arg(parse_error.errorString())
                                    .arg(PreviewLine(normalized));
        return;
    }

    const QJsonObject obj = doc.object();
    const std::string type = JsonValueToStdString(obj.value("type"));
    std::string peer_id = JsonValueToStdString(obj.value("peer_id"));
    if (peer_id.empty()) {
        peer_id = JsonValueToStdString(obj.value("id"));
    }
    qInfo().noquote() << QString("[Signaling] parsed message type=%1 from=\"%2\" to=\"%3\" peer_id=\"%4\" keys=%5")
                             .arg(QString::fromStdString(type))
                             .arg(QString::fromStdString(JsonValueToStdString(obj.value("from"))))
                             .arg(QString::fromStdString(JsonValueToStdString(obj.value("to"))))
                             .arg(QString::fromStdString(peer_id))
                             .arg(QStringList(obj.keys()).join(','));
    if (type.empty() || type == "register") {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        const std::string from = JsonValueToStdString(obj.value("from"));
        if (!from.empty()) {
            last_remote_peer_id_ = from;
        }
        if (type == "welcome" && !peer_id.empty()) {
            self_peer_id_ = peer_id;
        }
    }

    if (type == "answer") {
        const std::string sdp = JsonValueToStdString(obj.value("sdp"));
        if (!sdp.empty() && on_answer_) {
            qInfo().noquote() << QString("[Signaling] dispatch answer sdp_len=%1").arg(static_cast<int>(sdp.size()));
            on_answer_("answer", sdp);
        }
        return;
    }

    if (type == "offer") {
        const std::string sdp = JsonValueToStdString(obj.value("sdp"));
        if (!sdp.empty()) {
            qDebug().noquote() << "[Signaling] recv offer, sdp len="
                               << static_cast<int>(sdp.size());
            if (on_offer_) {
                qInfo().noquote() << QString("[Signaling] dispatch offer sdp_len=%1 from=\"%2\"")
                                         .arg(static_cast<int>(sdp.size()))
                                         .arg(QString::fromStdString(JsonValueToStdString(obj.value("from"))));
                on_offer_("offer", sdp);
            }
        } else {
            qWarning() << "[Signaling] offer without sdp";
        }
        return;
    }

    if (type == "ice" || type == "candidate") {
        std::string mid = JsonValueToStdString(obj.value("mid"));
        if (mid.empty()) {
            mid = JsonValueToStdString(obj.value("sdpMid"));
        }

        int mline = 0;
        if (obj.value("mlineIndex").isDouble()) {
            mline = obj.value("mlineIndex").toInt();
        } else if (obj.value("sdpMLineIndex").isDouble()) {
            mline = obj.value("sdpMLineIndex").toInt();
        }

        const std::string candidate = JsonValueToStdString(obj.value("candidate"));
        qDebug().noquote() << "[Signaling] recv ICE mid=" << QString::fromStdString(mid)
                           << "mline=" << mline
                           << "candidate len=" << static_cast<int>(candidate.size());
        if (on_ice_ && !candidate.empty()) {
            qInfo().noquote() << QString("[Signaling] dispatch ICE mid=\"%1\" mline=%2 candidate_len=%3")
                                     .arg(QString::fromStdString(mid))
                                     .arg(mline)
                                     .arg(static_cast<int>(candidate.size()));
            on_ice_(mid, mline, candidate);
        } else if (candidate.empty()) {
            qWarning() << "[Signaling] ICE without candidate payload";
        }
        return;
    }

    qInfo().noquote() << QString("[Signaling] unhandled message type=%1 raw=%2")
                             .arg(QString::fromStdString(type))
                             .arg(PreviewLine(normalized));
}

std::string SignalingClient::BuildRegisterLine() const {
    QJsonObject obj;
    obj.insert("type", "register");
    obj.insert("role", QString::fromStdString(role_));

    if (role_ == "subscriber") {
        const std::string device_id = device_id_.empty() ? kDefaultDemoDeviceId : device_id_;
        const int stream_index = stream_index_ < 0 ? kDefaultDemoStreamIndex : stream_index_;
        const std::string stream_id =
            stream_id_.empty() ? device_id + ":" + std::to_string(stream_index) : stream_id_;
        obj.insert("stream_id", QString::fromStdString(stream_id));
        obj.insert("device_id", QString::fromStdString(device_id));
        obj.insert("stream_index", stream_index);
    } else {
        if (!stream_id_.empty()) {
            obj.insert("stream_id", QString::fromStdString(stream_id_));
        } else if (!device_id_.empty() && stream_index_ >= 0) {
            obj.insert("stream_id",
                       QString::fromStdString(device_id_ + ":" + std::to_string(stream_index_)));
        }
        if (!device_id_.empty()) {
            obj.insert("device_id", QString::fromStdString(device_id_));
        }
        if (stream_index_ >= 0) {
            obj.insert("stream_index", stream_index_);
        }
    }

    return QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
}

std::string SignalingClient::EscapeJsonString(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

}  // namespace webrtc_demo
