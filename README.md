# Game of Life + Life Duel

Two programs that share one C++ simulation core:

| Executable | What it is |
|---|---|
| `life_classic` | Conway's Game of Life with a native GUI (SFML): draw, place well-known patterns, continuous speed control and a max-speed mode. |
| `life_battle` | **Life Duel**, a red-vs-blue 1v1 played in the browser. A small built-in HTTP server runs the simulation in C++, and the browser handles placement and rendering. |
| `life_arena` | Trains Life Duel bots by self-play on one fixed battlefield (command line, no GUI). |

The rules of the duel are in **[RULES.md](RULES.md)**.

## Building

Requirements: a C++17 compiler, CMake ≥ 3.16, and SFML 2.5 for the classic GUI
(`sudo apt install libsfml-dev`). The battle server has no dependencies beyond
POSIX sockets.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build        # core tests
```

## Running

```bash
./build/life_classic                    # 400x250 torus
./build/life_classic --size 800x500 --no-wrap
./build/life_battle                     # serves http://127.0.0.1:8080/ and opens the browser
./build/life_battle --port 9000 --host 0.0.0.0 --no-browser   # e.g. play from another device in the LAN
```

### Classic controls

| | |
|---|---|
| Left drag / right drag | draw / erase cells |
| Click a pattern in the list, then left-click | stamp it (`R` rotate, `F` flip, right-click or `Esc` to stop) |
| Mouse wheel, middle drag, arrow keys | zoom, pan |
| `Space`, `N`, `C`, `X` | play/pause, step, clear, randomize |
| Speed slider, `+`/`−` | 1–500 generations per second (logarithmic) |
| **MAX SPEED**, `M` | compute as many generations as fit into each frame |
| `G`, `0` / `Home` | grid lines, fit view |

## Training bots (`life_arena`)

```bash
./build/life_arena train --league league.txt --iterations 50   # creates or resumes league.txt
./build/life_arena show --league league.txt                    # armies with their Nash weights
./build/life_arena show --league league.txt 12                 # draw army #12
./build/life_arena match --league league.txt 12 7              # #12 as red against #7 as blue
./build/life_arena bench                                       # engine speed comparison
```

How it works:

- **Armies are bought from the arsenal**, exactly like a human player does:
  pieces with position, rotation and mirror, inside the red zone and within the
  budget. Blue plays the mirror image (or the 180° rotation on a rotational
  battlefield), so one army describes both sides and the game is exactly fair.
- **Score instead of win/lose:** each battle scores Red in [-1, 1]:
  0.7 × result (win +1, draw 0, loss -1) + 0.3 × final margin
  (red − blue) / (red + blue).
- **League with a Nash mixture (PSRO):** the league stores every army found so
  far and the payoff of every pair. Each iteration computes the Nash mixture
  of the league (the optimal way to choose randomly among its armies, via
  regret matching), then an evolutionary search (mutations: nudge, move,
  turn, swap, buy, sell, copy, shift; plus crossover) looks for an army that
  beats that mixture. It joins the league if it gains more than `--min-gain`.
  Because new armies must beat the mixture, not just the last champion,
  rock-paper-scissors cycles cannot make the league forget what it learned.
- **gain** in the log is the best score the search reaches against the
  mixture. It falls as the league gets harder to exploit.
- Training is resumable: the league is saved after every iteration, and
  `league.txt.json` holds the mixture and all armies (red and blue
  placements) for other programs.
- Battles run on `gol::FastBattle`, a bit-parallel engine (64 cells per
  machine word, about 15× faster than the reference `gol::Battle`, checked
  cell by cell against it in the tests), on all cores.

## Layout

```
core/       gol_core library
  life.*      classic B3/S23 grid (torus or bounded), with cell ages for colouring
  patterns.*  RLE parser, pattern library, battle arsenal and prices
  battle.*    coloured battle rules, end detection, battlefield presets, board encoding
  fast_battle.*  the same rules bit-parallel, for bulk simulation
classic/    life_classic (SFML) + a tiny immediate-mode widget layer
battle/     life_battle: HTTP server + JSON API
  web/        index.html, style.css, app.js (no build step, no external libraries)
arena/      life_arena: armies, mutation, matches, Nash solver, league training
tests/      core_tests
assets/     DejaVu fonts for the native GUI
```

### Battle server API

| Endpoint | Purpose |
|---|---|
| `GET /api/config` | arsenal (patterns + costs), battlefield presets, board sizes |
| `GET /api/battlefield?w&h&preset&count&seed` | generated grey battlefield |
| `POST /api/battle?w&h&maxgen&variant` (body = board) | start a battle, returns the first frame (`maxgen=0`: no limit) |
| `GET /api/step?id&n=K` or `&ms=T` | advance K generations, or as many as fit into T ms; `board=0` omits the board |

Boards are sent as run-length text: `.` dead, `r` red, `b` blue, `g` grey, each
optionally prefixed with a repeat count (`12.3r.g`).
