#include "net.h"
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

bool Net::connect_to(const char* host, int port, std::string* err) {
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    const std::string port_s = std::to_string(port);
    if (getaddrinfo(host, port_s.c_str(), &hints, &res) != 0 || res == nullptr) {
        if (err) *err = "host not found";
        return false;
    }
    fd_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd_ < 0) {
        if (err) *err = "socket failed";
        freeaddrinfo(res);
        return false;
    }
    if (::connect(fd_, res->ai_addr, res->ai_addrlen) != 0) {
        if (err) *err = std::strerror(errno);
        ::close(fd_);
        fd_ = -1;
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);
    ::fcntl(fd_, F_SETFL, O_NONBLOCK);   // poll() drives the loop; recv must not block
    return true;
}

void Net::shutdown() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    buf_.clear();
}

void Net::pump(std::vector<std::string>* lines) {
    if (fd_ < 0) return;
    char chunk[4096];
    for (;;) {
        const ssize_t n = ::recv(fd_, chunk, sizeof chunk, 0);
        if (n > 0) { buf_.append(chunk, (size_t)n); continue; }
        if (n == 0) { eof_ = true; break; }               // the server closed: an honest end
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        eof_ = true; break;                                // a hard error rides the same loss
    }
    size_t at = 0;
    for (;;) {
        const size_t nl = buf_.find('\n', at);
        if (nl == std::string::npos) break;
        std::string line = buf_.substr(at, nl - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines->push_back(line);
        at = nl + 1;
    }
    buf_.erase(0, at);
}

bool Net::send_line(const std::string& s) {
    if (fd_ < 0) return false;
    std::string msg = s;
    msg += '\n';
    return ::send(fd_, msg.data(), msg.size(), 0) == (ssize_t)msg.size();
}

std::string Net::flush_pending() {
    std::string out;
    out.swap(buf_);
    return out;
}
