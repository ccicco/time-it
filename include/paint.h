#pragma once
#include <string>
#include <vector>

struct _win_st;

// Paint: ANSI-honest display. Spans carry text + curses attr; rows are
// styled visual lines. The wire stays raw; only the paint interprets.
struct PaintSpan { std::string text; int attr; };
using PaintRow = std::vector<PaintSpan>;

// Wrap a raw wire line (SGR escapes embedded) into styled visual rows of
// the given width. Visible width is counted on printable bytes only.
void paint_wrap(const std::string& raw, int width, std::vector<PaintRow>* rows);

// Draw one styled row at (y, x), clipped to maxw columns.
void paint_row(struct _win_st* win, int y, int x, const PaintRow& row, int maxw);
