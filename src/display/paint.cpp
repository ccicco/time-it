#include "paint.h"
#include <ncurses.h>
#include <cstdlib>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// The style state one SGR stream maintains while a logical line is parsed.
struct PaintState {
    bool bold = false;
    int  pair = 0;      // 0 = terminal default fg, else curses pair (idx+1)
};

static int attr_of(const PaintState& st) {
    return (st.bold ? A_BOLD : 0) | (st.pair ? COLOR_PAIR(st.pair) : 0);
}

// One consistent pair map: SGR color index n (0..255) rides pair n+1, so
// "30" and "38;5;0" land on the same pair and the legacy eight need no
// translation. Terminals with fewer colors clamp at the top.
static int palette_pair(int n) {
    if (n < 0) n = 0;
    const int maxc = COLORS > 0 ? COLORS : 8;
    if (n >= maxc) n = maxc - 1;
    return n + 1;
}

// xterm-256 quantization: the 6x6x6 cube (16..231), the 24 grays (232..255),
// else the legacy 16. Good enough to honor truecolor honestly on curses.
static int rgb_pair(int r, int g, int b) {
    if (r < 0) r = 0; if (r > 255) r = 255;                 // clamp each channel
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    const int lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
    const int hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (hi - lo < 12) {                                     // near-neutral: gray ramp
        const int gray = (r + g + b) / 3;
        return palette_pair(232 + (gray * 23 + 127) / 255);
    }
    static const int lvl[6] = { 0, 95, 135, 175, 215, 255 };
    int qr = 0, qg = 0, qb = 0;
    for (int i = 1; i < 6; ++i) {
        if (std::abs(r - lvl[i]) < std::abs(r - lvl[qr])) qr = i;
        if (std::abs(g - lvl[i]) < std::abs(g - lvl[qg])) qg = i;
        if (std::abs(b - lvl[i]) < std::abs(b - lvl[qb])) qb = i;
    }
    return palette_pair(16 + 36 * qr + 6 * qg + qb);
}

// Consume one escape sequence starting at s[pos] == '\x1b'. SGR ('m' end)
// updates the style state; any other sequence is swallowed whole, because
// terminal-positioning control has no meaning inside a pane.
static size_t consume_escape(const std::string& s, size_t pos, PaintState* st) {
    if (pos + 1 >= s.size() || s[pos + 1] != '[') {
        return pos + 1 < s.size() ? 2 : 1;                 // lone ESC or ESC(something: skip the pair
    }
    size_t i = pos + 2;
    while (i < s.size()) {
        const char c = s[i];
        if ((c >= '0' && c <= '9') || c == ';' || c == '?') ++i;
        else break;
    }
    if (i >= s.size()) return s.size() - pos;              // truncated escape at wire end: eat it
    if (s[i] != 'm') return i - pos + 1;                    // swallowed non-SGR CSI
    // SGR: parse the parameter list.
    std::vector<int> params;
    {
        int cur = -1;
        for (size_t p = pos + 2; p < i; ++p) {
            if (s[p] == ';' || s[p] == '?') { params.push_back(cur); cur = -1; }
            else cur = (cur < 0 ? 0 : cur * 10) + (s[p] - '0');
        }
        params.push_back(cur);
    }
    for (size_t pi = 0; pi < params.size(); ++pi) {
        const int v = params[pi];
        if (v == 38 || v == 48 || v == 58) {                // extended color grammar
            if (pi + 2 < params.size() && params[pi + 1] == 5) {
                if (v == 38) st->pair = palette_pair(params[pi + 2]);
                pi += 2;                                    // 5;n - the 256 palette (bg politely ignored in v1)
                continue;
            }
            if (pi + 4 < params.size() && params[pi + 1] == 2) {
                if (v == 38) st->pair = rgb_pair(params[pi + 2], params[pi + 3], params[pi + 4]);
                pi += 4;                                    // 2;r;g;b - truecolor, quantized to the palette
                continue;
            }
            continue;                                      // malformed extension: skip wholesale, degrade gently
        }
        if (v == 0) { st->bold = false; st->pair = 0; }                    // reset
        else if (v == 1) st->bold = true;
        else if (v == 22) st->bold = false;
        else if (v >= 30 && v <= 37) st->pair = palette_pair(v - 30);     // legacy eight ride the same pair map
        else if (v >= 90 && v <= 97) { st->pair = palette_pair(v - 90 + 8); st->bold = true; } // brights: 8..15
        else if (v == 39) st->pair = 0;                                    // default fg
        // backgrounds and exotic codes: politely ignored in v1
    }
    return i - pos + 1;
}

