#include "highlight.h"
#include "input.h"
#include "prompt.h"
#include "net.h"
#include "view.h"
#include <ncurses.h>
#include <poll.h>
#include <unistd.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static void log_line(std::FILE* log, const char* tag, const std::string& s) {
    if (log) std::fprintf(log, "%s%s\n", tag, s.c_str());
}

// The shape of a password prompt, recognized wherever Diku and its children
// say it: the line's last word is "password:". What follows is secret.
static bool ends_with_password_prompt(const std::string& raw) {
    std::string s = raw;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
    if (s.size() < 9) return false;
    std::string t = s.substr(s.size() - 9);
    for (char& c : t) c = (char)std::tolower((unsigned char)c);
    return t == "password:";
}

static bool is_color_name(const std::string& s) {
    return s == "red" || s == "green" || s == "yellow" || s == "blue" ||
           s == "magenta" || s == "cyan" || s == "white" || s == "orange" || s == "bold";
}

static int color_attr(const std::string& name) {
    if (name == "red")     return COLOR_PAIR(2) | A_BOLD;
    if (name == "green")   return COLOR_PAIR(3);
    if (name == "yellow")  return COLOR_PAIR(4);
    if (name == "blue")    return COLOR_PAIR(5);
    if (name == "magenta") return COLOR_PAIR(6) | A_BOLD;
    if (name == "cyan")    return COLOR_PAIR(7);
    if (name == "white")   return COLOR_PAIR(8) | A_BOLD;
    if (name == "orange")  return COLOR_PAIR(209);
    if (name == "bold")    return A_BOLD;
    return COLOR_PAIR(4) | A_BOLD;                        // unspoken color: bold yellow, the classic
}

// One macro: name swaps for expansion at the submit hinge, trailing words ride.
struct Alias {
    std::string name;
    std::string expansion;
};

static bool expand_aliases(std::vector<Alias>* aliases, std::string* text) {
    bool any = false;
    for (;;) {
        const std::string& s = *text;
        const size_t sp = s.find(' ');
        const std::string head = sp == std::string::npos ? s : s.substr(0, sp);
        if (head.empty()) break;
        bool hit = false;
        for (const Alias& a : *aliases) {
            if (head == a.name) {
                *text = a.expansion + (sp == std::string::npos ? "" : s.substr(sp));
                any = hit = true;
                break;
            }
        }
        if (!hit) break;                                   // no rule chain: one substitution per pass
        if (text->find(' ') == std::string::npos) {
            const std::string& t = *text;                  // single word left: try once more, no loop
            bool chain = false;
            for (const Alias& a : *aliases) if (t == a.name) { chain = true; break; }
            if (!chain) break;
        }
    }
    return any;
}

// --- the rc: ~/.timeit.rc, human grammar, auto-loaded at boot, rewritten at every mutation.
//     File lines: "hi <text> [color]" (last word colors only if it names one) and
//     "alias <name> <expansion...>". No exit hook: the file is always current.
static std::string rc_path() {
    const char* h = std::getenv("HOME");
    return std::string(h ? h : ".") + "/.timeit.rc";
}

static std::string attr_to_name(int attr) {
    static const struct { const char* n; int a; } kTable[] = {
        {"red", COLOR_PAIR(2) | A_BOLD}, {"green", COLOR_PAIR(3)}, {"yellow", COLOR_PAIR(4)},
        {"blue", COLOR_PAIR(5)}, {"magenta", COLOR_PAIR(6) | A_BOLD}, {"cyan", COLOR_PAIR(7)},
        {"white", COLOR_PAIR(8) | A_BOLD}, {"orange", COLOR_PAIR(209)}, {"bold", A_BOLD},
    };
    for (const auto& e : kTable) if (e.a == attr) return e.n;
    return "";                                             // default bold-yellow: written as bare text
}

static bool save_rc(const std::vector<HiRule>& hi, const std::vector<Alias>& al) {
    std::FILE* f = std::fopen(rc_path().c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "# time-it rules. Grammar: hi <text> [color] | alias <name> <expansion...>\n");
    for (const HiRule& r : hi) {
        const std::string n = attr_to_name(r.attr);
        std::fprintf(f, "hi %s%s%s\n", r.text.c_str(), n.empty() ? "" : " ", n.c_str());
    }
    for (const Alias& a : al) std::fprintf(f, "alias %s %s\n", a.name.c_str(), a.expansion.c_str());
    std::fclose(f);
    return true;
}

static size_t load_rc(std::vector<HiRule>* hi, std::vector<Alias>* al) {
    std::FILE* f = std::fopen(rc_path().c_str(), "r");
    if (!f) return 0;
    char buf[1024];
    size_t n = 0;
    while (std::fgets(buf, sizeof buf, f)) {
        std::string s = buf;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty() || s[0] == '#') continue;
        if (s.rfind("hi ", 0) == 0) {
            std::string rest = s.substr(3);
            std::string text = rest, color;
            const size_t sp = rest.rfind(' ');
            if (sp != std::string::npos) {
                const std::string tail = rest.substr(sp + 1);
                if (is_color_name(tail)) { text = rest.substr(0, sp); color = tail; }
            }
            if (!text.empty()) { hi->push_back(HiRule{text, color_attr(color)}); ++n; }
        } else if (s.rfind("alias ", 0) == 0) {
            const std::string rest = s.substr(6);
            const size_t sp = rest.find(' ');
            if (sp != std::string::npos) {
                al->push_back(Alias{rest.substr(0, sp), rest.substr(sp + 1)});
                ++n;
            }
        }
    }
    std::fclose(f);
    return n;
}

