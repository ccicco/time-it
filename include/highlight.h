#pragma once
#include "paint.h"
#include <string>
#include <vector>

// Client-side highlight rules: substring matches re-style what matters.
struct HiRule {
    std::string text;
    int attr;      // fully-formed curses attr; the rule wins the whole style
};

// Re-style a painted row in place: matches split their span and take the
// rule's attr. First (leftmost) match wins.
void highlight_row(PaintRow* row, const std::vector<HiRule>& rules);
