#pragma once
#include <deque>
#include <string>

struct _win_st;

// The room: scrollback ring, status bar, input line. Repainted whole per beat.
class View {
public:
    View() = default;
    ~View();
    View(const View&) = delete;
    View& operator=(const View&) = delete;
    void init();                       // windows exist, sized to the screen
    void push_line(const std::string& s);      // scrollback gains a logical line (raw, ANSI untouched)
    bool on_key(int ch);               // browse keys: PageUp/PageDown/ESC; returns true if consumed
    void render(const std::string& input, size_t cursor, const std::string& status,
               const std::string& tail);
    size_t line_count() const { return lines_.size(); }   // the ring's weight, for the bar to speak
private:
    static const size_t kScrollback = 4000;
    std::deque<std::string> lines_;
    size_t offset_ = 0;                // browse distance from live, in logical lines
    struct _win_st* wout_    = nullptr;
    struct _win_st* wstatus_ = nullptr;
    struct _win_st* winput_  = nullptr;
};
