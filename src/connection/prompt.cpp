#include "prompt.h"
#include <cctype>

// Scan a run of digits starting at s[i]; false if none.
static bool eat_int(const std::string& s, size_t* i, int* v) {
    if (*i >= s.size() || !std::isdigit((unsigned char)s[*i])) return false;
    *v = 0;
    while (*i < s.size() && std::isdigit((unsigned char)s[*i])) {
        *v = *v * 10 + (s[*i] - '0');
        ++(*i);
    }
    return true;
}

// Try to read one prompt beginning exactly at b. Grammar:
//   '[' int (',' int)* ']' ( '(' int ')' )? spaces-to-end
// 1 number = immortal room; 3 = mortal vitals; '(' adds foe.
static bool prompt_at(const std::string& s, size_t b, PromptReport* out) {
    size_t i = b + 1;
    int nums[3] = {0, 0, 0};
    int n = 0;
    for (;;) {
        if (n >= 3) return false;
        if (!eat_int(s, &i, &nums[n])) return false;
        ++n;
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        break;
    }
    if (i >= s.size() || s[i] != ']') return false;
    ++i;
    bool foe = false;
    int foe_val = 0;
    if (i < s.size() && s[i] == '(') {
        ++i;
        if (!eat_int(s, &i, &foe_val) || i >= s.size() || s[i] != ')') return false;
        ++i;
        foe = true;
    }
    while (i < s.size() && (s[i] == ' ')) ++i;           // prompt may trail spaces
    if (i != s.size()) return false;
    if (n == 3) {
        out->vitals = true;
        out->hit = nums[0];
        out->mana = nums[1];
        out->move = nums[2];
        out->fight = foe;
        out->foe = foe_val;
        return true;
    }
    if (n == 1 && !foe) {                                 // the immortal dialect: a room number
        *out = PromptReport{};
        out->room = nums[0];
        return true;
    }
    return false;                                         // 2 numbers: the wire never says that
}

bool parse_prompt(const std::string& tail, PromptReport* out) {
    const size_t b = tail.rfind('[');
    if (b == std::string::npos) return false;
    PromptReport r;
    if (!prompt_at(tail, b, &r)) return false;
    *out = r;
    return true;
}

bool whole_tail_is_prompt(const std::string& tail) {
    size_t i = 0;
    while (i < tail.size() && tail[i] == ' ') ++i;
    if (i >= tail.size() || tail[i] != '[') return false;
    PromptReport r;
    return prompt_at(tail, i, &r);
}
