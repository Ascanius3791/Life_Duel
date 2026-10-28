# Game of Life + Life Duel

Two programs that share one C++ simulation core:

| Executable | What it is |
|---|---|
| `life_classic` | Conway's Game of Life with a native GUI (SFML): draw, place well-known patterns, continuous speed control and a max-speed mode. |
| `life_battle` | **Life Duel**, a red-vs-blue 1v1 played in the browser. A small built-in HTTP server runs the simulation in C++, and the browser handles placement and rendering. |
| `life_arena` | Trains Life Duel bots by self-play on one fixed battlefield (command line, no GUI). |
| `life_challenge` | Lets the computer attack a Challenge level: the fewest red cells that beat its bot. |

Life Duel also runs without any server, as a static web site: the same C++
engine compiled to WebAssembly. That is how it is published on GitHub Pages and
played **online against a friend** on another computer (see [Play online](#play-online)).

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

By default the build is tuned for the building machine (`-march=native`,
option `GOL_NATIVE`), which makes bot training noticeably faster through
AVX2/AVX-512 and POPCNT. For binaries that must run on other/older CPUs,
configure with `-DGOL_NATIVE=OFF`.

## Running

```bash
./build/life_classic                    # 400x250 torus
./build/life_classic --size 800x500 --no-wrap
./build/life_battle                     # serves http://127.0.0.1:8080/ and opens the browser
./build/life_battle --port 9000 --host 0.0.0.0 --no-browser   # e.g. play from another device in the LAN
```

### Placing an army

All controls are in [RULES.md](RULES.md#6-controls-browser). Two of them shape
an army cell by cell:

- **Choosing the phase:** while a pattern is selected (dragged or clicked, not
  yet on the board), `,` and `.` step it one generation back and forward
  (`Shift`: 10, up to 1000). It evolves on its own, on an unbounded plane, and
  the preview shows the generation, its living cells and its price. So a gun
  can be placed already running, or a methuselah halfway through. A piece costs
  exactly its living cells, so an advanced pattern is placed as single cells
  (`cell` pieces): armies keep their format everywhere (saved and published
  armies, army codes, online play, league files). If the advanced pattern is
  the arsenal piece again (a still life, an oscillator or spaceship after a
  full period), it is placed as that piece.
- **Taking cells out:** right-click removes a placed pattern as a whole;
  `Ctrl+right-click` (or pointing at the cell and pressing `X`) removes just the
  cell under the pointer. The rest of the pattern stays as single cells and one
  cell's points come back.

### Playing against the bot

Click **vs Bot** at the top left of Life Duel. The bot is a league trained by
`life_arena` (see below). The leagues in `assets/leagues/` were trained on the
Medium board with 120 points, a 2000 generation limit and the Birthright rule,
one per battlefield: *Hive lattice* (the default, also the default battlefield
for two players), *No man's land* (`open`) and *Random (mirrored)* with 900 grey
cells, seed 1 (`mirror900`).

- Pick your side (**You play Red / Blue**). The bot plays the other one.
- The battlefield and all settings are locked to the ones the bot was trained
  for, because its armies are only good there. The exceptions are the
  **generation limit** and **home cells don't count** (Settings). Like
  everywhere else, the battle runs without a limit by default, so you see how it
  really ends. The progress bar then marks the limit the bot was trained with
  (2000), a message shows the standings when the battle passes it, and the
  result dialog says who would have won there. The bots were trained with all
  cells counted.
- The bot draws its army when the preparation starts, before you place
  anything, and it stays hidden until the battle begins. **Hard** draws from the
  league's Nash mixture, **Easy** picks any army of the league with equal chance
  (including weak early ones). After the battle the result names the army it
  played.

`life_battle` offers every `*.json` league export in `assets/leagues/` (found
next to the sources or as `leagues/` next to the executable). To play against a
league of your own, pass it instead, as often as you like:

```bash
./build/life_battle --league league.txt            # league.txt.json is served
./build/life_battle --league a.txt --league b.txt  # a choice of bots
```

The league file is re-read for every game, so you can play against a league
while it is still being trained.

### Watching bots

Click **Watch** to see two armies of a league fight on its battlefield (the
Hive league by default; the list next to the pickers switches leagues). Each
side is **🎲 Random (Nash)** — drawn from the league's Nash mixture, the
default — **🎲 Random (any)**, or one particular army: the list holds every
army of the league (Nash support first), your saved armies and the published
ones. **◀ ▶** step through them, 🎲 draws again, 💾 keeps the army on the board
in your saved armies. **New game** draws the random sides again.

### Solo and Challenge

Two one-player modes on fixed **levels**: *Hive lattice*, *No man's land* and
*Champion*, all on the board of the bot leagues (260×128, 120 points,
Birthright). Every battle runs to its end with **no generation limit** (until
the board repeats or an army dies out) and **home cells never count**; nothing
can be changed, so records stay comparable. Your best result per level is kept
in this browser and shown in the level list; **Best army** puts the army of
your record back on the board.

- **Solo**: Red plays alone, there is no Blue. The score is the number of red
  cells **outside your own area** when the board starts repeating (the average
  over the cycle; 0 if Red dies out). Grey cells never change colour under
  Birthright, red grows by births next to them, so red cells are what counts.
  On *No man's land* there is no grey at all: the task is plain growth. On
  *Champion* the strongest army of our pool training on that field
  (`data/pool/n100m.txt`, not in the repository; best against the open
  league's Nash mixture) stands in grey where Blue's army would be, and flies
  at you.
- **Challenge**: Red against a fixed, visible bot army. Win, and the record is
  the **fewest living red cells** on the starting board that still won (points
  spent break ties). The bots: the army with the largest Nash weight of the
  Hive league and of the open league, and on *Champion* the champion itself
  (they were trained with a 2000 generation limit and all cells counted).
  Where the computer has attacked a level (see
  [`life_challenge`](#exploiting-the-challenges-life_challenge)), the level
  text says how few cells it needed and **Computer's army** puts its army on
  the board. A win with exactly that army is not stored as your record.

New levels are entries of `LEVELS` in `battle/web/levels.js` (armies written as
in a league file). The battle runs with `solo=1` (see the API below), so a board
without Blue is no elimination.

### Saved and published armies

Each arsenal has **Save** and **Load**. Saved armies have a name and stay in
this browser (`localStorage`). **Armies** (top right) lists them: load one for
a side, rename, delete, copy its *army code* (one line, `LD1|52x128|name|...`,
to pass on by hand; paste one into the import field) or **Publish** it.

Every army has an **ID**: the first 12 hex digits of the SHA-256 of its living
cells in the zone (stored as Red would place it; Blue gets the mirror image).
Equal boards have equal IDs, however they were built — saved, published and
league armies alike (the bot's army is named with its ID after a battle).

**Publishing** goes through GitHub issues, so it needs no server: *Publish*
opens a pre-filled issue in the repository (`ARMY_REPO` in
`battle/web/armies.js`, default `Ascanius3791/Life_Duel`); the player clicks
*Create* (a GitHub account is needed for that, not for browsing). Every open
issue that contains a valid army code is a published army, listed for every
player under **Armies → Published** (read with the public GitHub API, cached
for 3 minutes; GitHub allows 60 requests per hour per visitor without login).
If the same army is published twice, the first issue counts. Closing an issue
unpublishes the army. **The repository must be public** for this; a private
code repository can point `ARMY_REPO` (or `window.LIFE_DUEL_ARMY_REPO`) at a
separate public one.

### Play online

Click **Online** at the top left. You get an invitation link; send it to your
friend (chat, mail, ...). When they open it, the two browsers connect directly
(WebRTC through [PeerJS](https://peerjs.com/); PeerJS's free public server only
helps them find each other, no game data goes through it).

- The host's settings and battlefield are used; the host can still change them
  until they lock in their army (the round then starts over for both). The
  host plays Red first, sides swap with every rematch.
- Both place their armies at the same time and cannot see each other's. **Ready**
  locks the army in: the page sends only a fingerprint (SHA-256 of the army
  and a random salt). When both are locked in, both armies are revealed; each
  page checks the other's against its fingerprint and the placement rules.
- Each page then simulates the battle itself. They are deterministic and
  compare checksums of the starting board and of the result (including its
  whole history), so a desync would be reported.
- After the battle, **Rematch** asks the friend for another round. If the
  connection drops, the page says so; **New link** starts a new game.

The link only works once and while the host keeps the page open. Online play
works from the GitHub Pages site and from `life_battle` alike (it then needs
internet access for PeerJS; a link to `127.0.0.1` of course only reaches the
same machine, so share the Pages link or run `life_battle --host 0.0.0.0`).
Very restrictive networks (symmetric NAT, some corporate/mobile networks) may
block direct WebRTC connections; that would need a TURN server
(`window.LIFE_DUEL_PEER_OPTIONS = { config: { iceServers: [...] } }` before
`online.js` loads).

### The static web site (GitHub Pages)

`tools/build_site.sh` compiles the battle engine to WebAssembly with
[Emscripten](https://emscripten.org/), runs the core tests under node (the
browser must compute exactly what the native build computes) and writes the
site to `dist/`:

```bash
# once: Emscripten SDK, e.g. in ~/emsdk (the script finds it there or on PATH)
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest

tools/build_site.sh                       # -> dist/
python3 -m http.server -d dist 8000       # open http://localhost:8000/
```

In the static site, `static-backend.js` answers the same `/api/...` calls
`app.js` makes to `life_battle` (see the API below), by running the C++ engine
(`battle/wasm_engine.cpp`) in a Web Worker; the bot leagues are plain files
listed in `leagues/index.json`.

**Publishing:** `.github/workflows/pages.yml` builds and tests everything on
every push to `main` and deploys `dist/` to GitHub Pages. In the repository's
*Settings → Pages*, set *Source* to **GitHub Actions** once. The game is then at
`https://<user>.github.io/<repository>/`.

### Classic controls

| | |
|---|---|
| Left drag / right drag | draw / erase cells |
| Click a pattern in the list, then left-click | stamp it (`R` rotate, `F` flip, right-click or `Esc` to stop) |
| `,` / `.` while a pattern is selected | step it a generation back / forward before stamping (`Shift`: 10; evolved without edges, the preview shows the generation and its cells) |
| Mouse wheel, middle drag, arrow keys | zoom, pan |
| `Space`, `N`, `C`, `X` | play/pause, step, clear, randomize |
| Speed slider, `+`/`−` | 1–500 generations per second (logarithmic) |
| **MAX SPEED**, `M` | compute as many generations as fit into each frame |
| `G`, `0` / `Home` | grid lines, fit view |

## Training bots (`life_arena`)

```bash
./build/life_arena train --league league.txt --iterations 50   # creates or resumes league.txt
./build/life_arena train --twins --league league.txt           # twin leagues, stops by itself (see below)
./build/life_arena show --league league.txt                    # armies with their Nash weights
./build/life_arena show --league league.txt 12                 # draw army #12
./build/life_arena match --league league.txt 12 7              # #12 as red against #7 as blue
./build/life_arena bench                                       # engine speed comparison (random armies)
./build/life_arena bench --league league.txt                   # ... on battles between league armies
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
  mixture. It falls as the league gets harder to exploit, but on open
  battlefields it stays high for a long time, so it is no stopping criterion
  (and comparing a league with its own older versions says nothing: PSRO
  never makes the mixture worse against them).
- Training is resumable: the league is saved after every iteration, and
  `league.txt.json` holds the mixture and all armies (red and blue
  placements) for other programs.
- Battles run on `gol::FastBattle` on all cores. It is checked cell by cell
  against the reference `gol::Battle` in the tests and is about 100-250×
  faster than it on one thread (1-3 ms for a 2000-generation battle on
  260×128):
  - bit planes (alive, red, blue), 64 cells per word, stored in columns so
    that a SIMD vector holds 4 rows of one column (one AVX2 register); the
    neighbours are counted with a bit-sliced full-adder tree;
  - only tiles (64×4 cells) whose neighbourhood changed are recomputed: a
    tile whose surroundings equal those of two generations ago gets the
    state it had one generation ago, which is already in the back buffer.
    Empty space, still lifes and period-2 oscillators cost nothing;
  - cell counts and the repetition hash are updated from the changed words
    only (the hash is a sum of per-word hashes).
- The search never plays a battle twice: league armies take their scores
  from the payoff matrix, and an army that comes up again in a search
  reuses its scores (results are identical to replaying).

### Twin training (`train --twins`)

Twin training answers "when is the league good enough?":

- **Two leagues, A and B,** are trained with the same settings but different
  search seeds (`--rng N` for A, N+1000 for B). Both searches run in lockstep
  in one process and every search generation of both is played as one batch,
  which keeps the cores busy when the Nash support is small. A is exactly the
  league a single run with `--rng N` would produce.
- **Cross-evaluation** every `--eval-every K` iterations (default 5):
  `xA` is the best score any army of B reaches against A's Nash mixture (same
  scoring as the league), `xB` the same the other way round. B never trained
  against A, so `xA` is a lower bound on how exploitable A really is, and it
  measures whether A's strength generalises to strategies it has never seen.
  The search gain is a rough upper estimate (the search may also fail to find
  what B found, so on small searches xA can exceed the gain). The payoffs
  between the twins are cached; only armies added since the last evaluation
  play.
- **Stopping rules,** checked at every evaluation, the first one wins and is
  logged:
  1. *converged:* `xA` and `xB` are both below `--eps` (default 0.05);
  2. *plateau:* max(`xA`, `xB`), averaged over the last 3 evaluations, has
     not dropped by `--delta` (default 0.02) below its best value for
     `--patience` evaluations in a row (default 6). The best value can only
     drop by delta at a time, so this always ends a run;
  3. *budget:* `--iterations N` (here a maximum, default 1000) or
     `--max-hours H` of this run; a final evaluation is made first.
- **Output:** at every evaluation and at the end, `FILE` and `FILE.json`
  hold the **union of A and B** with the Nash mixture over the union (origins
  end in "(A)" or "(B)"), so a long run is always usable. `FILE.json` gets an
  extra `training` member (settings of the rules, stop reason and the
  evaluation curve) that the browser ignores. Everything else lives in
  `FILE.twins/`, which is not offered as a bot (`life_battle` and the static
  site only take `*.json` directly in `assets/leagues/`):
  `A.txt`, `B.txt` (the twins, saved after every iteration), `state.txt`
  (iteration, elapsed time) and `eval.csv` (iteration, gainA, gainB, xA, xB,
  league and support sizes, elapsed seconds).
- Running the same command again resumes from `FILE.twins/` (the stop rules
  see the whole history). `--iterations 0` just re-evaluates and rewrites
  `FILE`. A `FILE` that exists without `FILE.twins/` is never overwritten.

```bash
./build/life_arena train --twins --league assets/leagues/open-twins-260x128.txt \
    --preset empty --size 260x128 --budget 120 --maxgen 2000 \
    --support 12 --pop 48 --gens 30 --max-hours 10
```

### Pool training (`pool`, experimental)

League training searches every new army against the few strongest armies of
the Nash mixture (`--support`, 6 by default). The armies it finds beat those
few, but carry over badly to armies they never met. `life_arena pool` keeps a
pool of up to `--pool-size` armies (default 1000) together with the complete
table of their results against each other. Battles are deterministic, so one
battle per pair is exact, and an army's **score** is its exact mean against
every other army in the pool *now*: no luck from drawn opponents, and wins
against armies that have left the pool stop counting.

- Each iteration searches for armies against armies drawn at random from the
  pool, drawn anew for every search round (`--gens` rounds), so it cannot
  specialise on a few opponents. With `--halving 4,8,16` every candidate plays
  4 of them, the better half plays on up to 8, and so on.
- The entrance test: every new army of the last search round plays the pool
  armies in the same random order, a batch at a time (16 first, then 8 or a
  quarter of those played so far). An army drops out once we are
  `--confidence` sure (default 0.95) that its mean against the whole pool will
  end below `--min-gain` (default 0.02: it must beat the pool), or that
  `--max-entrants` others will end above it. The rest play everyone, which
  fills their rows of the table; a candidate that fails usually costs a few
  dozen battles instead of a whole pool.
- Those that passed join best first, at most `--max-entrants` per search
  (default 2% of the pool size), as long as they still beat the pool (which
  now includes the armies that joined before them) and are not **copies**: an
  army whose results against the pool correlate above `--max-similar`
  (default 0.9) with those of a pool army behaves like it and stays out. Once
  the pool is full, each one replaces the army with the lowest score; each
  replacement is one **generation**, and the time before is the warm-up.

```bash
./build/life_arena pool --league data/pool/open.txt --from assets/leagues/open-twins-260x128.txt \
    --pool-size 1000 --pop 32 --gens 10 --halving 4,8,16 --max-hours 1
```

`--from` seeds a new pool with the armies of existing leagues (every pair
plays once at the start; a seed larger than the pool drops its weakest);
running the same command again continues the pool. `FILE.state` keeps the
table (4 × size² bytes: 4 MB at 1000, 400 MB at 10000; saved at most once a
minute), `FILE.csv` logs every iteration. `tools/heldout.py FILE` plays the
pool's armies against the Nash mixture of an independently trained league (by
default `assets/leagues/open-260x128.txt`).

### Battle logs and the result predictor (experimental)

`train --log-matches P` records every battle (both armies, the score, and the
red/blue cell counts after 50, 100, 200, 400, 800 and 1200 generations) in
`P.armies.tsv` and `P.matches.csv`; logging does not change the training.
`life_arena raster --league FILE --log-matches P` adds the armies' cell
bitmaps. `tools/surrogate/study.py --prefix P` (PyTorch) then measures how well
a learnt model and shortened battles predict the outcome on the newest battles,
in terms that matter for the search: rank correlation inside a search
generation, how often the best candidate survives a pre-selection of the top
quarter, and how near-identical armies are told apart. Logs go to `data/`,
which is not committed.

## Exploiting the challenges (`life_challenge`)

How few red cells does it take to beat a Challenge bot? `life_challenge` plays
the level exactly like the browser does (same board, bot, Birthright, no
generation limit, home cells not counted; one deterministic battle decides) and
searches over sets of red cells, since cells are what the record counts and any
set can be drawn. It never sees the records players set.

```bash
./build/life_challenge --level hive --minutes 2           # stochastic search -> data/challenge/hive.txt
./build/life_challenge --level hive --exhaustive 4        # every army of up to 4 cells
./build/life_challenge --level hive --verify 'cell@46,65,r0,m0;...'   # one army, both engines
./build/life_challenge --level hive --verify '...' --vs-league assets/leagues/hive-260x128.txt   # against the whole league
```

- **Search**: random armies of arsenal pieces until some win, then an archive of
  winners per cell count is mutated, mostly towards fewer cells (delete a
  cell or a cluster, two cells for one, a cluster for a smaller pattern) but
  also sideways (move a cell, shift a cluster or the army) because the game is
  chaotic: a same-size neighbour of a winner is often where the next smaller
  winner is. Every new record gets a sweep over all its single-cell deletions.
- **Exhaustive**: cells more than 2 apart from all others in groups of one or
  two die at once without effect, and larger groups first evolve as if alone,
  so checking every *linked* group (cells at most 2 apart) of up to K cells at
  every position covers all armies of up to K cells. Placements that look the
  same after 30 generations while still far from everything else play the same
  battle, which brings ~21 million placements down to ~78,000 battles.

The output lists the winners with their LD1 army code (import under **Armies →
My armies**). Battles still running after `--cap` generations (30000) count as
lost; none did on the Hive.

**Hive lattice**: the search found a 5-cell winner within 30 seconds
(about 9,000 battles). The exhaustive check shows that no army of 3 or 4
cells wins (and 1 or 2 cells die at once), so **5 cells is the minimum**.
It is a pure exploit of that one bot: against the rest of the Hive league it
loses every army with a Nash weight except #103 (9.7% of the mixture under
Challenge rules, 0% and a payoff of −0.84 under the league's own rules).
*No man's land* and *Champion* fall to a single blinker (3 cells, the least
any army can have) in a well-chosen spot, found within seconds: those bots
are chaotic enough that the smallest nudge tips the outcome.

## Layout

```
core/       gol_core library
  life.*      classic B3/S23 grid (torus or bounded), with cell ages for colouring
  patterns.*  RLE parser, pattern library, battle arsenal and prices
  battle.*    coloured battle rules, end detection, battlefield presets, board encoding
  fast_battle.*  the same rules bit-parallel, for bulk simulation
classic/    life_classic (SFML) + a tiny immediate-mode widget layer
battle/     life_battle: HTTP server + JSON API (api_json.* is shared with the WebAssembly engine)
  wasm_engine.cpp  the engine for the static site (Emscripten target life_duel_engine)
  web/        index.html, style.css, app.js (no build step; PeerJS is loaded from a CDN for online play)
    online.js         online play: PeerJS connection, commit-reveal, checksums, rematch
    armies.js         saved armies (names, IDs, army codes) and publishing through GitHub issues
    levels.js         Solo and Challenge: the levels and the records
    static-backend.js, engine-worker.js   the /api/ calls answered in the browser (static site only)
tools/      build_site.sh: WebAssembly build + static site in dist/
.github/workflows/pages.yml   CI: native build + tests, static site, GitHub Pages deploy
arena/      life_arena: armies, mutation, matches, Nash solver, league and twin training
  challenge.cpp  life_challenge: fewest cells that beat a Challenge bot (search and exhaustive check)
tests/      core_tests, arena_tests; web/ browser tests (Playwright scripts, usage in their headers)
assets/     DejaVu fonts for the native GUI, leagues/ with the bot for vs Bot mode
```

### Battle server API

| Endpoint | Purpose |
|---|---|
| `GET /api/config` | arsenal (patterns + costs), battlefield presets, board sizes |
| `GET /api/battlefield?w&h&preset&count&seed` | generated grey battlefield |
| `POST /api/battle?w&h&maxgen&variant&home&solo` (body = board) | start a battle, returns the first frame (`maxgen=0`: no limit; `home=0`: cells in their own starting area don't count, the frames then also carry `redHome`/`blueHome`; `solo=1`: Red plays alone, only Red dying out ends the battle early) |
| `GET /api/step?id&n=K` or `&ms=T` | advance K generations, or as many as fit into T ms; `board=0` omits the board |
| `GET /api/bots` | the bot leagues on offer: `{"bots":[{"id"}]}` |
| `GET /api/bot?id` | that league's JSON export, verbatim (settings, armies with Nash weights and red/blue placements) |

Boards are sent as run-length text: `.` dead, `r` red, `b` blue, `g` grey, each
optionally prefixed with a repeat count (`12.3r.g`).

## License

MIT, see [LICENSE](LICENSE). The DejaVu fonts in `assets/` keep their own free
licence (Bitstream Vera / DejaVu).