int main(int argc, char** argv) {
    const char* host = "127.0.0.1";
    int port = 2600;
    const char* log_path = nullptr;
    bool demo = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc) log_path = argv[++i];
        else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) host = argv[++i];
        else if (std::strcmp(argv[i], "--ansi-demo") == 0) demo = true;
    }

    Net net;
    std::string err;
    bool alive = false;
    if (!demo) {                                           // the demo needs no wire, just the screen
        if (!net.connect_to(host, port, &err)) {
            std::printf("Time-It: cannot reach %s:%d (%s).\n", host, port, err.c_str());
            return 1;
        }
        alive = true;
    }
    std::FILE* log = log_path ? std::fopen(log_path, "a") : nullptr;

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(1);

    View view;
    view.init();
    InputLine line;
    const std::string link = std::string(host) + ":" + std::to_string(port);
    std::vector<Alias> aliases;                             // born from the rc at boot, saved at every change
    std::vector<HiRule> rules;                              // staged for the view once the screen exists
    const size_t rc_count = load_rc(&rules, &aliases);
    for (const HiRule& r : rules) view.add_highlight(r.text, r.attr);

    bool expect_secret = false;                             // set by a Password: prompt, spent by the next Enter
    if (demo) {
        view.push_line("Time-It ANSI demo - painted by the same engine the scroll uses.");
        view.push_line("\x1b[30mblack\x1b[0m \x1b[31mred\x1b[0m \x1b[32mgreen\x1b[0m \x1b[33myellow\x1b[0m \x1b[34mblue\x1b[0m \x1b[35mmagenta\x1b[0m \x1b[36mcyan\x1b[0m \x1b[37mwhite\x1b[0m");
        view.push_line("bold rail: \x1b[1;30mbright black\x1b[0m \x1b[1;31mbright red\x1b[0m \x1b[1;32mbright green\x1b[0m \x1b[1;33mbright yellow\x1b[0m \x1b[1;34mbright blue\x1b[0m \x1b[1;35mbright magenta\x1b[0m \x1b[1;36mbright cyan\x1b[0m \x1b[1;37mbright white\x1b[0m");
        view.push_line("extended 256: \x1b[38;5;208mpalette orange\x1b[0m \x1b[38;5;46mmatrix green\x1b[0m \x1b[38;5;196malert red\x1b[0m \x1b[38;5;21mdeep blue\x1b[0m");
        view.push_line("truecolor: \x1b[38;2;255;140;0mtruecolor orange\x1b[0m \x1b[38;2;64;224;208mtruecolor turquoise\x1b[0m \x1b[38;2;128;0;255mtruecolor violet\x1b[0m");
        view.push_line("wrap + color interlocked: \x1b[36mthe cyan sentence must bend at the right edge and stay cyan on every row it occupies, seam and all, until the last word lands.\x1b[0m");
        view.push_line("swallow test: \x1b[2K\x1b[1;33mthis bright yellow follows a consumed cursor-clear; nothing garbled above.\x1b[0m");
        view.push_line("/hi paints what matters, /alias names what you say. /quit to leave.");
    } else {
        view.push_line("Time-It 1.0 - linked to " + link);
        view.push_line("Type commands below. Arrows recall history, PageUp browses, /hi highlights, /alias macros, /quit leaves (Ctrl-D too).");
    }
    if (rc_count > 0)
        view.push_line("[rc] " + std::to_string(rc_count) + " rule(s) remembered from ~/.timeit.rc");

    std::string status;                                     // recomposed each beat: the bar speaks
    for (;;) {
        std::string state;
        if (demo)               state = "ansi demo";
        else if (alive)         state = "live";
        else                    state = "free";
        status = link + " | " + state + " | " + std::to_string(view.line_count()) + " lines";

        /* prompt: the bar right-voice, spoken from the pending tail every beat */
        PromptReport pr;
        std::string prompt_note;
        if (alive && parse_prompt(net.pending(), &pr)) {
            if (pr.vitals) {
                prompt_note = "hp " + std::to_string(pr.hit) + " m " + std::to_string(pr.mana) + " mv " + std::to_string(pr.move);
                if (pr.fight) prompt_note += " foe " + std::to_string(pr.foe);
            } else {
                prompt_note = "room " + std::to_string(pr.room);
            }
        }
        view.render(expect_secret ? std::string(line.text().size(), '*') : line.text(),
                    line.cursor(), status, alive ? net.pending() : std::string(), prompt_note);
        struct pollfd pf[2] = { { STDIN_FILENO, POLLIN, 0 }, { net.fd(), POLLIN, 0 } };
        const int r = ::poll(pf, alive ? 2 : 1, 250);
        if (r > 0 && (pf[0].revents & POLLIN)) {
            const int ch = getch();
            if (ch == KEY_RESIZE) { view.init(); continue; }
            if (!view.on_key(ch)) {
                std::string submitted;
                const InputLine::Act act = line.feed(ch, &submitted);
                if (act == InputLine::Act::quit) break;
                if (act == InputLine::Act::submit) {
                    if (submitted == "/quit") break;
                    if (submitted.rfind("/hi", 0) == 0) {     // client-side: never reaches the wire
                        std::string rest = submitted.substr(3);
                        while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
                        while (!rest.empty() && rest.back() == ' ') rest.pop_back();
                        if (rest.empty() || rest == "list") {
                            view.push_line("[hi] " + std::to_string(view.highlights().size()) + " rule(s)");
                            for (const HiRule& r : view.highlights())
                                view.push_line("  " + r.text);
                        } else if (rest == "clear") {
                            view.clear_highlights();
                            aliases = aliases;                // highlights clear alone; macros keep their names
                            save_rc(view.highlights(), aliases);
                            view.push_line("[hi] cleared (rc updated)");
                        } else {
                            std::string text = rest, color;
                            const size_t sp = rest.rfind(' ');  // last word colors only if it names a color
                            if (sp != std::string::npos) {
                                const std::string tail = rest.substr(sp + 1);
                                if (is_color_name(tail)) { text = rest.substr(0, sp); color = tail; }
                            }
                            view.add_highlight(text, color_attr(color));
                            std::string note = "[hi] painted: " + text;
                            if (!color.empty()) note += " (" + color + ")";
                            view.push_line(note);
                            log_line(log, "[hi] ", note);
                            save_rc(view.highlights(), aliases);
                        }
                    } else if (submitted.rfind("/alias", 0) == 0) {   // client-side: never reaches the wire
                        std::string rest = submitted.substr(6);
                        while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
                        while (!rest.empty() && rest.back() == ' ') rest.pop_back();
                        if (rest.empty() || rest == "list") {
                            view.push_line("[alias] " + std::to_string(aliases.size()) + " macro(s)");
                            for (const Alias& a : aliases)
                                view.push_line("  " + a.name + " = " + a.expansion);
                        } else if (rest == "clear") {
                            aliases.clear();
                            save_rc(view.highlights(), aliases);
                            view.push_line("[alias] cleared (rc updated)");
                        } else {
                            const size_t sp = rest.find(' ');
                            if (sp == std::string::npos) {
                                view.push_line("[alias] usage: /alias <name> <expansion...>");
                            } else {
                                const std::string name = rest.substr(0, sp);
                                const std::string expansion = rest.substr(sp + 1);
                                bool replaced = false;
                                for (Alias& a : aliases) {
                                    if (a.name == name) { a.expansion = expansion; replaced = true; break; }
                                }
                                if (!replaced) aliases.push_back(Alias{name, expansion});
                                view.push_line("[alias] " + name + " = " + expansion);
                                log_line(log, "[alias] ", name + " = " + expansion);
                                save_rc(view.highlights(), aliases);
                            }
                        }
                    } else {
                        // the hinge: macros speak before the wire ever hears
                        std::string before = submitted;
                        if (expand_aliases(&aliases, &submitted) && submitted != before) {
                            view.push_line("[alias] " + before + " -> " + submitted);
                            log_line(log, "[alias-echo] ", before + " -> " + submitted);
                        }
                        if (alive) {
                            const std::string tail = net.flush_pending();
                            if (!tail.empty()) {
                                if (whole_tail_is_prompt(tail)) {
                                    if (log) std::fprintf(log, "[tail-prompt] %s\n", tail.c_str());
                                } else {
                                view.push_line(tail);
                                log_line(log, "[tail] ", tail);
                                if (whole_tail_is_prompt(tail)) {} else if (ends_with_password_prompt(tail)) expect_secret = true;   // the wire just asked
                                }
                                }
                            }
                        if (expect_secret) {                     // the wire asked: the answer is never spoken in bytes
                            view.push_line("> " + std::string(submitted.size(), '*'));
                            log_line(log, "[secret withheld]", "");
                            expect_secret = false;
                        } else {
                            if (!submitted.empty()) { view.push_line("> " + submitted); log_line(log, "> ", submitted); }
                        }
                        if (alive) net.send_line(submitted);      // empty Enter still reaches the pager
                    }
                }
            }
        }
        if (r > 0 && alive && (pf[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            std::vector<std::string> lines;
            net.pump(&lines);
            for (const std::string& s : lines) {
                view.push_line(s);
                log_line(log, "", s);                        // the tape stays raw: SGR rides into the log unhonored or hidden
                if (ends_with_password_prompt(s)) expect_secret = true;
            }
            if (!net.live()) {                               // the link dropped; the room keeps its history
                alive = false;
                net.shutdown();
                view.push_line("[The link dropped. /quit to leave.]");
            }
        }
        if (log) std::fflush(log);
    }
    endwin();
    net.shutdown();
    if (log) std::fclose(log);
    std::printf("Time-It: clean exit.\n");
    return 0;
}
