#include "view.h"
#include "paint.h"
#include <ncurses.h>
#include <vector>

void View::init() {
    static bool s_colors = false;                    // once per process: pair n = palette color n-1
    if (!s_colors) {
        start_color();
        use_default_colors();
        const int n = COLORS < 256 ? COLORS : 256;
        for (int i = 0; i < n; ++i) init_pair(i + 1, i, -1);
        s_colors = true;
    }
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

    // Walk logical lines newest-first, paint-wrap each, fill the pane so the
    // last visual row is always the newest word on the wire, styled.
    std::vector<PaintRow> rows;
    size_t li = lines_.size() - offset_;                   // exclusive end: newest candidate
    while (li > 0 && rows.size() < (size_t)h) {
        std::vector<PaintRow> wrapped;
        paint_wrap(lines_[li - 1], COLS, &wrapped);
        const size_t need = (size_t)h - rows.size();
        const size_t skip = wrapped.size() > need ? wrapped.size() - need : 0;
        rows.insert(rows.begin(), wrapped.begin() + (long)skip, wrapped.end());
        --li;
    }

    werase(wout_);
    int row = 0;
    for (; row < (int)rows.size(); ++row) {
        paint_row(wout_, row, 0, rows[row], COLS);
    }
    // the pending tail rides the same paint, shown only at the live edge
    if (offset_ == 0 && !tail.empty() && row < h) {
        std::vector<PaintRow> trows;
        paint_wrap(tail, COLS, &trows);
        for (const PaintRow& t : trows) {
            if (row >= h) break;
            paint_row(wout_, row++, 0, t, COLS);
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

View::~View() {
    if (wout_) { delwin(wout_); delwin(wstatus_); delwin(winput_); }
}
