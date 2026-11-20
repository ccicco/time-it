# Time-It doctrine (frozen 2026-11-20)

Time-It: Curtiss's terminal MUD client for TimeMUD. C++20, ncurses,
Termux first. One grammar for keyboard and init file. No readline -
the input line is ours (the founding criterion: arrow-key history
must work on the Termux soft keyboard and extra-keys row, day one).

## The wire

Raw TCP to TimeMUD (default 127.0.0.1:2600), no telnet negotiation.
Hybrid tags: after the `client timeit 1` handshake, TimeMUD emits quiet
ti: lines (room/self/entity/way/step) beside unchanged prose; the
client hides ti: from the scroll and renders them (map, status). Prose
outranks tags on any disagreement. Client degrades gracefully without
tags. Reconnect = fresh handshake + full-snapshot resync, backoff
1/2/5/10s, scrollback survives. External protocols (MCP/MXP/GMCP/etc)
all tossed; their ideas mined, not adopted. A telnet shim enters scope
only if a third-party MUD is ever fronted.

## The command char

`/` is the client namespace, a hard rule, nothing configurable to
break: known /commands act locally; unknown /word errors locally and
never reaches the wire; `//text` passes through with one slash
stripped. Comments in init files: `#`.

## Init files

~/.timeitrc auto-loads every start (absence fine, defaults govern).
Per-character: ~/.timeit/<char>.rc via --char, layered after the
global file. Format = the input line itself, one grammar, idempotent
lines (loading twice leaves one alias, not two). Boot-load and /load
hot-load run the SAME loader routine. Hot-load is ruled in: settings
apply live, no restart, no reconnect.
HARD WALL: init files are client-side only. They configure; they
never speak to the wire. No auto-login blocks, no on-connect sends;
credentials ride behind a live keypress. (The s2 FredMUD double-free
crash is the scar under this law.)

## Features, in founding order

Aliases (every keystroke) -> highlights (every line) -> triggers
(occasion tools). Baseline for aliases and sessions: at least as good
as OLD tintin++ - the Termux tt++ regressions are the founding reason
this client exists. Tossed with reasons: scripting language, timers,
gag, pathfinding/auto-route, speedwalk (parked, toss-lean), ANSI from
server, wall-clock timestamps (the world keeps its own time),
bells/sound, multi-session tabs, encryption on a loopback wire,
event-loop libraries.

## Libraries

In: ncurses (panes), PCRE2 (trigger round; std::regex first), termux
wake-lock (sessions outlive screen timeouts), Catch2/doctest considered
post-R1 as dev harness. Out: readline (owns the scar we exist to fix -
RULED OUT BY CURTISS, CONFIRMED), FTXUI/notcurses (ncurses loyalty
until a real wall), config parsers (the init grammar IS the input
grammar). Principle: take what enriches wire or pixels; refuse what
sits between thumbs and keystrokes.

## Protocols (a standing category)

ti: handshake + tag family: KEPT (hybrid). Raw TCP line protocol: ruled
R0. External family: tossed, ideas mined (MXP clickable exits, GMCP's
structured shape already honored by ti:). Telnet shim door: parked,
third-party-MUD only. Sync contract (fresh handshake + snapshot
resync): kept. Session-collision contract: open until the R8 census.

## Round plan (frozen)

R1 link and speaks (connect, scroll, input+history, browse, log, /quit)
R2 words and colors (wrap, status polish, first highlights, raw view)
R3 aliases and the boot grammar (interpreter, init, hot-load)
R4 ti: talks (TimeMUD emit-side round, riding shot)
R5 the grid (map pane; overlay vs split judged on the phone in-round)
R6 triggers (PCRE2 lands)
R7 tab (way-word completion)
R8 sessions with manners (second-connection census, ask policy)

Every round: warning-free compile or nothing counts; censuses when they
apply; commit AND push in the same chunk as the gates.

Session laws: no blind auto-login ever; second-session connect is an
asked question, never an assumption; reconnect trusts only a fresh
handshake.

## Structure law

src/ rooms are named EXACTLY after the ruled feature categories -
connection/, display/, input/, automation/, world/, data/, protocols/ -
with Dev & QA living in docs/ (doctrine now; tests/ there if a harness
round rides). No renaming: automation/ is not "lang", protocols/ is
not "wire". All seven runtime rooms are created at the structure
commit with .gitkeep stubs; every category is ruled-in so none is
speculative, and files arrive per round into their booked room:
R1 -> connection/net.cpp, display/view.cpp, input/input.cpp, main.cpp
at root; R3 -> automation/ + data/; R4 -> protocols/; R5 -> world/ +
display map code. A future feature with no room ships WITH its room
and this file gains the category line. include/ mirrors src/ flatly.
build/ is ephemeral, swept on clean.

The category map is falsifiable on purpose. When a round's file would
be placed where it does not truly belong, that misfit is a CATEGORY
ANNOUNCING ITSELF: stop, name it, create its room with its files, and
add the category here. Never shoe-horn a file into a near room to
avoid the ceremony. The map above is what is known so far, not what
is true forever.
