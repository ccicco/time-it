#include "highlight.h"
#include "input.h"
#include "net.h"
#include "prompt.h"
#include "view.h"
#include <ncurses.h>
#include <poll.h>
#include <unistd.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

static void log_line(std::FILE* log, const char* tag, const std::string& s) {
    if (!log) return;
    const time_t now = ::time(nullptr);
    char clock[16];
    const struct tm* tv = ::localtime(&now);
    ::strftime(clock, sizeof clock, "%H:%M:%S ", tv);
    std::fputs(clock, log);                               // the tape names WHEN, not just what
    std::fprintf(log, "%s%s\n", tag, s.c_str());
}

// The shape of a password prompt, recognized wherever Diku and its children say it.
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
    return COLOR_PAIR(4) | A_BOLD;                        // unspoken color: bold yellow
}

static std::string trim(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

// case-fold substring: the family's shared sense of "the wire said it"
static bool contains_ci(const std::string& s, const std::string& text) {
    if (text.empty() || s.size() < text.size()) return false;
    for (size_t i = 0; i + text.size() <= s.size(); ++i) {
        bool hit = true;
        for (size_t j = 0; j < text.size(); ++j) {
            if (std::tolower((unsigned char)s[i + j]) != std::tolower((unsigned char)text[j])) { hit = false; break; }
        }
        if (hit) return true;
    }
    return false;
}

static bool parse_walk(const std::string& s, std::vector<std::string>* out) {
    if (s.size() < 2 || s.size() % 2 != 0) return false;   // pairs only: 2n, 3e, 2n3e
    for (size_t i = 0; i < s.size(); i += 2) {
        if (s[i] < '2' || s[i] > '9') return false;        // digits required, single dirs pass through
        const char d = s[i + 1];
        if (d != 'n' && d != 's' && d != 'e' && d != 'w' && d != 'u' && d != 'd') return false;
        for (int k = 0; k < s[i] - '0'; ++k) out->push_back(std::string(1, d));
    }
    return true;
}

struct Alias {
    std::string name;
    std::string expansion;
};

struct ActRule {
    std::string text;       // what the wire says
    std::string response;   // what the client answers
    std::string cond;       // presence demanded (empty = unconditional)
};
static std::vector<std::string> room_buf;   // the room render since the last Exits:
struct Counter { std::string name; long v; };
static std::vector<Counter> counts;   // session counters, born at zero

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
        if (!hit) break;
        if (text->find(' ') == std::string::npos) {
            const std::string& t = *text;
            bool chain = false;
            for (const Alias& a : *aliases) if (t == a.name) { chain = true; break; }
            if (!chain) break;
        }
    }
    return any;
}

// --- gags: scroll-silence, tape-honest.
static bool is_gagged(const std::string& s, const std::vector<std::string>& gags) {
    for (const std::string& g : gags) if (contains_ci(s, g)) return true;
    return false;
}

// --- the rc: ~/.timeit.rc. Lines: "hi <text> [color]", "alias <name> <expansion...>",
//     "gag <text>", "act <text> => <response>".
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
    return "";
}

static bool save_rc(const std::vector<HiRule>& hi, const std::vector<Alias>& al,
                    const std::vector<std::string>& gags, const std::vector<ActRule>& acts) {
    std::FILE* f = std::fopen(rc_path().c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "# time-it rules. hi <text> [color] | alias <name> <expansion...> | gag <text> | act <text> => <response>\n");
    for (const HiRule& r : hi) {
        const std::string n = attr_to_name(r.attr);
        std::fprintf(f, "hi %s%s%s\n", r.text.c_str(), n.empty() ? "" : " ", n.c_str());
    }
    for (const Alias& a : al) std::fprintf(f, "alias %s %s\n", a.name.c_str(), a.expansion.c_str());
    for (const std::string& g : gags) std::fprintf(f, "gag %s\n", g.c_str());
    for (const ActRule& a : acts) std::fprintf(f, "act %s => %s%s\n", a.text.c_str(), a.response.c_str(), a.cond.empty() ? "" : (" WHEN " + a.cond).c_str());
    std::fclose(f);
    return true;
}

