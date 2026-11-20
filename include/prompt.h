#pragma once
#include <string>

// The wire's prompt, per FredMUD comm.c verbatim:
//   mortal:          "[hit,mana,move] "
//   mortal fighting: "[hit,mana,move](foe) "
//   immortal:        "[room-vnum] "
struct PromptReport {
    bool vitals = false;   // the 3-number shape
    bool fight  = false;   // followed by (foe hitpoints)
    int hit = 0, mana = 0, move = 0;
    int foe = 0, room = 0;
};

// true if `tail` ENDS with a well-formed prompt; out filled accordingly.
bool parse_prompt(const std::string& tail, PromptReport* out);

// true if the whole tail is nothing but a prompt: the scroll keeps silent.
bool whole_tail_is_prompt(const std::string& tail);
