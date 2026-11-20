#include "view.h"
#include <ncurses.h>
#include <vector>

// A word gap collapses to one space at a seam; a monster token hard-breaks;
// short lines and centered banners pass untouched. Width is decided at render,
// so a resize re-wraps the same scrollback honestly - the ring stores words,
// never pre-broken rows.
static void wrap_line(const std::string& s, int width, std::vector<std::string>* out) {
    if (width < 1) width = 1;
    size_t start = 0;
    for (;;) {
        const size_t remain = s.size() - start;
        if (remain <= (size_t)width) {
            out->push_back(s.substr(start));
            return;
        }
        size_t brk = s.find_last_of(' ', start + (size_t)width - 1);
        if (brk == std::string::npos || brk <= start) brk = start + (size_t)width;  // no gap in sight: hard break
        out->push_back(s.substr(start, brk - start));
        start = brk;
        if (start < s.size() && s[start] == ' ') ++start;                            // the collapsed seam
    }
}

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
    if (ch == KEY_PPAGE) { offset_ += (size_t)page; }
    else if (ch == KEY_NPAGE) { offset_ = offset_ > (size_t)page ? offset_ - (size_t)page : 0; }
    else if (ch == 27) { offset_ = 0; }                    // ESC: home to live
    else return false;
    return true;
}

void View::render(const std::string& input, size_t cursor, const std::string& status,
                  const std::string& tail) {
    const int h = LINES - 2 > 0 ? LINES - 2 : 1;
    if (offset_ > lines_.size()) offset_ = lines_.size();   // the ring is finite

    // Walk logical lines newest-first, wrap each, and fill the pane so the
    // last visual row is always the newest word on the wire.
    std::vector<std::string> rows;
    size_t li = lines_.size() - offset_;                   // exclusive end: newest candidate
    while (li > 0 && rows.size() < (size_t)h) {
        std::vector<std::string> wrapped;
        wrap_line(lines_[li - 1], COLS, &wrapped);
        const size_t need = (size_t)h - rows.size();
        const size_t skip = wrapped.size() > need ? wrapped.size() - need : 0;
        rows.insert(rows.begin(), wrapped.begin() + (long)skip, wrapped.end());
        --li;
    }

    werase(wout_);
    int row = 0;
    for (; row < (int)rows.size(); ++row) {
        mvwaddnstr(wout_, row, 0, rows[row].c_str(), COLS);
    }
    // the pending tail rides the same wrap, shown only at the live edge
    if (offset_ == 0 && !tail.empty() && row < h) {
        std::vector<std::string> trows;
        wrap_line(tail, COLS, &trows);
        for (const std::string& t : trows) {
            if (row >= h) break;
            mvwaddnstr(wout_, row++, 0, t.c_str(), COLS);
        }
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