// One word (or run of spaces) as a styled token. vis counts printable bytes.
struct Tok {
    std::vector<PaintSpan> spans;
    size_t vis = 0;
    bool   space = false;
};

static void tok_add_span(Tok* t, const PaintState& st, const std::string& text) {
    if (text.empty()) return;
    const int a = attr_of(st);
    if (!t->spans.empty() && t->spans.back().attr == a) t->spans.back().text += text;
    else t->spans.push_back(PaintSpan{text, a});
    for (char ch : text) if (ch >= 32 && ch != 127) ++t->vis;
}

static void tok_drop_one_space(Tok* t) {
    for (PaintSpan& sp : t->spans) {
        if (!sp.text.empty() && sp.text[0] == ' ') { sp.text.erase(0, 1); --t->vis; return; }
    }
}

// Copy the visible chars [from, from+count) out of a token's spans. Control
// bytes are dropped in the slice: a monster token is border junk, not prose.
static PaintRow spans_slice(const std::vector<PaintSpan>& spans, size_t from, size_t count) {
    PaintRow r;
    size_t seen = 0;
    for (const PaintSpan& sp : spans) {
        size_t vlen = 0;
        for (char ch : sp.text) if (ch >= 32 && ch != 127) ++vlen;
        if (seen + vlen <= from) { seen += vlen; continue; }
        const size_t lo = seen > from ? seen : from;
        const size_t hi = seen + vlen < from + count ? seen + vlen : from + count;
        std::string txt;
        size_t pos = seen;
        for (char ch : sp.text) {
            if (ch >= 32 && ch != 127) {
                if (pos >= lo && pos < hi) txt += ch;
                ++pos;
            }
        }
        if (!txt.empty()) r.push_back(PaintSpan{txt, sp.attr});
        seen += vlen;
        if (seen >= from + count) break;
    }
    return r;
}

void paint_wrap(const std::string& s, int width, std::vector<PaintRow>* out) {
    if (width < 1) width = 1;

    // --- tokenize on space/non-space boundaries, styles carried inline
    std::vector<Tok> toks;
    PaintState st;
    Tok cur;
    std::string pend;
    bool cur_space = false;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        if (c == 0x1b) {
            if (!pend.empty()) { tok_add_span(&cur, st, pend); pend.clear(); }
            i += consume_escape(s, i, &st);
            continue;
        }
        const bool sp = (c == ' ');
        if (sp != cur_space && cur.vis > 0) {
            toks.push_back(cur);
            cur = Tok{};
        }
        cur_space = sp;
        cur.space = sp;
        pend += (char)c;
        ++i;
    }
    if (!pend.empty()) tok_add_span(&cur, st, pend);
    if (cur.vis > 0) toks.push_back(cur);

    // --- pack tokens into rows, greedy, mirrors the R2 wrap seam rules
    PaintRow row;
    size_t w = 0;
    bool first_row = true;
    auto flush_row = [&]() {
        out->push_back(row);
        row.clear();
        w = 0;
        first_row = false;
    };
    for (Tok& t : toks) {
        if (t.vis > (size_t)width) {                        // monster token: own rows, hard-broken
            if (w > 0) flush_row();
            for (size_t off = 0; off < t.vis; off += (size_t)width) {
                out->push_back(spans_slice(t.spans, off, (size_t)width));
            }
            first_row = false;
            continue;
        }
        if (w > 0 && w + t.vis > (size_t)width) flush_row();
        if (w == 0 && !first_row && t.space) {              // the seam: one space collapses
            tok_drop_one_space(&t);
            if (t.vis == 0) continue;
        }
        for (PaintSpan& sp : t.spans) {
            if (!row.empty() && row.back().attr == sp.attr) row.back().text += sp.text;
            else row.push_back(sp);
        }
        w += t.vis;
    }
    if (!row.empty() || out->empty()) out->push_back(row);  // every line yields at least one row
}

void paint_row(struct _win_st* win, int y, int x, const PaintRow& row, int maxw) {
    wmove(win, y, x);
    int rem = maxw;
    for (const PaintSpan& sp : row) {
        if (rem <= 0) break;
        wattrset(win, sp.attr);
        const int n = (int)sp.text.size() < rem ? (int)sp.text.size() : rem;
        waddnstr(win, sp.text.c_str(), n);
        rem -= n;
    }
    wattrset(win, A_NORMAL);
}
