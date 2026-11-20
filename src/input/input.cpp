#include "input.h"
#include <ncurses.h>

void InputLine::load_history() {
    buf_ = hist_[(size_t)hidx_];
    pos_ = buf_.size();
}

InputLine::Act InputLine::feed(int ch, std::string* submitted) {
    if (ch == KEY_RESIZE) return Act::resize;
    if (ch == 4) return Act::quit;                          // Ctrl-D: a clean exit
    if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
        if (!buf_.empty() && (hist_.empty() || hist_.back() != buf_)) {
            hist_.push_back(buf_);
            if (hist_.size() > kHist) hist_.erase(hist_.begin());
        }
        hidx_ = -1;
        draft_.clear();
        if (submitted) *submitted = buf_;
        buf_.clear();                                        // THE FOUNDING FIX: the line empties after Enter
        pos_ = 0;
        return Act::submit;
    }
    if (ch == KEY_UP || ch == 16) {                         // the founding criterion; Ctrl-P for hardware keyboards
        if (hist_.empty()) return Act::none;
        if (hidx_ == -1) { draft_ = buf_; hidx_ = (int)hist_.size() - 1; }
        else if (hidx_ > 0) --hidx_;
        else return Act::none;
        load_history();
        return Act::none;
    }
    if (ch == KEY_DOWN || ch == 14) {                        // Ctrl-N, the way back down
        if (hidx_ == -1) return Act::none;
        ++hidx_;
        if ((size_t)hidx_ >= hist_.size()) { hidx_ = -1; buf_ = draft_; pos_ = buf_.size(); }
        else load_history();
        return Act::none;
    }
    if (ch == KEY_LEFT)  { if (pos_ > 0) --pos_; return Act::none; }
    if (ch == KEY_RIGHT) { if (pos_ < buf_.size()) ++pos_; return Act::none; }
    if (ch == KEY_HOME || ch == 1) { pos_ = 0; return Act::none; }
    if (ch == KEY_END  || ch == 5) { pos_ = buf_.size(); return Act::none; }
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        if (pos_ > 0) { buf_.erase(pos_ - 1, 1); --pos_; }
        return Act::none;
    }
    if (ch == KEY_DC) { if (pos_ < buf_.size()) buf_.erase(pos_, 1); return Act::none; }
    if (ch == 21) { buf_.clear(); pos_ = 0; return Act::none; }   // Ctrl-U kill-line
    if (ch >= 32 && ch < 127) {                             // printable; UTF-8 rides the same byte path
        buf_.insert(buf_.begin() + (long)pos_, (char)ch);
        ++pos_;
        return Act::none;
    }
    return Act::none;
}