static size_t load_rc(std::vector<HiRule>* hi, std::vector<Alias>* al, std::vector<std::string>* gags,
                      std::vector<ActRule>* acts) {
    std::FILE* f = std::fopen(rc_path().c_str(), "r");
    if (!f) return 0;
    char buf[1024];
    size_t n = 0;
    while (std::fgets(buf, sizeof buf, f)) {
        std::string s = trim(buf);
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
            if (sp != std::string::npos) { al->push_back(Alias{rest.substr(0, sp), rest.substr(sp + 1)}); ++n; }
        } else if (s.rfind("gag ", 0) == 0) {
            const std::string rest = s.substr(4);
            if (!rest.empty()) { gags->push_back(rest); ++n; }
        } else if (s.rfind("act ", 0) == 0) {
            const std::string rest = s.substr(4);
            const size_t sep = rest.find("=>");
            if (sep != std::string::npos) {
                const std::string text = trim(rest.substr(0, sep));
                std::string resp = trim(rest.substr(sep + 2));
                std::string cond;
                const size_t wc = resp.find(" WHEN ");
                if (wc != std::string::npos) { cond = trim(resp.substr(wc + 6)); resp = trim(resp.substr(0, wc)); }
                if (!text.empty() && !resp.empty() && !(wc != std::string::npos && cond.empty())) { acts->push_back(ActRule{text, resp, cond}); ++n; }
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
    time_t next_dial = 0;   // zero means no redial pending
    if (!demo) {
        if (!net.connect_to(host, port, &err)) {
            std::printf("Time-It: cannot reach %s:%d (%s).\n", host, port, err.c_str());
            return 1;
        }
        alive = true;
    }
    std::FILE* log = log_path ? std::fopen(log_path, "a") : nullptr;
    if (log) {
        const time_t now = ::time(nullptr);
        char iso[40];
        const struct tm* tv = ::localtime(&now);
        ::strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%S", tv);
        std::fprintf(log, "# session %s %s:%d\n", iso, host, port);
    }

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(1);

    View view;
    view.init();
    InputLine line;
    const std::string link = std::string(host) + ":" + std::to_string(port);

    std::vector<Alias> aliases;
    std::vector<HiRule> rules;
    std::vector<std::string> gags;
    std::vector<ActRule> acts;                              // the wire's words, answered
    const size_t rc_count = load_rc(&rules, &aliases, &gags, &acts);
    for (const HiRule& r : rules) view.add_highlight(r.text, r.attr);

    const auto push = [&](const std::string& s) { if (!is_gagged(s, gags)) view.push_line(s); };

    bool expect_secret = false;
    if (demo) {
        view.push_line("Time-It ANSI demo - painted by the same engine the scroll uses.");
        view.push_line("\x1b[30mblack\x1b[0m \x1b[31mred\x1b[0m \x1b[32mgreen\x1b[0m \x1b[33myellow\x1b[0m \x1b[34mblue\x1b[0m \x1b[35mmagenta\x1b[0m \x1b[36mcyan\x1b[0m \x1b[37mwhite\x1b[0m");
        view.push_line("bold rail: \x1b[1;30mbright black\x1b[0m \x1b[1;31mbright red\x1b[0m \x1b[1;32mbright green\x1b[0m \x1b[1;33mbright yellow\x1b[0m \x1b[1;34mbright blue\x1b[0m \x1b[1;35mbright magenta\x1b[0m \x1b[1;36mbright cyan\x1b[0m \x1b[1;37mbright white\x1b[0m");
        view.push_line("extended 256: \x1b[38;5;208mpalette orange\x1b[0m \x1b[38;5;46mmatrix green\x1b[0m \x1b[38;5;196malert red\x1b[0m \x1b[38;5;21mdeep blue\x1b[0m");
        view.push_line("truecolor: \x1b[38;2;255;140;0mtruecolor orange\x1b[0m \x1b[38;2;64;224;208mtruecolor turquoise\x1b[0m \x1b[38;2;128;0;255mtruecolor violet\x1b[0m");
        view.push_line("wrap + color interlocked: \x1b[36mthe cyan sentence must bend at the right edge and stay cyan on every row it occupies, seam and all, until the last word lands.\x1b[0m");
        view.push_line("swallow test: \x1b[2K\x1b[1;33mthis bright yellow follows a consumed cursor-clear; nothing garbled above.\x1b[0m");
        view.push_line("/hi paints, /alias names, /gag silences, /act answers. /quit to leave.");
    } else {
        view.push_line("Time-It 1.0 - linked to " + link);
        view.push_line("Commands: /hi /alias /gag /act (each has list & clear), /quit.");
    }
    if (rc_count > 0)
        view.push_line("[rc] " + std::to_string(rc_count) + " rule(s) remembered from ~/.timeit.rc");

    std::string status;
    for (;;) {
        std::string state;
        if (demo)               state = "ansi demo";
        else if (alive)         state = "live";
        else                    state = "free";
        status = link + " | " + state + " | " + std::to_string(view.line_count()) + " lines";

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
        if (!alive && !demo && next_dial > 0 && ::time(nullptr) >= next_dial) {
            std::string rerr;
            if (net.connect_to(host, port, &rerr)) {
                alive = true;
                next_dial = 0;
                view.push_line("[reconnect] back in the world");

                net.send_line("l");   // the room wakes without being asked
                log_line(log, "[reconnect] ", std::string(host) + ":" + std::to_string(port) + " - auto dial ok");
            } else {
                view.push_line("[reconnect] redial refused: " + rerr);
                log_line(log, "[reconnect] ", "auto FAILED " + rerr);
                next_dial = ::time(nullptr) + 10;
            }
        }
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
                    if (submitted.rfind("/reconnect", 0) == 0) {   // the reflex of coming back
                        std::string rerr;
                        net.shutdown();
                        room_buf.clear();                          // the room-sense dies honestly
                        if (net.connect_to(host, port, &rerr)) {
                            alive = true;
                            view.push_line("[reconnect] dialed " + std::string(host) + ":" + std::to_string(port) + " live again");

                            net.send_line("l");   // the room wakes without being asked
                            view.push_line("[reconnect] presence sleeps until the room re-renders");
                            log_line(log, "[reconnect] ", std::string(host) + ":" + std::to_string(port) + " - dial ok");
                        } else {
                            view.push_line("[reconnect] refused: " + rerr);
                            log_line(log, "[reconnect] ", "FAILED " + rerr);
                        }
                    } else if (submitted.rfind("/mark ", 0) == 0) {   // a pin in the tape
                        const std::string note = trim(submitted.substr(6));
                        if (!note.empty()) {
                            log_line(log, "[mark] ", note);
                            view.push_line("[mark] " + note);
                        }
                    } else if (submitted.rfind("/tail", 0) == 0) {   // the tape comes home
                        if (!log_path) {
                            view.push_line("[tail] no tape - start with --log <file>");
                        } else {
                            std::fflush(log);
                            int n = 10;
                            const std::string rest = trim(submitted.substr(5));
                            if (!rest.empty()) { n = std::atoi(rest.c_str()); if (n < 1) n = 1; if (n > 200) n = 200; }
                            std::FILE* r = std::fopen(log_path, "r");
                            if (!r) {
                                view.push_line("[tail] cannot open the tape");
                            } else {
                                std::vector<std::string> ring;
                                char buf[4096];
                                while (std::fgets(buf, sizeof buf, r)) {
                                    std::string ln(buf);
                                    if (!ln.empty() && ln.back() == '\n') ln.pop_back();
                                    ring.push_back(ln);
                                    if (ring.size() > (size_t)n) ring.erase(ring.begin());
                                }
                                std::fclose(r);
                                if (ring.empty()) {
                                    view.push_line("[tail] the tape is blank");
                                } else {
                                    view.push_line("[tail] last " + std::to_string(ring.size()) + ":");
                                    for (const std::string& ln : ring) view.push_line("  " + ln);
                                }
                            }
                        }
                    } else if (submitted.rfind("/hi", 0) == 0) {
                        std::string rest = trim(submitted.substr(3));
                        if (rest.empty() || rest == "list") {
                            view.push_line("[hi] " + std::to_string(view.highlights().size()) + " rule(s)");
                            for (const HiRule& r : view.highlights()) view.push_line("  " + r.text);
                        } else if (rest == "clear") {
                            view.clear_highlights();
                            save_rc(view.highlights(), aliases, gags, acts);
                            view.push_line("[hi] cleared (rc updated)");
                        } else {
                            std::string text = rest, color;
                            const size_t sp = rest.rfind(' ');
                            if (sp != std::string::npos) {
                                const std::string tail = rest.substr(sp + 1);
                                if (is_color_name(tail)) { text = rest.substr(0, sp); color = tail; }
                            }
                            view.add_highlight(text, color_attr(color));
                            std::string note = "[hi] painted: " + text;
                            if (!color.empty()) note += " (" + color + ")";
                            view.push_line(note);
                            log_line(log, "[hi] ", note);
                            save_rc(view.highlights(), aliases, gags, acts);
                        }
                    } else if (submitted.rfind("/alias", 0) == 0) {
                        std::string rest = trim(submitted.substr(6));
                        if (rest.empty() || rest == "list") {
                            view.push_line("[alias] " + std::to_string(aliases.size()) + " macro(s)");
                            for (const Alias& a : aliases) view.push_line("  " + a.name + " = " + a.expansion);
                        } else if (rest == "clear") {
                            aliases.clear();
                            save_rc(view.highlights(), aliases, gags, acts);
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
                                save_rc(view.highlights(), aliases, gags, acts);
                            }
                        }
                    } else if (submitted.rfind("/ungag", 0) == 0) {   // one hand, one law undone
                        std::string rest = trim(submitted.substr(6));
                        if (rest.empty()) {
                            view.push_line("[ungag] usage: /ungag <text>");
                        } else {
                            bool removed = false;
                            for (size_t i = 0; i < gags.size(); ++i) {
                                if (gags[i] == rest) { gags.erase(gags.begin() + (long)i); removed = true; break; }
                            }
                            view.push_line(removed ? "[ungag] spoke again: " + rest : "[ungag] no such gag");
                            if (removed) save_rc(view.highlights(), aliases, gags, acts);
                        }
                    } else if (submitted.rfind("/gag", 0) == 0) {
                        std::string rest = trim(submitted.substr(4));
                        if (rest.empty() || rest == "list") {
                            view.push_line("[gag] " + std::to_string(gags.size()) + " gag(s)");
                            for (const std::string& g : gags) view.push_line("  " + g);
                        } else if (rest == "clear") {
                            gags.clear();
                            save_rc(view.highlights(), aliases, gags, acts);
                            view.push_line("[gag] cleared (rc updated)");
                        } else {
                            bool replaced = false;
                            for (const std::string& g : gags) if (g == rest) { replaced = true; break; }
                            if (!replaced) gags.push_back(rest);
                            view.push_line("[gag] silenced: " + rest);
                            log_line(log, "[gag] ", rest);
                            save_rc(view.highlights(), aliases, gags, acts);
                        }
                    } else if (submitted.rfind("/unact", 0) == 0) {   // one hand, one reflex undone
                        std::string rest = trim(submitted.substr(6));
                        if (rest.empty()) {
                            view.push_line("[unact] usage: /unact <text>");
                        } else {
                            bool removed = false;
                            for (size_t i = 0; i < acts.size(); ++i) {
                                if (acts[i].text == rest) { acts.erase(acts.begin() + (long)i); removed = true; break; }
                            }
                            view.push_line(removed ? "[unact] disarmed: " + rest : "[unact] no such act");
                            if (removed) save_rc(view.highlights(), aliases, gags, acts);
                        }
                    } else if (submitted.rfind("/count", 0) == 0) {   // the numbers with names
                        const std::string cname = trim(submitted.substr(6));
                        if (cname.empty()) {
                            view.push_line("[count] " + std::to_string(counts.size()) + " counter(s)");
                            for (const Counter& c : counts) view.push_line("  " + c.name + ": " + std::to_string(c.v));
                        } else {
                            Counter* c = nullptr;
                            for (Counter& cc : counts) if (cc.name == cname) { c = &cc; break; }
                            if (!c) { counts.push_back({cname, 0}); c = &counts.back(); }
                            c->v++;
                            log_line(log, "[count] ", cname + " = " + std::to_string(c->v));
                            view.push_line("[count] " + cname + ": " + std::to_string(c->v));
                        }
                    } else if (submitted.rfind("/act", 0) == 0) {
                        std::string rest = trim(submitted.substr(4));
                        if (rest.empty() || rest == "list") {
                            view.push_line("[act] " + std::to_string(acts.size()) + " reflex(es)");
                            for (const ActRule& a : acts) view.push_line("  " + a.text + " => " + a.response + (a.cond.empty() ? "" : " WHEN " + a.cond));
                        } else if (rest == "clear") {
                            acts.clear();
                            save_rc(view.highlights(), aliases, gags, acts);
                            view.push_line("[act] cleared (rc updated)");
                        } else {
                            const size_t sep = rest.find("=>");
                            if (sep == std::string::npos) {
                                view.push_line("[act] usage: /act <text> => <response> [< WHEN present>]");
                            } else {
                                const std::string text = trim(rest.substr(0, sep));
                                std::string resp = trim(rest.substr(sep + 2));
                                std::string cond;
                                const size_t wc = resp.find(" WHEN ");
                                if (wc != std::string::npos) { cond = trim(resp.substr(wc + 6)); resp = trim(resp.substr(0, wc)); }
                                if (text.empty() || resp.empty() || (wc != std::string::npos && cond.empty())) {
                                    view.push_line("[act] usage: /act <text> => <response> [< WHEN present>]");
                                } else {
                                    bool replaced = false;
                                    for (ActRule& a : acts) {
                                        if (a.text == text) { a.response = resp; a.cond = cond; replaced = true; break; }
                                    }
                                    if (!replaced) acts.push_back(ActRule{text, resp, cond});
                                    view.push_line("[act] armed: " + text + " => " + resp + (cond.empty() ? "" : " WHEN " + cond));
                                    log_line(log, "[act] ", text + " => " + resp + (cond.empty() ? "" : " WHEN " + cond));
                                    save_rc(view.highlights(), aliases, gags, acts);
                                }
                            }
                        }
                    } else {
                        std::string before = submitted;
                        std::vector<std::string> walksteps;
            if (parse_walk(submitted, &walksteps)) {
                view.push_line("[walk] " + submitted + " -> " + std::to_string(walksteps.size()) + " step(s)");
                log_line(log, "[walk] ", submitted + " (" + std::to_string(walksteps.size()) + " steps)");
                if (alive) for (const std::string& wd : walksteps) net.send_line(wd);
                goto done_submit;
            }
                        if (expand_aliases(&aliases, &submitted) && submitted != before) {
                            view.push_line("[alias] " + before + " -> " + submitted);
                            log_line(log, "[alias-echo] ", before + " -> " + submitted);
                        }
                        if (alive) {
                            const std::string tail = net.flush_pending();
                            if (!tail.empty()) {
                                if (whole_tail_is_prompt(tail)) {
                                    log_line(log, "[tail-prompt] ", tail);
                                } else {
                                    push(tail);
                                    log_line(log, "[tail] ", tail);
                                    if (ends_with_password_prompt(tail)) expect_secret = true;
                                }
                            }
                        }
                        if (expect_secret) {
                            view.push_line("> " + std::string(submitted.size(), '*'));
                            log_line(log, "[secret withheld]", "");
                            expect_secret = false;
                        } else {
                            if (!submitted.empty()) { push("> " + submitted); log_line(log, "> ", submitted); }
                        }
                        if (alive) net.send_line(submitted);
                    }
                    done_submit:;
                }
            }
        }
        if (r > 0 && alive && (pf[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            std::vector<std::string> lines;
            net.pump(&lines);
            for (const std::string& s : lines) {
                const bool pw = ends_with_password_prompt(s);   // the secret law gates the reflexes
                push(s);
                log_line(log, "", s);
                if (pw) {
                    expect_secret = true;
                    continue;                                   // never arm, never fire, on secrets
                }
                if (!acts.empty() && alive) {
                    if (contains_ci(s, "Exits:")) room_buf.clear();   // a new room render begins
                    room_buf.push_back(s);
                    for (const ActRule& a : acts) {
                        if (!contains_ci(s, a.text)) continue;        // the wire must say it
                        if (!a.cond.empty()) {                        // presence demanded: the room must show it
                            bool met = false;
                            for (const std::string& rl : room_buf) if (contains_ci(rl, a.cond)) { met = true; break; }
                            if (!met) continue;                        // unmet is silence, reflex stays armed
                        }
                        net.send_line(a.response);
                        view.push_line("[act] " + a.response);
                        log_line(log, "[act-fired] ", a.text + " => " + a.response + (a.cond.empty() ? "" : " (WHEN " + a.cond + ")"));
                    }
                }
            }
            if (!net.live()) {
                alive = false;
                net.shutdown();
                room_buf.clear();                          // room-sense dies honestly, again
                next_dial = ::time(nullptr) + 1;          // redial starts in one second
                view.push_line("[reconnect] the wire fell - redialing every 10s, /quit to stop");
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
