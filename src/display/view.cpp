#include "view.h"
#include <ncurses.h>

void View::init() {
    if (wout_) { delwin(wout_); delwin(wstatus_); delwin(winput_); }
    wout_    = newwin(LINES - 2 > 0 ? LINES - 2 : 1, COLS, 0, 0);
    wstatus_ = newwin(1, COLS, LINES - 2 > 0 ? LINES - 2 : 0, 0);
    winput_  = newwin(1, COLS, LINES - 1, 0);
}

void View::push_line(const std::string& s) {
    lines_.push_back(s);
    if (lines_.size() > kScrollback) lines_.pop_front();
}

bool View::on_key(int ch) {
    const int page = LINES > 3 ? LINES - 3 : 1;
    if (ch == KEY_PPAGE) { offset_ += page; }
    else if (ch == KEY_NPAGE) { offset_ = offset_ > (size_t)page ? offset_ - (size_t)page : 0; }
    else if (ch == 27) { offset_ = 0; }                    // ESC: home to live
    else return false;
    return true;
}

void View::render(const std::string& input, size_t cursor, const std::string& status,
                  const std::string& tail) {
    const int h = LINES - 2 > 0 ? LINES - 2 : 1;
    if (offset_ > lines_.size()) offset_ = lines_.size();   // the ring is finite
    const size_t end = lines_.size() - offset_;
    const size_t start = end > (size_t)h ? end - (size_t)h : 0;
    werase(wout_);
    int row = 0;
    for (size_t i = start; i < end && row < h; ++i) {
        mvwaddnstr(wout_, row, 0, lines_[i].c_str(), COLS);
        ++row;
    }
    // the pending tail: the server's unfinished line, shown only at the live edge
    if (offset_ == 0 && !tail.empty() && row < h) {
        mvwaddnstr(wout_, row, 0, tail.c_str(), COLS);
        ++row;
    }
    if (offset_ > 0) {                                     // browsing leaves a marker naming the distance home
        const std::string back = "[-" + std::to_string(offset_) + " ESC=live]";
        mvwaddnstr(wout_, h - 1, COLS - (int)back.size() - 1, back.c_str(), (int)back.size());
    }
    werase(wstatus_);
    mvwaddnstr(wstatus_, 0, 0, status.c_str(), COLS - 1);
    werase(winput_);
    mvwaddnstr(winput_, 0, 0, "> ", 2);
    mvwaddnstr(winput_, 0, 2, input.c_str(), COLS - 3);
    size_t x = 2 + (cursor <= input.size() ? cursor : input.size());
    wmove(winput_, 0, (int)(x < (size_t)COLS ? x : (size_t)(COLS - 1)));
    wnoutrefresh(wout_);
    wnoutrefresh(wstatus_);
    wnoutrefresh(winput_);
    doupdate();
}
