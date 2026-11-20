#pragma once
#include <deque>
#include <string>

// xterm-honest panes: output scroll, one status line, one input line.
// ncurses WINDOW, kept out of this header by forward declaration.
struct _win_st;

class View {
public:
    static constexpr size_t kScrollback = 10000;
    void init();
    void push_line(const std::string& s);
    bool on_key(int ch);        // PgUp/PgDn browse, ESC returns to live
    void render(const std::string& input, size_t cursor, const std::string& status,
                const std::string& tail);
private:
    struct _win_st* wout_ = nullptr;
    struct _win_st* wstatus_ = nullptr;
    struct _win_st* winput_ = nullptr;
    std::deque<std::string> lines_;
    size_t offset_ = 0;         // lines above the live edge; 0 = live
};
