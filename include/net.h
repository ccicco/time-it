#pragma once
#include <string>
#include <vector>

// Time-It's wire: one plain TCP line stream, no telnet ceremony.
class Net {
public:
    bool connect_to(const char* host, int port, std::string* err);
    void shutdown();
    int  fd() const { return fd_; }
    bool live() const { return fd_ >= 0 && !eof_; }
    // drain what arrived; whole lines out ('\r' stripped), the ring keeps the tail
    void pump(std::vector<std::string>* lines);
    // the newline-less remainder: prompts live here until their newline retires them
    const std::string& pending() const { return buf_; }
    // retire the tail NOW (Enter is the chronological hinge: prompt line first,
    // the player's echo beneath it)
    std::string flush_pending();
    bool send_line(const std::string& s);
private:
    int         fd_ = -1;
    std::string buf_;
    bool        eof_ = false;
};
