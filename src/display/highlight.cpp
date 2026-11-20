#include "highlight.h"
#include <cctype>

// Case-insensitive substring search: MUD text changes voice, not spelling.
static size_t ci_find(const std::string& hay, const std::string& needle, size_t from) {
    if (needle.empty() || hay.size() < needle.size() || from >= hay.size()) return (size_t)-1;
    for (size_t i = from; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (std::tolower((unsigned char)hay[i + j]) != std::tolower((unsigned char)needle[j])) { ok = false; break; }
        }
        if (ok) return i;
    }
    return (size_t)-1;
}

void highlight_row(PaintRow* row, const std::vector<HiRule>& rules) {
    if (rules.empty()) return;
    PaintRow out;
    out.reserve(row->size() * 2 + 2);
    for (const PaintSpan& sp : *row) {
        size_t off = 0;
        for (;;) {
            size_t best = (size_t)-1;
            const HiRule* win = nullptr;
            for (const HiRule& r : rules) {                  // leftmost match wins
                const size_t m = ci_find(sp.text, r.text, off);
                if (m != (size_t)-1 && (best == (size_t)-1 || m < best)) { best = m; win = &r; }
            }
            if (!win) {
                if (off < sp.text.size()) out.push_back(PaintSpan{sp.text.substr(off), sp.attr});
                break;
            }
            if (best > off) out.push_back(PaintSpan{sp.text.substr(off, best - off), sp.attr});
            out.push_back(PaintSpan{sp.text.substr(best, win->text.size()), win->attr});
            off = best + win->text.size();
        }
    }
    if (!out.empty()) *row = out;
}
