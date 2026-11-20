#include "input.h"
#include "net.h"
#include "view.h"
#include <ncurses.h>
#include <poll.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static void log_line(std::FILE* log, const char* tag, const std::string& s) {
    if (log) std::fprintf(log, "%s%s\n", tag, s.c_str());
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
    std::string status;
    if (demo) {
        status = "ansi demo | the palette room";
        view.push_line("Time-It ANSI demo - painted by the same engine the scroll uses.");
        view.push_line("\x1b[30mblack\x1b[0m \x1b[31mred\x1b[0m \x1b[32mgreen\x1b[0m \x1b[33myellow\x1b[0m \x1b[34mblue\x1b[0m \x1b[35mmagenta\x1b[0m \x1b[36mcyan\x1b[0m \x1b[37mwhite\x1b[0m");
        view.push_line("bold rail: \x1b[1;30mbright black\x1b[0m \x1b[1;31mbright red\x1b[0m \x1b[1;32mbright green\x1b[0m \x1b[1;33mbright yellow\x1b[0m \x1b[1;34mbright blue\x1b[0m \x1b[1;35mbright magenta\x1b[0m \x1b[1;36mbright cyan\x1b[0m \x1b[1;37mbright white\x1b[0m");
        view.push_line("extended 256: \x1b[38;5;208mpalette orange\x1b[0m \x1b[38;5;46mmatrix green\x1b[0m \x1b[38;5;196malert red\x1b[0m \x1b[38;5;21mdeep blue\x1b[0m");
        view.push_line("truecolor: \x1b[38;2;255;140;0mtruecolor orange\x1b[0m \x1b[38;2;64;224;208mtruecolor turquoise\x1b[0m \x1b[38;2;128;0;255mtruecolor violet\x1b[0m");
        view.push_line("wrap + color interlocked: \x1b[36mthe cyan sentence must bend at the right edge and stay cyan on every row it occupies, seam and all, until the last word lands.\x1b[0m");
        view.push_line("swallow test: \x1b[2K\x1b[1;33mthis bright yellow follows a consumed cursor-clear; nothing garbled above.\x1b[0m");
        view.push_line("/quit to leave.");
    } else {
        status = std::string(host) + ":" + std::to_string(port) + " | live";
        view.push_line("Time-It 1.0 - linked to " + std::string(host) + ":" + std::to_string(port));
        view.push_line("Type commands below. Arrows recall history, PageUp browses, /quit leaves (Ctrl-D too).");
    }

    for (;;) {
        view.render(line.text(), line.cursor(), status, alive ? net.pending() : std::string());
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
                    if (alive) {
                        const std::string tail = net.flush_pending();
                        if (!tail.empty()) { view.push_line(tail); log_line(log, "[tail] ", tail); }
                    }
                    if (!submitted.empty()) { view.push_line("> " + submitted); log_line(log, "> ", submitted); }
                    if (alive) net.send_line(submitted);      // empty Enter still reaches the pager
                }
            }
        }
        if (r > 0 && alive && (pf[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            std::vector<std::string> lines;
            net.pump(&lines);
            for (const std::string& s : lines) {
                view.push_line(s);
                log_line(log, "", s);                       // the tape stays raw: SGR rides into the log unhonored or hidden
            }
            if (!net.live()) {                               // the link dropped; the room keeps its history
                alive = false;
                net.shutdown();
                status = std::string(host) + ":" + std::to_string(port) + " | link dropped";
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
